// `context install` implementation (see install_command.h).

#include "context/cli/install_command.h"

#include "context/editor/contract/json.h"
#include "context/editor/pkg/npm_install.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace context::cli
{

using editor::contract::Envelope;
using editor::contract::Json;
namespace pkg = editor::pkg;
namespace fs = std::filesystem;

namespace
{

std::optional<std::string> read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// The offline artifact source: resolves each package to `<cache>/<sanitized-name>-<version>.tar`.
// A scoped name's '/' is folded to '+' so `@scope/pkg` maps to a flat filename. This is the v1
// verified fetcher; the live-registry (TLS/cert-pinned, gzip `.tgz`) source is a tracked seam.
class LocalDirSource final : public pkg::PackageSource
{
public:
    explicit LocalDirSource(fs::path cache_dir) : cache_dir_(std::move(cache_dir)) {}

    std::optional<std::string> fetch(const pkg::ResolvedPackage& package) override
    {
        std::string flat = package.name;
        for (char& c : flat)
            if (c == '/')
                c = '+';
        const fs::path artifact = cache_dir_ / (flat + "-" + package.version + ".tar");
        return read_file(artifact);
    }

private:
    fs::path cache_dir_;
};

Json planned_packages_json(const pkg::InstallPlan& plan)
{
    Json arr = Json::array();
    for (const pkg::PlannedPackage& planned : plan.packages)
    {
        Json entry = Json::object();
        entry.set("name", Json(planned.pkg.name));
        entry.set("version", Json(planned.pkg.version));
        entry.set("tier", Json(planned.tier == pkg::TrustTier::Native ? "native" : "sandbox"));
        entry.set("dev", Json(planned.pkg.dev));
        arr.push_back(std::move(entry));
    }
    return arr;
}

} // namespace

Envelope run_install(const std::map<std::string, std::string>& flags)
{
    const std::string project = flags.count("project") ? flags.at("project") : ".";
    const bool dry_run = flags.count("dry-run") != 0;
    const bool production = flags.count("production") != 0;

    const fs::path project_root(project);
    const std::optional<std::string> manifest = read_file(project_root / "package.json");
    if (!manifest.has_value())
        return Envelope::failure("file.not_found",
                                 "no package.json under the project root '" + project + "'.");
    const std::optional<std::string> lock = read_file(project_root / "package-lock.json");
    if (!lock.has_value())
        return Envelope::failure("file.not_found",
                                 "no package-lock.json under the project root '" + project +
                                     "' — an engine-driven install requires a pinned lockfile.");

    const pkg::InstallPlan plan = pkg::plan_install(*manifest, *lock);
    if (plan.status != pkg::PlanStatus::Ok)
        return Envelope::failure(plan.error_code, plan.message);

    if (dry_run)
    {
        Json data = Json::object();
        data.set("wouldApply", Json(false));
        data.set("packages", planned_packages_json(plan));
        data.set("note", Json(std::string("dry-run: the plan validated (pins + lockfile integrity "
                                          "+ tier classification); no artifact was fetched.")));
        return Envelope::success(std::move(data));
    }

    if (!flags.count("source"))
        return Envelope::failure(
            "usage.missing_argument",
            "`context install` needs --source <cache-dir> (the offline artifact cache); v1 has no "
            "live-registry fetcher (a tracked seam). Use --dry-run to validate without fetching.");

    LocalDirSource source(fs::path(flags.at("source")));
    pkg::InstallOptions opts;
    opts.production = production;
    opts.allow_native = false; // v1: no third-party native packages (R-SEC-001) — the L-49 gate

    const pkg::InstallOutcome outcome =
        pkg::execute_install(plan, source, project_root.string(), opts);
    if (outcome.status != pkg::InstallStatus::Ok)
        return Envelope::failure(outcome.error_code, outcome.message);

    Json installed = Json::array();
    for (const pkg::InstalledPackage& item : outcome.installed)
    {
        Json entry = Json::object();
        entry.set("name", Json(item.name));
        entry.set("version", Json(item.version));
        entry.set("path", Json(item.install_path));
        installed.push_back(std::move(entry));
    }
    Json data = Json::object();
    data.set("installed", std::move(installed));
    data.set("count", Json(static_cast<std::int64_t>(outcome.installed.size())));
    data.set("ignoredScripts", Json(true)); // --ignore-scripts holds by construction (R-SEC-005)
    return Envelope::success(std::move(data));
}

} // namespace context::cli
