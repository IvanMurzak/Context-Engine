// Environment doctor core (R-BUILD-008, task a09): the pure, deterministic diagnosis spine of
// `context doctor`. `diagnose` compares the components a requested build target NEEDS (the R-PKG-002
// per-target toolchain manifest + the fixed dev prerequisites) against an injected EnvironmentProbe —
// the observed presence/versions of the toolchain, the file-sync OS resource budget, and the signing
// prerequisites — and produces a machine-readable DoctorReport. No filesystem, no subprocess, no
// environment probing lives here: the CLI wrapper (src/cli/doctor_command.cpp) fills the probe from the
// real host; a test fills it from a fixture (the R-QA-011 corpus of deliberately-broken environments).
// This is the exact split build_orchestrator.* / build_command.cpp uses (pure core + IO wrapper), so
// the diagnosis is a total, cache-clean function that the corpus drives to every documented finding.
//
// The doctor.* diagnostic vocabulary is OWNED here (the promote-a-local-string pattern used across the
// codebase — bridge's scope.denied, build_errors.h's kBuild*Code): the contract error catalog
// (src/editor/contract/error_catalog.cpp) registers the SAME literals, so this build module never links
// the contract layer. Keep these strings in lockstep with the catalog's doctor.* block.
//
// SECURITY (R-BUILD-008): signing-prerequisite checks report PRESENCE / REACHABILITY only — a
// SigningProbe carries a boolean, never a secret / key / credential value. The doctor never surfaces a
// secret value anywhere in the report.

#pragma once

