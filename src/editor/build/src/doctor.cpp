// Environment doctor core (see doctor.h). Pure, deterministic, total — no filesystem / subprocess /
// environment probing (that is the CLI wrapper's job).

#include "context/editor/build/doctor.h"

#include <string>
#include <string_view>
#include <vector>

namespace context::editor::build
{

namespace
{

// Parse a version string into its leading numeric components: "20.1.2" -> {20,1,2}; "v20.11" -> {20,11}
// (a leading non-digit run is skipped); "20.1-rc" -> {20,1} (stops at the first non-digit after a dot).
std::vector<long long> numeric_components(std::string_view text)
{
    std::vector<long long> out;
    std::size_t i = 0;
    // Skip a leading non-digit prefix (e.g. node's "v", "cmake version ").
    while (i < text.size() && (text[i] < '0' || text[i] > '9'))
        ++i;
    long long current = 0;
    bool have_digit = false;
    for (; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c >= '0' && c <= '9')
        {
            current = current * 10 + (c - '0');
            have_digit = true;
        }
        else if (c == '.')
        {
            if (!have_digit)
                break;
            out.push_back(current);
            current = 0;
            have_digit = false;
        }
        else
        {
            break; // any other char ends the numeric version (e.g. "-rc", " (clang-…)")
        }
    }
    if (have_digit)
        out.push_back(current);
    return out;
}

// Fetchable vs preinstalled for a manifest compiler id (R-BUILD-008 / R-PKG-002 §400): mainline clang
// (Linux) and the Emscripten LLVM (web, via emsdk) are engine-fetched; the MSVC STL / Windows SDK and
// Apple clang / Xcode are dev-preinstalled prerequisites (licensed with VS / Xcode).
Acquisition compiler_acquisition(std::string_view compiler)
{
    if (compiler == "clang" || compiler == "emscripten-clang")
        return Acquisition::Fetchable;
    return Acquisition::Preinstalled; // msvc, apple-clang
}

std::string docs_anchor(std::string_view fragment)
{
    return "docs/toolchain-bootstrap.md#" + std::string(fragment);
}

} // namespace

std::string_view acquisition_name(Acquisition a) noexcept
{
    return a == Acquisition::Fetchable ? "fetchable" : "preinstalled";
}

const ToolProbe* EnvironmentProbe::find_tool(std::string_view name) const
{
    for (const ToolProbe& t : tools)
        if (t.name == name)
            return &t;
    return nullptr;
}

const SigningProbe* EnvironmentProbe::find_signing(std::string_view target,
                                                   std::string_view requirement) const
{
    for (const SigningProbe& s : signing)
        if (s.target == target && s.requirement == requirement)
            return &s;
    return nullptr;
}

std::size_t DoctorReport::blocking_count() const
{
    std::size_t n = 0;
    for (const ComponentFinding& c : components)
        if (c.blocking)
            ++n;
    return n;
}

std::vector<std::string> signing_requirements(std::string_view build_target)
{
    // The v1 desktop-signing legs (R-BUILD-005): Windows Authenticode, macOS Developer ID + notarization.
    // Linux / Web have no v1 code-signing prerequisite.
    if (build_target == "windows")
        return {"authenticode"};
    if (build_target == "macos")
        return {"developer-id-notarization"};
    return {};
}

std::vector<ComponentRequirement>
component_requirements(std::string_view build_target, const std::vector<ToolchainEntry>& manifest)
{
    std::vector<ComponentRequirement> reqs;
    if (toolchain_target_key(build_target).empty())
        return reqs; // unknown build target — the CLI rejects it before diagnose

    // The compiler component, resolved from the L-42 per-target manifest (pin + enforcement).
    if (const ToolchainEntry* tc = resolve_toolchain(manifest, build_target); tc != nullptr)
    {
        ComponentRequirement compiler;
        compiler.component = tc->compiler;
        compiler.role = "compiler";
        compiler.acquisition = compiler_acquisition(tc->compiler);
        compiler.required_version = tc->pin;
        compiler.enforcement = tc->enforcement;
        compiler.remediation_pointer = docs_anchor(build_target);
        reqs.push_back(std::move(compiler));
    }

    // CMake — the build-system prerequisite, presence-only, every target (docs/toolchain-bootstrap.md).
    reqs.push_back({"cmake", "build-system", Acquisition::Preinstalled, /*required_version=*/"",
                    /*enforcement=*/"documented", docs_anchor("common")});

    // Node.js — the TS-tier authoring prerequisite (R-VER-003), preinstalled, every target. The embedded
    // runtime VM is V8-direct (L-61), NOT Node — Node exists only at dev/build time.
    reqs.push_back({"node", "js-toolchain", Acquisition::Preinstalled, /*required_version=*/"",
                    /*enforcement=*/"documented", docs_anchor("common")});
    return reqs;
}

bool version_satisfies(std::string_view required, std::string_view found)
{
    if (required.empty())
        return true; // presence-only prerequisite
    const std::vector<long long> req = numeric_components(required);
    const std::vector<long long> fnd = numeric_components(found);
    if (req.empty())
        return true;
    if (fnd.size() < req.size())
        return false;
    for (std::size_t i = 0; i < req.size(); ++i)
        if (fnd[i] != req[i])
            return false;
    return true;
}

