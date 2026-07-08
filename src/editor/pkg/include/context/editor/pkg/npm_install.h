// The R-SEC-005 engine-driven install path: plan (validate + classify) then execute (fetch + verify
// + extract-without-scripts). This is the security envelope, not a full npm client — a pluggable
// PackageSource yields artifact bytes so the invariants are TESTED hermetically. The security
// guarantees, all fail-closed:
//   * --ignore-scripts in ALL tiers, BY CONSTRUCTION — extraction writes files verbatim and never
//     executes anything, so a package's lifecycle scripts can never run from an engine-driven
//     install (no code path invokes a shell/subprocess).
//   * pinned versions + lockfile integrity enforced incl. transitive (lockfile.h), and each fetched
//     artifact's bytes verified against its SRI before use (integrity.h); a mismatch is refused.
//   * a package that requires install scripts is classified native-tier and trips the L-49 consent
//     gate (v1 grants no native consent — R-SEC-001 — so it is refused with consent metadata).

#pragma once

#include "context/editor/pkg/lockfile.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::pkg
{

// L-49 trust tiers. Sandbox = the genuinely-sandboxable TS/WASM tier (the default). Native = a
// package whose install scripts / native code cannot be honestly sandboxed (classified from the
// lockfile's hasInstallScript); a native-tier action is a human-approval boundary (L-49 / R-SEC-011).
enum class TrustTier
{
    Sandbox,
    Native,
};

struct PlannedPackage
{
    ResolvedPackage pkg;
    TrustTier tier = TrustTier::Sandbox;
};

enum class PlanStatus
{
    Ok,
    Rejected, // a pin/completeness failure surfaced during validation (error_code is set)
};

struct InstallPlan
{
    PlanStatus status = PlanStatus::Ok;
    std::string error_code; // "" on Ok, else a codes.h install.* code
    std::string message;
    std::string offending;
    std::vector<PlannedPackage> packages; // every resolved package + its classified tier
};

// Pure: parse + validate the manifest/lockfile and classify each package's trust tier. A
// pin/completeness failure returns Rejected with the lockfile's install.* code; otherwise Ok with
// every package listed (native-tier packages ARE listed — classification is a fact; the L-49 gate
// is applied by execute_install, not here).
[[nodiscard]] InstallPlan plan_install(std::string_view package_json,
                                       std::string_view package_lock_json);

// The pluggable artifact source: yields the raw ustar archive bytes for a resolved package.
// std::nullopt = a fetch failure (fail-closed). v1 ships an offline directory source (CLI layer);
// the TLS/cert-pinned live-registry + gzip `.tgz` fetcher is a documented follow-up seam.
class PackageSource
{
public:
    virtual ~PackageSource() = default;
    [[nodiscard]] virtual std::optional<std::string> fetch(const ResolvedPackage& pkg) = 0;
};

struct InstallOptions
{
    // The L-49 native-consent grant. v1 has NO path that sets this (R-SEC-001: no third-party native
    // packages), so a native-tier package is always refused — the fail-closed consent gate.
    bool allow_native = false;
    // Skip dev-only dependencies (npm --production / --omit=dev).
    bool production = false;
};

enum class InstallStatus
{
    Ok,
    Rejected,          // the plan itself was rejected (pin/completeness); error_code carried through
    ConsentRequired,   // a native-tier package hit the L-49 gate without a grant (fail-closed)
    FetchFailed,       // the source could not yield an artifact
    IntegrityMismatch, // a fetched artifact failed SRI verification / was not a valid archive
};

struct InstalledPackage
{
    std::string name;
    std::string version;
    std::string install_path;
};

struct InstallOutcome
{
    InstallStatus status = InstallStatus::Ok;
    std::string error_code; // "" on Ok
    std::string message;
    std::string offending;
    std::vector<InstalledPackage> installed; // sandbox-tier packages actually extracted
};

// Execute a plan under `project_root`. GATES THE WHOLE PLAN before extracting a byte: any native-
// tier package without opts.allow_native short-circuits to ConsentRequired (nothing is installed).
// Otherwise each sandbox-tier package (respecting --production) is fetched, SRI-verified, and
// extracted into `project_root/<install_path>` WITHOUT running any lifecycle script. The first
// fetch/integrity failure stops the install fail-closed.
[[nodiscard]] InstallOutcome execute_install(const InstallPlan& plan, PackageSource& source,
                                             const std::string& project_root,
                                             const InstallOptions& opts);

} // namespace context::editor::pkg