#include "context/editor/build/toolchain_manifest.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::build
{

// --- the doctor.* diagnostic vocabulary (registered in the contract catalog by string) --------------

// One or more required toolchain components for a requested target are missing or the wrong version —
// the blocking top-level refusal `context doctor` returns when the environment cannot build the
// requested target(s). Validation-class, deterministic (a bare re-run without fixing the env re-fails).
inline constexpr std::string_view kDoctorEnvironmentIncompleteCode = "doctor.environment_incomplete";

// A required toolchain component is absent for a requested target (a per-finding code carried in the
// report). The finding's `fetchable` flag says whether the fix is an engine-fetch (via the a08-verified
// path) or a dev-preinstall.
inline constexpr std::string_view kDoctorToolchainMissingCode = "doctor.toolchain_missing";

// A required toolchain component is present but its version does not satisfy the L-42 manifest pin (a
// per-finding code). Blocking only when the L-42 enforcement level is `strict`; `advisory`/`documented`
// drift is a non-blocking warning (the drift-alarm, not a build blocker).
inline constexpr std::string_view kDoctorToolchainVersionMismatchCode =
    "doctor.toolchain_version_mismatch";

// The per-user file-sync OS budget (watch handles) is below the project file count × worktree-daemon
// count — the up-front check for the R-FILE-002 `watcher.degraded` path. ADVISORY (never blocking):
// raise the limit, or expect degraded change-detection latency. Pairs with the R-FILE-011 N-daemons
// scenario.
inline constexpr std::string_view kDoctorFileSyncBudgetLowCode = "doctor.filesync_budget_low";

// A code-signing prerequisite for a requested target (Windows Authenticode identity, macOS signing
// identity + notary creds) is not configured/reachable. ADVISORY (a ship-time prereq, never a build
// blocker). Presence only — never a secret value (R-BUILD-008 security constraint).
inline constexpr std::string_view kDoctorSigningPrereqAbsentCode = "doctor.signing_prereq_absent";

// `context doctor --target <t>` named a value that is not a known build target (usage-class).
inline constexpr std::string_view kDoctorUnknownTargetCode = "doctor.unknown_target";

// --- the R-BUILD-008 fetchable-vs-preinstalled split ------------------------------------------------

// How a toolchain component is acquired (R-BUILD-008 / R-PKG-002 §Native engine-package manager):
//   Fetchable    — engine-fetched on demand, signed + verified per R-SEC-009 (mainline clang, Emscripten
//                  LLVM via emsdk). The fetch-now offer routes through the a08-verified fetch path.
//   Preinstalled — a dev prerequisite the engine never fetches: the MSVC STL / Windows SDK (licensed with
//                  Visual Studio), Apple Xcode (macOS), and Node.js for TS-tier authoring (R-VER-003).
enum class Acquisition
{
    Fetchable,
    Preinstalled,
};

// The canonical string form of an Acquisition, for the report + docs.
[[nodiscard]] std::string_view acquisition_name(Acquisition a) noexcept;

// One toolchain component a build target requires (the enumerated R-BUILD-008 split). The compiler's
// required_version / enforcement come from the L-42 manifest; the build-system (cmake) and js-toolchain
// (node) prerequisites are presence-only.
struct ComponentRequirement
{
    std::string component;         // probe key: "clang" | "msvc" | "apple-clang" | "emscripten-clang" |
                                   //            "cmake" | "node"
    std::string role;              // "compiler" | "build-system" | "js-toolchain"
    Acquisition acquisition = Acquisition::Preinstalled;
    std::string required_version;  // the L-42 major.minor pin, or "" for a presence-only prerequisite
    std::string enforcement;       // "strict" | "advisory" | "documented" (L-42 drift-alarm level)
    std::string remediation_pointer; // a docs anchor an agent branches to (docs/toolchain-bootstrap.md#…)
};

// The signing prerequisites a build target needs to SHIP (R-BUILD-005 / R-BUILD-008): the requirement
// ids doctor checks presence for. Empty for targets with no v1 desktop-signing leg (linux/web).
[[nodiscard]] std::vector<std::string> signing_requirements(std::string_view build_target);

// Enumerate every component a build target requires (compiler from `manifest`, plus the fixed cmake +
// node prerequisites). Empty for an unknown build target id. Pure.
[[nodiscard]] std::vector<ComponentRequirement>
component_requirements(std::string_view build_target, const std::vector<ToolchainEntry>& manifest);

// --- the injected environment observation (filled by the CLI from the host; by a test from a fixture) --

// The observed presence + version of one probed tool. version is the extracted numeric version string
// (e.g. "20.1.2"), or "" when the tool ran but no version could be parsed / it is absent.
struct ToolProbe
{
    std::string name;
    bool present = false;
    std::string version;
};

// The observed file-sync OS resource budget (R-FILE-002). A negative field means "unknown / not probed
// on this host" (e.g. the inotify watch limit is Linux-only) — never a blocking finding, only reported.
struct FileSyncProbe
{
    std::int64_t project_file_count = -1;    // files under the project the daemon would watch
    std::int64_t worktree_daemon_count = 1;  // concurrent worktree daemons on this host (the L-26 flow)
    std::int64_t watch_limit = -1;           // per-user watch-handle limit (Linux inotify), -1 = unknown
    std::int64_t fd_limit = -1;              // per-user open-fd cap, -1 = unknown (documented v1 gap)
};

// The observed presence of ONE signing prerequisite for a target. PRESENCE ONLY — never a secret value
// (R-BUILD-008). known=false means the check could not run on this host (reported as "unknown").
struct SigningProbe
{
    std::string target;
    std::string requirement; // "authenticode" | "developer-id-notarization"
    bool configured = false;
    bool known = true;
};

// The whole injected environment: what the host actually has. The CLI fills it via subprocess version
// probes + OS-limit reads; a test constructs it directly (the corpus of broken environments).
struct EnvironmentProbe
{
    std::vector<ToolProbe> tools;
    FileSyncProbe filesync;
    std::vector<SigningProbe> signing;

    [[nodiscard]] const ToolProbe* find_tool(std::string_view name) const;
    [[nodiscard]] const SigningProbe* find_signing(std::string_view target,
                                                   std::string_view requirement) const;
};

// --- the machine-readable diagnosis -----------------------------------------------------------------

// One toolchain-component finding: what was required, what was found, and the branchable verdict.
struct ComponentFinding
{
    std::string target;
    std::string component;
    std::string role;
    std::string acquisition;       // acquisition_name(...)
    std::string status;            // "ok" | "missing" | "version_mismatch"
    std::string required_version;  // the L-42 pin ("" = presence-only)
    std::string found_version;     // observed version ("" = absent / unparseable)
    std::string enforcement;       // "strict" | "advisory" | "documented"
    std::string code;              // a doctor.* code for a problem finding, "" when ok
    std::string remediation;       // one-line machine-branchable remediation
    std::string remediation_pointer;
    bool fetchable = false;        // acquisition == Fetchable
    bool can_fetch_now = false;    // fetchable AND a verified-fetch mechanism exists for it
    bool blocking = false;         // this finding flips report.ok to false
};

// The file-sync OS-budget finding (advisory — the R-FILE-002 up-front check).
struct FileSyncFinding
{
    std::int64_t project_file_count = -1;
    std::int64_t worktree_daemon_count = 1;
    std::int64_t watch_limit = -1;
    std::int64_t fd_limit = -1;
    std::int64_t required_watches = -1; // project_file_count × worktree_daemon_count (-1 = unknown)
    std::string status;                 // "ok" | "degraded" | "unknown"
    std::string code;                   // kDoctorFileSyncBudgetLowCode when degraded, "" otherwise
    std::string remediation;
    std::string remediation_pointer;
    bool blocking = false;              // always false (advisory watcher.degraded path)
};

// One signing-prerequisite finding (advisory — a ship-time check). PRESENCE ONLY — no secret value.
struct SigningFinding
{
    std::string target;
    std::string requirement;
    std::string status; // "configured" | "absent" | "unknown"
    std::string code;   // kDoctorSigningPrereqAbsentCode when absent, "" otherwise
    std::string remediation;
    std::string remediation_pointer;
    bool blocking = false; // always false (ship-time prereq)
};

// The whole diagnosis. ok ⇒ every requested target's environment is buildable (no blocking finding);
// !ok ⇒ at least one required component is missing or strictly-mismatched (the CLI returns
// doctor.environment_incomplete with this report attached). Non-blocking findings (budget degraded,
// signing absent, advisory version drift) populate `warnings` AND stay in the report.
struct DoctorReport
{
    bool ok = true;
    std::vector<std::string> targets;
    std::vector<ComponentFinding> components;
    FileSyncFinding filesync;
    std::vector<SigningFinding> signing;
    std::vector<std::string> warnings;

    [[nodiscard]] std::size_t blocking_count() const;
};

// Compare a found version against an L-42 pin, numeric-component-wise: `required` empty ⇒ satisfied
// (presence-only); else every numeric component of `required` must equal the corresponding component of
// `found` (so "20.1" is satisfied by "20.1.2" but NOT by "20.10.0"). Pure + total.
[[nodiscard]] bool version_satisfies(std::string_view required, std::string_view found);

// Diagnose the requested targets against the environment probe. `targets` MUST be known build target ids
// (the CLI rejects an unknown target with doctor.unknown_target BEFORE calling this). Pure + total —
// never throws, never touches the filesystem/environment.
[[nodiscard]] DoctorReport diagnose(const std::vector<std::string>& targets,
                                    const std::vector<ToolchainEntry>& manifest,
                                    const EnvironmentProbe& probe);

} // namespace context::editor::build
