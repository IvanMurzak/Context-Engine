// The build.* diagnostic vocabulary (R-CLI-008, task a05). The build orchestrator OWNS these code
// strings; the contract error catalog (src/editor/contract/error_catalog.cpp) registers the SAME
// literals — the promote-a-local-string pattern used across the codebase (bridge's scope.denied,
// runtime/ts's kTs*Code, pkg's kInstall*Code), so this build module never links the contract layer.
// Keep these strings in lockstep with the catalog's build.* block (a code appearing in only one place
// is the drift a reviewer catches; there is no compile-time binding by design, exactly like the other
// promoted-string domains).

#pragma once

#include <string_view>

namespace context::editor::build
{

// The runnable project/template failed pre-build verification (a malformed or empty root scene, or a
// blocking composition diagnostic) — OR the R-BUILD-004 export template (the shipped --runtime host
// binary) failed verify-before-use against the pinned trust root (R-SEC-009 / L-58; the CLI fires this
// code from src/cli/build_command.cpp's verify seam). Deterministic (validation-class): a bare retry
// re-fails.
inline constexpr std::string_view kBuildTemplateUnverifiedCode = "build.template_unverified";

// The per-target toolchain manifest (R-PKG-002 / L-42) could not supply the requested target's
// toolchain — no manifest entry for the target — OR an engine-fetched toolchain artifact failed
// verify-before-use against the pinned trust root (R-SEC-009; an unverifiable fetch is a failed fetch,
// fired from the CLI verify seam). Transient/environmental (retriable): a re-fetch against a repaired
// manifest or a correctly-signed artifact can succeed.
inline constexpr std::string_view kBuildToolchainFetchFailedCode = "build.toolchain_fetch_failed";

// The authored-script (TypeScript) AOT tier could not be produced for the target — a malformed or
// unresolvable script entrypoint. Deterministic (a bare retry re-fails on the same source).
inline constexpr std::string_view kBuildAotFailedCode = "build.aot_failed";

// A per-platform asset transcode node failed while producing the target's variant (wraps the a03
// transcode.* detail code). Deterministic (same artifact + platform re-fails).
inline constexpr std::string_view kBuildTranscodeFailedCode = "build.transcode_failed";

// The final-link path failed: the R-KERNEL-003 generated-registration TU references a package with no
// registrable module (an undefined register_<pkg> — the link's undefined-symbol failure).
// Deterministic (a bare retry cannot conjure the missing module).
inline constexpr std::string_view kBuildLinkFailedCode = "build.link_failed";

} // namespace context::editor::build
