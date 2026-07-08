// The engine-driven install orchestrator (see npm_install.h).

#include "context/editor/pkg/npm_install.h"

#include "context/editor/pkg/codes.h"
#include "context/editor/pkg/integrity.h"
#include "context/editor/pkg/tar.h"

#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

namespace context::editor::pkg
{
namespace
{

namespace fs = std::filesystem;

// Reject a package-relative entry path that could escape the package directory (R-SEC-008 spirit):
// an absolute path or any ".." segment. "." segments and empty segments are dropped.
bool sanitize_relpath(const std::string& raw, std::string& out)
{
    if (raw.empty())
        return false;
    if (raw.front() == '/' || raw.front() == '\\')
        return false;
    if (raw.size() >= 2 && raw[1] == ':') // a Windows drive-absolute path
        return false;
    std::vector<std::string> segments;
    std::string seg;
    for (char c : raw)
    {
        if (c == '/' || c == '\\')
        {
            if (!seg.empty())
            {
                if (seg == "..")
                    return false;
                if (seg != ".")
                    segments.push_back(seg);
                seg.clear();
            }
        }
        else
        {
            seg.push_back(c);
        }
    }
    if (!seg.empty())
    {
        if (seg == "..")
            return false;
        if (seg != ".")
            segments.push_back(seg);
    }
    if (segments.empty())
        return false;
    out.clear();
    for (std::size_t i = 0; i < segments.size(); ++i)
    {
        if (i != 0)
            out.push_back('/');
        out += segments[i];
    }
    return true;
}

// Strip npm's conventional leading "package/" directory prefix from a tarball entry path.
std::string strip_package_prefix(const std::string& path)
{
    const std::string prefix = "package/";
    if (path.rfind(prefix, 0) == 0)
        return path.substr(prefix.size());
    return path;
}

InstallOutcome outcome_fail(InstallStatus status, std::string_view code, std::string message,
                            std::string offending)
{
    InstallOutcome out;
    out.status = status;
    out.error_code = std::string(code);
    out.message = std::move(message);
    out.offending = std::move(offending);
    return out;
}

// Extract a verified ustar archive into `dest_dir`, writing files verbatim and running NOTHING
// (structural --ignore-scripts). Returns false on a path-jail violation or a filesystem error.
bool extract_into(const std::vector<TarEntry>& entries, const fs::path& dest_dir, std::string& err)
{
    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    if (ec)
    {
        err = "could not create " + dest_dir.string() + ": " + ec.message();
        return false;
    }
    for (const TarEntry& entry : entries)
    {
        const std::string stripped = strip_package_prefix(entry.path);
        if (stripped.empty())
            continue; // the bare "package/" directory entry
        std::string rel;
        if (!sanitize_relpath(stripped, rel))
        {
            err = "archive entry escapes the package directory: " + entry.path;
            return false; // fail-closed (R-SEC-008)
        }
        const fs::path target = dest_dir / fs::path(rel);
        if (entry.is_dir)
        {
            fs::create_directories(target, ec);
            if (ec)
            {
                err = "could not create directory " + target.string() + ": " + ec.message();
                return false;
            }
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        if (ec)
        {
            err = "could not create " + target.parent_path().string() + ": " + ec.message();
            return false;
        }
        std::ofstream os(target, std::ios::binary | std::ios::trunc);
        if (!os)
        {
            err = "could not write " + target.string();
            return false;
        }
        os.write(entry.data.data(), static_cast<std::streamsize>(entry.data.size()));
        if (!os)
        {
            err = "short write to " + target.string();
            return false;
        }
    }
    return true;
}

} // namespace

InstallPlan plan_install(std::string_view package_json, std::string_view package_lock_json)
{
    const LockfileParse parsed = parse_lockfile(package_json, package_lock_json);
    if (parsed.status != LockParseStatus::Ok)
    {
        InstallPlan plan;
        plan.status = PlanStatus::Rejected;
        plan.error_code = parsed.error_code;
        plan.message = parsed.message;
        plan.offending = parsed.offending;
        return plan;
    }

    InstallPlan plan;
    plan.status = PlanStatus::Ok;
    for (const ResolvedPackage& pkg : parsed.packages)
    {
        PlannedPackage planned;
        planned.pkg = pkg;
        planned.tier = pkg.has_install_script ? TrustTier::Native : TrustTier::Sandbox;
        plan.packages.push_back(std::move(planned));
    }
    return plan;
}

InstallOutcome execute_install(const InstallPlan& plan, PackageSource& source,
                               const std::string& project_root, const InstallOptions& opts)
{
    if (plan.status != PlanStatus::Ok)
        return outcome_fail(InstallStatus::Rejected, plan.error_code, plan.message, plan.offending);

    // Gate the WHOLE plan before extracting a byte: a native-tier package requires the L-49 consent
    // grant, which v1 never carries (R-SEC-001). Refuse fail-closed with the specific classification
    // code; the reserved R-SEC-011 `consent_required` code names the general protocol.
    for (const PlannedPackage& planned : plan.packages)
    {
        if (opts.production && planned.pkg.dev)
            continue;
        if (planned.tier == TrustTier::Native && !opts.allow_native)
            return outcome_fail(
                InstallStatus::ConsentRequired, kInstallScriptsRequiredCode,
                "package '" + planned.pkg.name + "@" + planned.pkg.version +
                    "' requires install scripts and is classified native-tier; engine-driven "
                    "installs never run lifecycle scripts, so it is refused pending the L-49 "
                    "consent gate (" + std::string(kConsentRequiredCode) + ", R-SEC-011).",
                planned.pkg.name + "@" + planned.pkg.version);
    }

    InstallOutcome out;
    for (const PlannedPackage& planned : plan.packages)
    {
        if (opts.production && planned.pkg.dev)
            continue;
        // Only sandbox-tier packages reach here (natives already short-circuited above).
        const std::optional<std::string> bytes = source.fetch(planned.pkg);
        if (!bytes.has_value())
            return outcome_fail(InstallStatus::FetchFailed, kInstallLockfileIncompleteCode,
                                "could not fetch artifact for '" + planned.pkg.name + "@" +
                                    planned.pkg.version + "' from the package source.",
                                planned.pkg.name + "@" + planned.pkg.version);

        const SriVerification sri = verify_integrity(planned.pkg.integrity, *bytes);
        if (sri.result != SriResult::Ok)
            return outcome_fail(
                InstallStatus::IntegrityMismatch, kInstallIntegrityMismatchCode,
                "artifact for '" + planned.pkg.name + "@" + planned.pkg.version +
                    "' failed integrity verification (" +
                    (sri.result == SriResult::Mismatch
                         ? "digest mismatch"
                         : sri.result == SriResult::UnsupportedAlgorithm
                               ? "no verifiable hash algorithm in the SRI"
                               : "malformed SRI") +
                    "); the artifact is refused, never used with a warning (R-SEC-009).",
                planned.pkg.name + "@" + planned.pkg.version);

        const std::optional<std::vector<TarEntry>> entries = tar_read(*bytes);
        if (!entries.has_value())
            return outcome_fail(InstallStatus::IntegrityMismatch, kInstallIntegrityMismatchCode,
                                "verified artifact for '" + planned.pkg.name + "@" +
                                    planned.pkg.version + "' is not a valid ustar archive.",
                                planned.pkg.name + "@" + planned.pkg.version);

        const fs::path dest = fs::path(project_root) / fs::path(planned.pkg.install_path);
        std::string err;
        if (!extract_into(*entries, dest, err))
            return outcome_fail(InstallStatus::IntegrityMismatch, kInstallIntegrityMismatchCode,
                                "could not extract '" + planned.pkg.name + "@" +
                                    planned.pkg.version + "': " + err,
                                planned.pkg.name + "@" + planned.pkg.version);

        out.installed.push_back({planned.pkg.name, planned.pkg.version, planned.pkg.install_path});
    }

    out.status = InstallStatus::Ok;
    return out;
}

} // namespace context::editor::pkg
