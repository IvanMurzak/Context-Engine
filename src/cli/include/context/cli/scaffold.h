// Scaffold: `context new`'s runnable templates (R-QA-006 MUST half).
//
// R-QA-006 requires `context new`'s templates to yield a RUNNABLE result, not an illustrative one.
// For the DEFAULT template that means a skeleton — a scene, a camera, and a startable session such
// that the first query/step after `context new` succeeds without error: this module writes that
// template to disk and then PROVES it by loading it into a real context_kernel session, populating
// the World from the scene, and stepping the Scheduler once — the concrete point at which the CLI
// consumes the microkernel. The atomic file writes are plain std::filesystem here; they route
// through filesync's atomic IO once integrated.
//
// For the `extension-panel` template (M9 e13e) "runnable" means something different, because the
// artifact is not a project: it is an editor PACKAGE, and the thing that must succeed is the
// editor's own load of it. So the proof is split across two tiers, deliberately —
//   * HERE, at write time: `verify_extension_package` re-reads what was just written and refuses
//     anything the store would refuse, plus the one thing the store cannot check (that the panel
//     document `content.entry` names actually EXISTS on disk).
//   * In the integration tier: the package is scaffolded into a temp package store and taken
//     through the REAL load path — `scan_package_store` -> `package_mounts` ->
//     `ExtAssetResolver::mount` -> `resolve` (src/tests/integration/test_e13e_ext_scaffold.cpp).
// That split exists because the store + scheme live in `src/editor/shell/`, which sits ABOVE the
// CLI in the link order; the CLI cannot call them, and a scaffold whose only test asserted on its
// own template TEXT would prove nothing about whether the editor accepts the result.

#pragma once

#include "context/editor/contract/envelope.h"

#include <string>
#include <vector>

namespace context::cli
{

// The template `context new` scaffolds when none is named: the R-QA-006 runnable project skeleton.
inline constexpr const char* kDefaultTemplate = "default";

// The M9 e13e third-party editor-package template: a manifest + a hello iframe panel.
inline constexpr const char* kExtensionPanelTemplate = "extension-panel";

// The manifest file the package store reads at a package's root.
//
// ⚠ A MIRROR of `editor::shell::kPackageManifestFileName`, for the same link-order reason as
// `is_scaffold_package_id` below, and pinned against it by `test_e13e_ext_scaffold.cpp`. Writing any
// other name yields a directory the store refuses as `package.manifest_missing`.
inline constexpr const char* kExtensionPanelManifestFileName = "context-package.json";

// The panel document the generated manifest's `content.entry` names.
inline constexpr const char* kExtensionPanelEntryFileName = "panel.html";

// The R-EDIT-001 contract major the `extension-panel` manifest DECLARES.
//
// ⚠ IT MUST EQUAL `gui::contract::kContractMajor`, and it is spelled here rather than included from
// there because `src/editor/gui/contract/` is not on the CLI's link closure. The compatibility
// window is a single major (extension.h), so a manifest stating any other value is REFUSED outright
// by `read_package_manifest` — i.e. the day someone bumps the major, this template silently starts
// emitting packages the editor rejects. `test_e13e_ext_scaffold.cpp` links both tiers and asserts
// the value WRITTEN INTO THE FILE equals `gui::contract::kContractMajor`, so that bump reds a test
// instead of shipping a scaffold that teaches the wrong shape.
inline constexpr int kExtensionPanelContractVersion = 2;

// The list of template names `context new` accepts.
[[nodiscard]] const std::vector<std::string>& template_names();
[[nodiscard]] bool is_known_template(const std::string& name);

// Is `name` usable as an editor-package id — and therefore as the `context-ext://` HOST the
// scaffolded manifest will name?
//
// ⚠ A DELIBERATE MIRROR of `editor::shell::is_valid_package_id` (ext_scheme.h), not a second
// opinion: the store derives a package's id from its DIRECTORY NAME, so `context new --template
// extension-panel <dir>` can only produce a loadable package when `<dir>`'s basename satisfies that
// grammar. The mirror exists because `src/editor/shell/` sits above the CLI in the link order and
// this check has to run at scaffold time, in the CLI. The two are pinned AGAINST EACH OTHER over a
// shared table by `test_e13e_ext_scaffold.cpp`, which links both — so a grammar change on either
// side reds a test rather than drifting into a scaffold that emits unloadable packages.
[[nodiscard]] bool is_scaffold_package_id(const std::string& name);

// Re-read an already-scaffolded extension package and report whether the editor could load it:
// the manifest parses, declares the directory's own id, carries at least one iframe panel
// contribution whose `content.entry` names THIS package's `context-ext://` origin, and — the check
// the store itself does not make — that entry's document exists on disk. On success the envelope
// reports {directory, packageId, contributions, entry, files[]}.
[[nodiscard]] editor::contract::Envelope verify_extension_package(const std::string& directory);

// A dry-run description of what scaffolding `directory` with `template_name` WOULD write (no I/O).
[[nodiscard]] editor::contract::Json scaffold_plan(const std::string& directory,
                                                   const std::string& template_name);

// Write the template into `directory` (creating it + subdirs), then verify it is RUNNABLE by
// booting a context_kernel session over the scaffolded scene and stepping once. On success the
// envelope's data reports {directory, files[], entities, cameras, ticks}; on any failure
// (bad template, write error, non-runnable scaffold) it carries the matching R-CLI-008 code.
[[nodiscard]] editor::contract::Envelope scaffold_project(const std::string& directory,
                                                          const std::string& template_name);

// Load an already-scaffolded project directory into a fresh kernel session and step it once,
// returning an ok envelope with {entities, cameras, ticks} when the first query/step succeeds. This
// is the "startable session" proof, factored out so a test can run it against a scaffold.
[[nodiscard]] editor::contract::Envelope verify_runnable(const std::string& directory);

} // namespace context::cli
