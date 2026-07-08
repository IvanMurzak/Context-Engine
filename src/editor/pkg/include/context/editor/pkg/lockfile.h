// package.json + package-lock.json (npm v3) parsing + the R-SEC-005 pin/completeness gate.
//
// The npm v3 lockfile flattens the ENTIRE resolved dependency graph — direct AND transitive — into
// one `packages` map keyed by install path, each entry carrying the exact `version`, the `resolved`
// source, the `integrity` SRI, and `hasInstallScript`. That makes it the authoritative supply-chain
// input: the engine enforces exact pins + a complete integrity-bearing lock over every entry
// (transitive included) before a byte is fetched, and reads `hasInstallScript` as the native-tier
// classification signal (npm_install.h applies the L-49 gate).

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace context::editor::pkg
{

// One resolved package from the lockfile (a non-root `packages` entry).
struct ResolvedPackage
{
    std::string name;          // derived from the install path (after the last "node_modules/")
    std::string install_path;  // project-relative, e.g. "node_modules/left-pad"
    std::string version;       // the exact resolved version
    std::string resolved;      // the artifact source (tarball URL / offline key)
    std::string integrity;     // the SRI string (never empty after a successful parse)
    bool has_install_script = false; // the native-tier classification signal (L-49)
    bool dev = false;          // a dev-only dependency (filtered by --production)
};

enum class LockParseStatus
{
    Ok,
    Malformed,        // a package.json / lockfile that is not well-formed or not v3-shaped
    VersionUnpinned,  // a root dependency spec is not an exact pinned version
    Incomplete,       // a dependency missing from the lock, or a lock entry lacking version/integrity
};

struct LockfileParse
{
    LockParseStatus status = LockParseStatus::Ok;
    std::string error_code; // "" on Ok, else a codes.h install.* code
    std::string message;
    std::string offending;  // the package/spec at fault (for the diagnostic)
    std::vector<ResolvedPackage> packages; // every non-root entry, install-path order
};

// Parse + validate. Enforces (fail-closed): every root dependency/devDependency spec is an exact
// pin; every root dependency resolves in the lock to that exact version with a non-empty integrity;
// and every non-root lock entry (transitive included) carries an exact version + non-empty
// integrity. On the first failure it stops with the matching install.* code.
[[nodiscard]] LockfileParse parse_lockfile(std::string_view package_json,
                                           std::string_view package_lock_json);

// True when `spec` is an exact SemVer pin: three dot-separated numeric identifiers with an optional
// `-prerelease` and/or `+build` (e.g. "1.2.3", "1.2.3-rc.1", "1.0.0+build.5"). Ranges ("^1.0.0",
// "~1", ">=1.0.0", "1.x", "*"), dist-tags ("latest"), and url/git specs are NOT exact.
[[nodiscard]] bool is_exact_version(std::string_view spec);

} // namespace context::editor::pkg
