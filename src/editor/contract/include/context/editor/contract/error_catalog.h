// Error-code catalog: the stable, versioned, introspectable diagnostic vocabulary (R-CLI-008).
//
// Every machine-readable diagnostic the engine emits — file/TS diagnostics (R-FILE-003), the
// version-mismatch error (R-BRIDGE-006), path-jail violations (R-SEC-008), engine-compat mismatch
// (R-PKG-005) — draws its `code` from THIS one catalog rather than an ad-hoc shape. Two contracts
// the catalog upholds:
//   1. Additive-only across a major version (CI-enforced): a code, once shipped in the frozen
//      baseline, is never removed or renamed. The check
//      `missing_from_catalog(baseline_v0_codes(), catalog())` MUST stay empty — that is the
//      enforcement point the test and CI call.
//   2. A fixed exit-code table: every catalog entry maps to a process exit code, so a CLI caller
//      can branch on the class of failure without parsing the message.

#pragma once

#include <string>
#include <vector>

namespace context::editor::contract
{

// One entry in the versioned error-code catalog.
struct ErrorCode
{
    std::string code;    // stable dotted identifier, e.g. "path.jail_violation"
    std::string message; // human-facing template
    bool retriable;      // is a bare retry meaningful (transient) vs pointless (deterministic)?
    int exit_code;       // the process exit code the CLI returns for this class (exit-code table)
    std::string origin;  // the requirement whose diagnostics fold into this code (provenance)
};

// The live catalog the engine currently ships. Grows additively; never loses an entry.
[[nodiscard]] const std::vector<ErrorCode>& catalog();

// Look a code up. Returns nullptr when absent.
[[nodiscard]] const ErrorCode* find_code(const std::string& code);

// The exit code for a given error code; falls back to the generic-error exit (1) when unknown.
[[nodiscard]] int exit_code_for(const std::string& code);

// The FROZEN v0 baseline: the set of codes that shipped when protocolMajor == 0 was cut. This is a
// deliberately SEPARATE hardcoded snapshot from catalog() so removing/renaming a live code is
// caught structurally (the baseline still lists the gone code).
[[nodiscard]] const std::vector<std::string>& baseline_v0_codes();

// Codes present in `baseline` but MISSING from `live` — a non-empty result is an additive-only
// violation (something was removed or renamed). The R-CLI-008 CI enforcement.
[[nodiscard]] std::vector<std::string>
missing_from_catalog(const std::vector<std::string>& baseline, const std::vector<ErrorCode>& live);

} // namespace context::editor::contract