DoctorReport diagnose(const std::vector<std::string>& targets,
                      const std::vector<ToolchainEntry>& manifest, const EnvironmentProbe& probe)
{
    DoctorReport report;
    report.targets = targets;

    // --- per-target toolchain component checks (R-BUILD-008 fetchable-vs-preinstalled) --------------
    for (const std::string& target : targets)
    {
        for (const ComponentRequirement& req : component_requirements(target, manifest))
        {
            ComponentFinding f;
            f.target = target;
            f.component = req.component;
            f.role = req.role;
            f.acquisition = std::string(acquisition_name(req.acquisition));
            f.required_version = req.required_version;
            f.enforcement = req.enforcement;
            f.remediation_pointer = req.remediation_pointer;
            f.fetchable = req.acquisition == Acquisition::Fetchable;
            // v1 can OFFER a fetch for a fetchable component; the live fetch-and-verify runs through the
            // a08-verified path (docs/signing.md). can_fetch_now marks the offer as machine-branchable.
            f.can_fetch_now = f.fetchable;

            const ToolProbe* probe_tool = probe.find_tool(req.component);
            if (probe_tool == nullptr || !probe_tool->present)
            {
                f.status = "missing";
                f.blocking = true; // a required component absent — the target cannot be built
                f.code = std::string(kDoctorToolchainMissingCode);
                f.remediation =
                    f.fetchable
                        ? "engine-fetch '" + req.component +
                              "' via the verified-fetch path (fail-closed, R-SEC-009), or install it"
                        : "install the dev-preinstalled prerequisite '" + req.component + "'";
            }
            else
            {
                f.found_version = probe_tool->version;
                if (version_satisfies(req.required_version, probe_tool->version))
                {
                    f.status = "ok";
                }
                else
                {
                    f.status = "version_mismatch";
                    f.code = std::string(kDoctorToolchainVersionMismatchCode);
                    f.remediation = "provide '" + req.component + "' version " + req.required_version +
                                    " (found '" +
                                    (probe_tool->version.empty() ? std::string("unknown")
                                                                 : probe_tool->version) +
                                    "')";
                    // L-42 drift-alarm: strict pins block; advisory/documented drift is a warning only.
                    f.blocking = req.enforcement == "strict";
                    if (!f.blocking)
                        report.warnings.push_back("advisory toolchain version drift: " + f.component +
                                                  " for target " + target + " — " + f.remediation);
                }
            }
            report.components.push_back(std::move(f));
        }
    }

    // --- file-sync OS resource budget (R-FILE-002 / R-FILE-011): the up-front watcher.degraded check --
    FileSyncFinding& fs = report.filesync;
    fs.project_file_count = probe.filesync.project_file_count;
    fs.worktree_daemon_count = probe.filesync.worktree_daemon_count;
    fs.watch_limit = probe.filesync.watch_limit;
    fs.fd_limit = probe.filesync.fd_limit;
    fs.remediation_pointer = docs_anchor("file-sync-budget");
    if (fs.project_file_count >= 0 && fs.worktree_daemon_count > 0)
        fs.required_watches = fs.project_file_count * fs.worktree_daemon_count;
    if (fs.watch_limit < 0 || fs.required_watches < 0)
    {
        fs.status = "unknown"; // the limit is not probeable on this host (e.g. non-Linux)
    }
    else if (fs.required_watches > fs.watch_limit)
    {
        fs.status = "degraded";
        fs.code = std::string(kDoctorFileSyncBudgetLowCode);
        fs.remediation = "raise the per-user watch limit to >= " + std::to_string(fs.required_watches) +
                         " (project files × worktree daemons), or expect the watcher.degraded "
                         "background-crawl fallback (R-FILE-002)";
        report.warnings.push_back("file-sync budget low: " + fs.remediation);
    }
    else
    {
        fs.status = "ok";
    }

    // --- signing prerequisites (R-BUILD-005 / R-BUILD-008): presence only, NEVER a secret value -------
    for (const std::string& target : targets)
    {
        for (const std::string& requirement : signing_requirements(target))
        {
            SigningFinding sf;
            sf.target = target;
            sf.requirement = requirement;
            sf.remediation_pointer = docs_anchor("signing-prereqs");
            const SigningProbe* sp = probe.find_signing(target, requirement);
            if (sp == nullptr || !sp->known)
            {
                sf.status = "unknown"; // the check could not run on this host
            }
            else if (sp->configured)
            {
                sf.status = "configured";
            }
            else
            {
                sf.status = "absent";
                sf.code = std::string(kDoctorSigningPrereqAbsentCode);
                sf.remediation = "configure the '" + requirement + "' signing prerequisite for target " +
                                 target + " (a ship-time prereq, not a build blocker)";
                report.warnings.push_back("signing prerequisite absent: " + sf.remediation);
            }
            report.signing.push_back(std::move(sf));
        }
    }

    report.ok = report.blocking_count() == 0;
    return report;
}

} // namespace context::editor::build
