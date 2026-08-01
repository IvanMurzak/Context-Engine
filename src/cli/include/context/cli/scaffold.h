// Scaffold: `context new`'s runnable templates (R-QA-006 MUST half).
//
// R-QA-006 requires `context new`'s templates to yield a RUNNABLE result, not an illustrative one.
// For the DEFAULT template that means a skeleton — a scene, a camera, and a startable session such
// that the first query/step after `context new` succeeds without error: this module writes that
// template to disk and then PROVES it by loading it into a real context_kernel session, populating
// the World from the scene, and stepping the Scheduler once — the concrete point at which the CLI
// consumes the microkernel. The template files are written with plain std::filesystem rather than
// filesync's atomic IO — a standing CHOICE, not a pending integration (`context_cli` already links
// context_filesync, and `context set` calls `filesync::atomic_write`): stage + fsync + rename earns
// its cost when rewriting a file a reader may be holding, and buys nothing when populating a
// directory that did not exist a moment ago.
//
// For the `extension-panel` template (M9 e13e) "runnable" means something different, because the
// artifact is not a project: it is an editor PACKAGE, and the thing that must succeed is the
// editor's own load of it. So the proof is split across two tiers, deliberately —
//   * HERE, at write time: `verify_extension_package` re-reads what was just written and decides
//     the store rules the CLI can decide WITHOUT the shell — the manifest parses, its `id` is the
//     directory's own, and every contribution is namespaced, `iframe`-typed and entered on this
//     package's own origin — plus the one thing the store CANNOT check (that the panel document
//     `content.entry` names actually EXISTS on disk). It is a SUBSET, never a re-implementation:
//     the closed capability vocabulary, `contractVersion`, and contribution-id UNIQUENESS are
//     decided by the store alone.
//   * In the integration tier: the package is scaffolded into a temp package store and taken
//     through the REAL load path — `scan_package_store` -> `package_mounts` ->
//     `ExtAssetResolver::mount` -> `resolve` (src/tests/integration/test_e13e_ext_scaffold.cpp).
// That split exists because a scaffold whose only test asserted on its own template TEXT would
// prove nothing about whether the editor accepts the result.
//
// THE MIRROR LIST — the five pieces of shell-tier vocabulary this template re-spells. Each is
// pinned by test_e13e_ext_scaffold.cpp, the ONLY target that links both tiers; an unpinned mirror
// that drifts emits packages the store silently refuses, and nothing else in the repo can see it:
//   1. kExtensionPanelManifestFileName <- shell::kPackageManifestFileName  (pin: cross-tier ==)
//   2. kExtensionPanelContractVersion  <- gui::contract::kContractMajor    (pin: the value read
//      back out of the WRITTEN manifest)
//   3. is_scaffold_package_id          <- shell::is_valid_package_id       (pin: id-for-id table)
//   4. kExtUrlPrefix (in scaffold.cpp) <- shell::kExtUrlPrefix             (pin: the entry prefix)
//   5. `window.contextPanelPort`, spelled inside the generated panel.js <- shell::kExtPortGlobalName
//      (pin: the WRITTEN script is searched for it)
// The reason they are mirrored is DEPENDENCY WEIGHT plus a real inversion — NOT "link order", which
// forbids nothing here (CMake resolves target names at generate time, and context_cli already links
// targets declared after it). Mirrors 1/3/4/5 live in context_editor_shell, which PUBLIC-links the
// present path plus X11/AppKit, and the Shell SPAWNS the CLI as a subprocess — so the headless CLI
// linking it would invert that dependency. Mirror 2 is the exception worth knowing: gui::contract is
// pure C++ over context_bridge, which context_cli ALREADY links, so retiring that one costs a single
// link edge. Deliberately not taken here — relocating the mirrors is out of this task's scope.

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
// ⚠ Mirror 1 of the MIRROR LIST above (`editor::shell::kPackageManifestFileName`), pinned against it
// by `test_e13e_ext_scaffold.cpp`. Writing any other name yields a directory the store refuses as
// `package.manifest_missing`.
inline constexpr const char* kExtensionPanelManifestFileName = "context-package.json";

// The panel document the generated manifest's `content.entry` names.
inline constexpr const char* kExtensionPanelEntryFileName = "panel.html";

// The R-EDIT-001 contract major the `extension-panel` manifest DECLARES.
//
// ⚠ Mirror 2 of the MIRROR LIST above. IT MUST EQUAL `gui::contract::kContractMajor`, and is spelled
// here because gui::contract is not on the CLI's link closure TODAY — the one mirror that could be
// retired for a single link edge (see that list). The compatibility
// window is a single major (extension.h), so a manifest stating any other value is REFUSED outright
// by `read_package_manifest` — i.e. the day someone bumps the major, this template silently starts
// emitting packages the editor rejects. `test_e13e_ext_scaffold.cpp` links both tiers and asserts
// the value WRITTEN INTO THE FILE equals `gui::contract::kContractMajor`, so that bump reds a test
// instead of shipping a scaffold that teaches the wrong shape.
inline constexpr int kExtensionPanelContractVersion = 2;

