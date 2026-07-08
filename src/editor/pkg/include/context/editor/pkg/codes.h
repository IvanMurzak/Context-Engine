// The R-CLI-008 catalog codes the engine-driven install path (R-SEC-005 / R-SEC-011) emits, DEFINED
// here as the single source of truth and REGISTERED in src/editor/contract/error_catalog.cpp — the
// same promote-a-local-string pattern runtime/ts (kTs*Code) and bridge (kScopeDeniedCode) use, so
// src/editor/pkg does not need to link the editor/contract layer. error_catalog.cpp writes the same
// string literals (with a comment pointing here); test_error_catalog + this header keep them 1:1.

#pragma once

#include <string_view>

namespace context::editor::pkg
{

// version-pin-violation (R-SEC-005): a declared dependency spec is not an exact pinned version
// (a range / dist-tag / url). Deterministic; validation class.
inline constexpr std::string_view kInstallVersionUnpinnedCode = "install.version_unpinned";

// lockfile-integrity-failure (R-SEC-005): a fetched artifact's bytes did not match the lockfile's
// integrity (SRI) hash, or the SRI named no algorithm the engine can verify. Fail-closed; the
// artifact is refused, never used with a warning. Validation class.
inline constexpr std::string_view kInstallIntegrityMismatchCode = "install.integrity_mismatch";

// A dependency is missing from the lockfile, or its lock entry lacks an exact version / integrity —
// the lockfile does not fully pin the dependency graph, so the install is refused fail-closed
// (pinning is enforced incl. transitive dependencies). Validation class.
inline constexpr std::string_view kInstallLockfileIncompleteCode = "install.lockfile_incomplete";

// install-scripts-required -> native-tier-gated (R-SEC-005 / L-49): the package declares an install
// lifecycle script, classifying it native-tier; engine-driven installs never run lifecycle scripts,
// so a scripts-requiring package is refused pending the L-49 consent gate. Permission class.
inline constexpr std::string_view kInstallScriptsRequiredCode = "install.scripts_required";

// The R-SEC-011 machine-readable consent-gate code (reserved from day one; the R-CLI-008 catalog is
// additive-only so reserving the slot keeps the v2 async-consent protocol non-breaking). A
// native-tier action hit without the needed grant returns this; a bare retry cannot grant, so it is
// retriable=false until the out-of-band approval flow lands. Permission class.
inline constexpr std::string_view kConsentRequiredCode = "consent_required";

} // namespace context::editor::pkg
