// package.json + package-lock.json parsing + the pin/completeness gate (see lockfile.h).

#include "context/editor/pkg/lockfile.h"

#include "context/editor/contract/json.h"
#include "context/editor/pkg/codes.h"

#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace context::editor::pkg
{
namespace
{

using context::editor::contract::Json;

// A numeric SemVer identifier: one or more ASCII digits, no leading zero unless it is exactly "0".
bool is_numeric_identifier(std::string_view s)
{
    if (s.empty())
        return false;
    for (char c : s)
        if (c < '0' || c > '9')
            return false;
    return s.size() == 1 || s[0] != '0';
}

// The name after the final "node_modules/" segment of an install path. Empty when the path has no
// such segment (an unexpected shape we reject upstream).
std::string name_from_install_path(const std::string& path)
{
    const std::string marker = "node_modules/";
    const std::size_t pos = path.rfind(marker);
    if (pos == std::string::npos)
        return {};
    return path.substr(pos + marker.size());
}

LockfileParse fail(LockParseStatus status, std::string_view code, std::string message,
                   std::string offending)
{
    LockfileParse out;
    out.status = status;
    out.error_code = std::string(code);
    out.message = std::move(message);
    out.offending = std::move(offending);
    return out;
}

} // namespace

bool is_exact_version(std::string_view spec)
{
    if (spec.empty())
        return false;
    // Strip a build-metadata suffix "+..." (does not affect precedence, allowed on an exact pin).
    std::string_view core = spec;
    if (const std::size_t plus = core.find('+'); plus != std::string_view::npos)
    {
        if (plus + 1 >= core.size())
            return false; // trailing '+' with no metadata
        core = core.substr(0, plus);
    }
    // Split off an optional "-prerelease".
    std::string_view prerelease;
    if (const std::size_t dash = core.find('-'); dash != std::string_view::npos)
    {
        prerelease = core.substr(dash + 1);
        core = core.substr(0, dash);
        if (prerelease.empty())
            return false;
    }
    // core must be exactly major.minor.patch, each a numeric identifier.
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= core.size(); ++i)
    {
        if (i == core.size() || core[i] == '.')
        {
            parts.push_back(core.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.size() != 3)
        return false;
    for (std::string_view part : parts)
        if (!is_numeric_identifier(part))
            return false;
    // Each dot-separated prerelease identifier must be a non-empty alphanumeric/hyphen run; a purely
    // numeric one must not have a leading zero (SemVer §9). This is validation, not full ordering.
    if (!prerelease.empty())
    {
        std::size_t pstart = 0;
        for (std::size_t i = 0; i <= prerelease.size(); ++i)
        {
            if (i == prerelease.size() || prerelease[i] == '.')
            {
                const std::string_view id = prerelease.substr(pstart, i - pstart);
                if (id.empty())
                    return false;
                bool all_digits = true;
                for (char c : id)
                {
                    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                                    (c >= 'A' && c <= 'Z') || c == '-';
                    if (!ok)
                        return false;
                    if (c < '0' || c > '9')
                        all_digits = false;
                }
                if (all_digits && !is_numeric_identifier(id))
                    return false;
                pstart = i + 1;
            }
        }
    }
    return true;
}

LockfileParse parse_lockfile(std::string_view package_json, std::string_view package_lock_json)
{
    Json manifest;
    Json lock;
    try
    {
        manifest = Json::parse(std::string(package_json));
        lock = Json::parse(std::string(package_lock_json));
    }
    catch (const std::exception& e)
    {
        return fail(LockParseStatus::Malformed, kInstallLockfileIncompleteCode,
                    std::string("package.json / package-lock.json is not well-formed JSON: ") +
                        e.what(),
                    {});
    }

    if (!manifest.is_object() || !lock.is_object())
        return fail(LockParseStatus::Malformed, kInstallLockfileIncompleteCode,
                    "package.json and package-lock.json must each be a JSON object.", {});

    // v3 lockfile: a `packages` object keyed by install path. (lockfileVersion must be >= 2/3; v1's
    // `dependencies`-only shape is not accepted — the engine requires the flattened, integrity-
    // bearing `packages` map.)
    if (!lock.at("packages").is_object())
        return fail(LockParseStatus::Malformed, kInstallLockfileIncompleteCode,
                    "package-lock.json has no `packages` map (npm lockfileVersion >= 2 required).",
                    {});
    const Json& packages = lock.at("packages");

    // 1) Every root dependency + devDependency spec must be an exact pin.
    struct RootDep
    {
        std::string name;
        std::string spec;
        bool dev;
    };
    std::vector<RootDep> root_deps;
    for (const char* field : {"dependencies", "devDependencies"})
    {
        const Json& deps = manifest.at(field);
        if (deps.is_null())
            continue;
        if (!deps.is_object())
            return fail(LockParseStatus::Malformed, kInstallLockfileIncompleteCode,
                        std::string("package.json `") + field + "` must be an object.", {});
        const bool is_dev = std::string(field) == "devDependencies";
        for (const auto& [dep_name, spec_json] : deps.object_members())
        {
            if (!spec_json.is_string())
                return fail(LockParseStatus::Malformed, kInstallLockfileIncompleteCode,
                            "dependency spec must be a string.", dep_name);
            const std::string& spec = spec_json.as_string();
            if (!is_exact_version(spec))
                return fail(LockParseStatus::VersionUnpinned, kInstallVersionUnpinnedCode,
                            "engine-driven installs require an exact pinned version; '" + dep_name +
                                "' is specified as '" + spec + "' (a range / dist-tag / url).",
                            dep_name + "@" + spec);
            root_deps.push_back({dep_name, spec, is_dev});
        }
    }

    // 2) + 3) Walk the lock's `packages`. Validate every non-root entry has an exact version +
    // non-empty integrity (transitive included), and index by name for the root cross-check.
    std::vector<ResolvedPackage> resolved;
    for (const auto& [path, entry] : packages.object_members())
    {
        if (path.empty())
            continue; // the root project entry ("") — not an installed dependency
        if (!entry.is_object())
            return fail(LockParseStatus::Malformed, kInstallLockfileIncompleteCode,
                        "a package-lock `packages` entry is not an object.", path);
        // Link/workspace entries (a `link`/`resolved`-to-local) are out of v1 scope; require a
        // registry-shaped entry with version + integrity.
        const std::string name = name_from_install_path(path);
        if (name.empty())
            return fail(LockParseStatus::Malformed, kInstallLockfileIncompleteCode,
                        "a lock entry install path is not under node_modules/.", path);
        const Json& version = entry.at("version");
        const Json& integrity = entry.at("integrity");
        if (!version.is_string() || version.as_string().empty())
            return fail(LockParseStatus::Incomplete, kInstallLockfileIncompleteCode,
                        "lock entry '" + path + "' has no version; the graph is not fully pinned.",
                        path);
        if (!is_exact_version(version.as_string()))
            return fail(LockParseStatus::VersionUnpinned, kInstallVersionUnpinnedCode,
                        "lock entry '" + path + "' has a non-exact version '" +
                            version.as_string() + "'.",
                        path);
        if (!integrity.is_string() || integrity.as_string().empty())
            return fail(LockParseStatus::Incomplete, kInstallLockfileIncompleteCode,
                        "lock entry '" + path +
                            "' has no integrity; every artifact must be integrity-pinned "
                            "(incl. transitive).",
                        path);

        ResolvedPackage pkg;
        pkg.name = name;
        pkg.install_path = path;
        pkg.version = version.as_string();
        pkg.resolved = entry.at("resolved").is_string() ? entry.at("resolved").as_string() : "";
        pkg.integrity = integrity.as_string();
        pkg.has_install_script = entry.at("hasInstallScript").as_bool();
        pkg.dev = entry.at("dev").as_bool();
        resolved.push_back(std::move(pkg));
    }

    // 4) Every root dependency must resolve to its exact spec at node_modules/<name> with integrity.
    for (const RootDep& dep : root_deps)
    {
        const std::string expected_path = "node_modules/" + dep.name;
        const ResolvedPackage* match = nullptr;
        for (const ResolvedPackage& pkg : resolved)
            if (pkg.install_path == expected_path)
            {
                match = &pkg;
                break;
            }
        if (match == nullptr)
            return fail(LockParseStatus::Incomplete, kInstallLockfileIncompleteCode,
                        "declared dependency '" + dep.name +
                            "' is absent from the lockfile `packages` map.",
                        dep.name);
        if (match->version != dep.spec)
            return fail(LockParseStatus::Incomplete, kInstallLockfileIncompleteCode,
                        "declared dependency '" + dep.name + "' pins '" + dep.spec +
                            "' but the lock resolves '" + match->version + "'.",
                        dep.name);
    }

    LockfileParse out;
    out.status = LockParseStatus::Ok;
    out.packages = std::move(resolved);
    return out;
}

} // namespace context::editor::pkg