// The list of template names `context new` accepts.
//
// ⚠ It holds TWO KINDS of artifact now — a project (`default`) and an editor package
// (`extension-panel`) — so it is NOT the project-template catalog. `shell::available_templates()`
// (the welcome screen's "New PROJECT from template") is a deliberate SUBSET: its flow scaffolds and
// then opens the result with `context edit`, which an editor package has no `project.json` for. The
// welcome drill asserts welcome ⊆ CLI, so growing THIS list leaves it green — by design, not by luck.
[[nodiscard]] const std::vector<std::string>& template_names();
[[nodiscard]] bool is_known_template(const std::string& name);

// Is `name` usable as an editor-package id — and therefore as the `context-ext://` HOST the
// scaffolded manifest will name?
//
// ⚠ Mirror 3 of the MIRROR LIST above — `editor::shell::is_valid_package_id` (ext_scheme.h) — not a
// second opinion: the store derives a package's id from its DIRECTORY NAME, so `context new
// --template extension-panel <dir>` can only produce a loadable package when `<dir>`'s basename
// satisfies that grammar, and the check has to run at scaffold time, in the CLI. (That derivation is
// why `project_basename` normalizes a trailing separator — see its comment.) The two are pinned
// AGAINST EACH OTHER over a
// shared table by `test_e13e_ext_scaffold.cpp`, which links both — so a grammar change on either
// side reds a test rather than drifting into a scaffold that emits unloadable packages.
[[nodiscard]] bool is_scaffold_package_id(const std::string& name);

// Re-read an already-scaffolded extension package and report whether it passes the load rules the
// CLI can decide: the manifest parses, declares the directory's own id, carries at least one
// contribution — every one namespaced to this package, `iframe`-typed, and naming a `content.entry`
// on THIS package's `context-ext://` origin — and, the check the store itself does not make, that
// each entry's document exists on disk. On success the envelope reports
// {directory, packageId, contributions, entry, loadable:true}; `loadable` is what
// `scaffold_project` echoes into the `extension-panel` result.
//
// ⚠ SCOPE: this is a SCAFFOLD-TIME self-check on output this module just wrote, NOT a validator for
// foreign packages — see the MIRROR LIST's subset note above. A package it accepts can still be
// refused at boot (unknown capability token, a `contractVersion` that is not the current major, a
// duplicate contribution id), so a green here is not a promise the editor will load it.
[[nodiscard]] editor::contract::Envelope verify_extension_package(const std::string& directory);

// A dry-run description of what scaffolding `directory` with `template_name` WOULD write (no I/O).
// It DESCRIBES; it does not decide — callers must refuse an input `scaffold_project` would refuse.
// Prefer `scaffold_dry_run`, which does both.
[[nodiscard]] editor::contract::Json scaffold_plan(const std::string& directory,
                                                   const std::string& template_name);

// The `--dry-run` half of `scaffold_project`: the plan it WOULD apply, or the SAME refusal the real
// run would return, and no I/O either way. A dry run exists to PREDICT the real run — an agent reads
// it to decide whether to proceed — so the two MUST share their refusals rather than each carrying
// its own list. Without this, `--dry-run --template <typo>` reported a confident DEFAULT-template
// file list under the typo'd name, and an illegal package directory reported the five files the real
// run fails closed on.
[[nodiscard]] editor::contract::Envelope scaffold_dry_run(const std::string& directory,
                                                          const std::string& template_name);

// Write the template into `directory` (creating it + subdirs), then PROVE the result. For the
// default template that proof is `verify_runnable` — booting a context_kernel session over the
// scaffolded scene and stepping once — and the envelope reports
// {directory, template, files[], runnable, entities, cameras, mergeDriverInstalled}. For
// `extension-panel` it is `verify_extension_package` instead (no kernel, no scene), and the envelope
// reports {directory, template, files[], packageId, contributions, entry, loadable}. On any failure
// (bad template, illegal package id, write error, unverifiable scaffold) it carries the matching
// R-CLI-008 code.
[[nodiscard]] editor::contract::Envelope scaffold_project(const std::string& directory,
                                                          const std::string& template_name);

// Load an already-scaffolded project directory into a fresh kernel session and step it once,
// returning an ok envelope with {entities, cameras, ticks} when the first query/step succeeds. This
// is the "startable session" proof, factored out so a test can run it against a scaffold.
[[nodiscard]] editor::contract::Envelope verify_runnable(const std::string& directory);

} // namespace context::cli
