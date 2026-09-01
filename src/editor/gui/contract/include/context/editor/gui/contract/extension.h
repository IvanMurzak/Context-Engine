// R-EDIT-001 editor-UI extension contract (issue #152): the versioned descriptor a package (or a
// built-in) registers to contribute editor UI — a component inspector, a viewport gizmo, a panel, or
// an asset-kind editor. Every built-in panel is built ON this contract from day one (the Unity
// lesson), so opening it to third parties in v2 hardens an existing boundary instead of retrofitting.
//
// M9 e05b extends the descriptor into the full PANEL MANIFEST v2 (design 04 §3): the icon, docking
// defaults, the content type (a headless uitree panel vs a sandboxed third-party iframe), the D6 state
// schema version, the capability grants, the contributed commands, and the theme contributions. That
// is a BREAKING contract change, so kContractMajor moves 1 -> 2 (see below).
//
// editor-UX c2 extends it again into MANIFEST v3 (design 04 §2, D6): `instances {mode, max}` REPLACES
// v2's `dock.singleton` boolean, `path` groups the panel in the Window menu's tree, and
// `selection.subjects[]` / `events.{publishes,subscribes}[]` declare the D2 selection subjects and the
// D4 package facts a contribution deals in. Another BREAKING change, so kContractMajor moves 2 -> 3.

#pragma once

#include "context/editor/gui/contract/sandbox.h"

#include <cstdint>
#include <string>
#include <vector>

namespace context::editor::gui::contract
{

// The R-EDIT-001 extension-contract major. A contribution declaring a different major is refused by
// the registry — the compatibility window is exactly {kContractMajor} while one major exists, so the
// surface can evolve without silently breaking a contribution (mirrors the R-CLI-010
// protocol-negotiation discipline).
//
// 1 -> 2 (M9 e05b): the panel manifest v2 (04 §3) added the icon / dock / content / state /
// capabilities / commands / themes members below. Because the compatibility window is a SINGLE major,
// this bump refuses every v1 contribution the moment it lands — deliberate, and safe only because the
// registry has no out-of-repo clients yet (the M9 e05b enumeration walked EVERY in-repo consumer —
// the four CMake targets that link context_gui_contract and their tests, harnesses and fixtures — and
// each references this constant SYMBOLICALLY rather than hardcoding a literal).
//
// 2 -> 3 (editor-UX c2, D6): the panel manifest v3 (04 §2) REMOVED `dock.singleton` — not deprecated,
// removed — in favour of `instances {mode, max}`, and added `path`, `selection.subjects[]` and
// `events.{publishes,subscribes}[]`. Same single-major window, so this refuses every v2 contribution
// the instant it lands, and it is safe for the same reason and ONLY that reason: there are still no
// out-of-repo consumers, and after v1 ships the identical change costs a deprecation cycle. The
// enumeration was RE-RUN rather than inherited from the 1 -> 2 bump (that list was a year old): the
// targets that link context_gui_contract today are context_gui_a11y (+ its scan executable), the five
// context_gui_contract_test_* executables, context_gui_help_contextual, context_editor_host,
// context_editor_shell (PUBLIC, so every shell test + the CEF smokes), the integration
// test_m5exit3_seam_checklist executable, and — new with this bump — context_cli, which linked it in
// order to RETIRE the last hardcoded literal in the tree (cli::kExtensionPanelContractVersion, mirror
// 2 of scaffold.h's mirror list). Every one of them names this constant symbolically.
inline constexpr std::uint32_t kContractMajor = 3;

// The kinds of editor UI a package may contribute.
enum class ContributionKind
{
    panel,             // a free-floating panel (e.g. Problems, a custom tool)
    inspector,         // a component inspector, keyed by component type
    gizmo,             // a viewport gizmo, keyed by component type
    asset_kind_editor, // an editor for an authored asset kind, keyed by kind id
};

// Where a panel docks by default when it is first opened (04 §3 `dock.defaultZone`). Geometry beyond
// this hint is the docking layer's business (Dockview, D2) — the manifest only states the intent.
enum class DockZone
{
    left,
    right,
    top,
    bottom,
    center,
};

// How a contribution's content is produced (04 §3 `content.type`).
enum class ContentType
{
    // A headless C++ uitree panel the host renders itself (every built-in). `entry` MUST be empty —
    // the panel model IS the content.
    uitree,
    // A third-party web panel loaded into a sandboxed iframe (04 §5). `entry` MUST name its URL.
    iframe,
    // A panel editor-core renders ITSELF, from the e06c component kit (M9 e06d). `entry` MUST be
    // empty — like `uitree`, the model IS the content; unlike `uitree`, that model lives in the
    // RENDERER, not in C++.
    //
    // WHY A THIRD TOKEN EXISTS, stated so it is not reached for casually. The 04 §4 rule stands: a
    // panel whose subject is PROJECT state is a C++ uitree model (headless-instantiable, one logic
    // implementation serving both CI and the live editor). This token is for the strictly narrower
    // case where the panel's subject IS THE RENDERER'S OWN STATE and has no C++ representation at
    // all — the Settings panel's theme picker writes CSS custom properties on the editor-core
    // document (06 §1), so a C++ model of it could only be a second, lagging copy of state it
    // cannot observe. The test is "could a headless C++ model answer this panel's questions?"; when
    // it can, `uitree` is the answer.
    //
    // A `local` panel is NOT exempt from the register-with-the-panel discipline — it is covered on
    // the OTHER side of the wire: `gui-a11y-coverage` requires its coverage.manifest.jsonl line to
    // declare `ts-a11y`, and its a11y is asserted in the `webui-ts-*` browser tier over the REAL
    // DOM it renders, which is strictly closer to what a user meets than a scan of a C++ model.
    local,
};

// Docking defaults for a panel contribution (04 §3 `dock`).
//
// ⚠ `singleton` LIVED HERE UNTIL MANIFEST v3 and is GONE — see InstanceSpec below. It was removed
// rather than deprecated because the compatibility window is a single major: a deprecated member
// would have to be honoured by a registry that also honours `instances`, i.e. two sources of truth
// for one question ("how many copies may exist?") with no rule for disagreement.
struct DockDefaults
{
    DockZone default_zone = DockZone::center;
    // Minimum content size in logical pixels. 0 = "no minimum stated"; negatives are refused.
    int min_width = 0;
    int min_height = 0;
};

// How many live copies of a panel kind may exist at once (manifest v3 `instances.mode`, D6).
//
// This is the WHOLE of v2's `dock.singleton`, spelled as the closed vocabulary it always wanted to
// be: `singleton` IS what `true` meant. The imperative half — minting instance ids, focusing an
// already-open singleton, refusing past a limit — is the instance runtime (task c3); the manifest
// only DECLARES the rule, and the declaration is inert until that runtime lands.
enum class InstanceMode
{
    // At most one live copy; a second open focuses the existing instance rather than failing.
    singleton,
    // At most `max` live copies (`max` MUST be > 0); an open past the limit is refused with a
    // diagnostic naming it.
    limited,
    // As many copies as are asked for (the Viewport: several scene views is the point).
    unlimited,
};

// The instance declaration (manifest v3 `instances`).
//
// THE DEFAULT IS THE RESTRICTIVE ONE, and that is a deliberate change of meaning from v2, where
// `DockDefaults::singleton` defaulted to FALSE. That default was decorative — nothing consulted the
// flag, and `PanelHost` refuses a second open of ANY panel today (04 §1) — so `singleton` is both the
// deny-by-default answer and the truthful description of the runtime a v3 manifest meets.
struct InstanceSpec
{
    InstanceMode mode = InstanceMode::singleton;
    // The ceiling for `limited`, and MEANINGFUL ONLY THERE. 0 = unstated. The registry refuses
    // `limited` without a positive `max` AND refuses a `max` stated on either other mode, rather than
    // ignoring it: a silently ignored ceiling is a manifest lying about what it asked for.
    int max = 0;
};

// The selection subjects a contribution deals in (manifest v3 `selection.subjects[]`, D2).
//
// The subject-kind vocabulary is OPEN: `entity` / `file` / `asset` are contract-owned, and a package
// declares its own `<pkg>.<kind>` here. The registry validates the NAMESPACING (see
// Contribution::package_id) — a package may not name an unnamespaced kind, because that is how it
// would claim a contract-owned one.
struct SelectionSpec
{
    std::vector<std::string> subjects;
};

// The package facts a contribution deals in (manifest v3 `events`, D4).
//
// `publishes` are the daemon topics this contribution produces; a package may only publish under its
// OWN namespace. `subscribes` are the topics it consumes, which is exactly where a package names
// ANOTHER package's topic — the install-time consented, deny-by-default grant D4 describes — so the
// namespacing rule there is "namespaced under SOMEBODY", not "namespaced under you".
struct EventSpec
{
    std::vector<std::string> publishes;
    std::vector<std::string> subscribes;
};

// The content production seam for a contribution (04 §3 `content`).
struct ContentSpec
{
    ContentType type = ContentType::uitree;
    // The iframe entry URL (e.g. "context-ext://<package-id>/panel.html"). Required for `iframe`,
    // and MUST be empty for `uitree` — the registry refuses either mismatch.
    std::string entry;
};

// The D6 panel-state contract declaration (04 §3 `state`). The version the panel writes today; a
// persisted blob carrying any other version is handed back as NULL state plus a diagnostic, never a
// crash (see panel_state.h).
struct StateSpec
{
    std::uint32_t schema_version = 1;
};

// One command a contribution declares in its manifest (04 §3 `commands`). Built-in panels declare
// their commands on their uitree Panel model instead (the single source of truth for a C++ panel);
// this array exists for iframe contributions, which have no C++ model to read them from.
struct CommandContribution
{
    std::string id;    // stable, unique within the contribution (e.g. "inspector.edit")
    std::string title; // human/AI-readable label
    std::string when;  // optional context clause (e.g. "panelFocus == inspector"); empty = always
};

// --- capability vocabulary (04 §3 `capabilities`) -------------------------------------------------
// A capability is the manifest-declared grant a contribution ASKS for. The first four correspond
// one-to-one to the R-SEC-007 bridge scope vocabulary (bridge::Scope) so the manifest and the
// dispatcher speak one language — note the SPELLING differs by design: manifest tokens are
// underscored ("file_write") while the bridge's wire names are hyphenated ("file-write", see
// ScopeSet::names()), so registry.cpp owns the one translation between them. `ui_events` is the
// additional editor.ui read grant the panel bridge requires for bridge.ui.subscribe (04 §5, C-F18).
// Deny-by-default: a token outside this closed set is REFUSED (an unknown capability must never be
// silently dropped into a weaker-than-declared grant), and the registry additionally refuses a
// contribution whose sandbox GRANT exceeds what its manifest declares.
inline constexpr const char* kCapabilityReadQuery = "read_query";
inline constexpr const char* kCapabilityFileWrite = "file_write";
inline constexpr const char* kCapabilitySessionControl = "session_control";
inline constexpr const char* kCapabilityBuildInstall = "build_install";
inline constexpr const char* kCapabilityUiEvents = "ui_events";
// The editor-UX d2 (D4) PACKAGE FACT grant: may this contribution SUBSCRIBE to ANOTHER package's
// declared fact topic? Like `ui_events` it corresponds to NO daemon scope — a package fact rides the
// ordinary baseline subscription, and the Shell decides which topics reach which package
// (package_facts.h) — so granting it can never widen a package's daemon session.
//
// ⚠ IT IS ONE TOKEN FOR "may subscribe to foreign topics AT ALL", NOT one per topic, and the
// per-topic answer is the MANIFEST's: the grant is clamped to `events.subscribes[]`, which the
// registry already forces to be namespaced under SOMEBODY. Two clamps, and both are needed — a
// per-topic token vocabulary would put package-chosen strings into the closed capability set, which
// is exactly what `capability_supported` exists to prevent, while the token alone would let a
// consented package subscribe to a topic it never declared an interest in.
inline constexpr const char* kCapabilityPackageEvents = "package_events";

// Is `capability` on the closed manifest capability allowlist above?
[[nodiscard]] bool capability_supported(const std::string& capability);

// One registered editor-UI contribution — the panel manifest v2 (04 §3).
struct Contribution
{
    std::string id;   // stable, unique within a registry (e.g. "builtin.scene-tree")
    ContributionKind kind = ContributionKind::panel;
    // What it attaches to: a component type for inspector/gizmo, an asset-kind id for an asset-kind
    // editor, empty for a free-floating panel.
    std::string target;
    std::string title;
    // The R-EDIT-001 contract major this contribution was written against (see kContractMajor).
    std::uint32_t contract_version = kContractMajor;
    // The renderer trust boundary applied to this contribution (default = least privilege).
    SandboxPolicy sandbox;

    // --- manifest v2 (M9 e05b, design 04 §3) ------------------------------------------------------
    // Icon-set name (06 token kit). Empty = the host picks a default for the kind.
    std::string icon;
    DockDefaults dock;
    ContentSpec content;
    StateSpec state;
    // Requested capability grants, from the closed vocabulary above. Empty = the read/query baseline.
    std::vector<std::string> capabilities;
    // Manifest-declared commands (iframe contributions; see CommandContribution).
    std::vector<CommandContribution> commands;
    // Optional theme.json contributions (06).
    std::vector<std::string> themes;

    // --- manifest v3 (editor-UX c2, design 04 §2) -------------------------------------------------
    // How many live copies of this panel kind may exist (replaces v2's `dock.singleton`).
    InstanceSpec instances;
    // Slash-separated DISPLAY grouping for the Window menu's panel tree (d1), e.g. "Scene/Debug".
    // Empty = top level. It is NOT a filesystem path and nothing resolves it — the registry validates
    // it only as display text (no leading/trailing slash, no empty segment).
    std::string path;
    SelectionSpec selection;
    EventSpec events;

    // PROVENANCE, not a manifest member: the id of the package that declared this contribution, set
    // by the loader that read it (shell::read_package_manifest derives it from the package DIRECTORY,
    // never from the manifest text). EMPTY means a BUILT-IN — the editor's own contribution — which
    // is what buys a built-in the right to name an unnamespaced, contract-owned selection subject
    // while a package may not. It is deliberately not projected onto the wire: the renderer already
    // learns a package's identity from the contribution id's own namespace.
    std::string package_id;
};

// The grep-stable token for a contribution kind (used in diagnostics + describe output).
[[nodiscard]] const char* contribution_kind_token(ContributionKind kind);

// The grep-stable token for a dock zone (diagnostics + the manifest projection).
[[nodiscard]] const char* dock_zone_token(DockZone zone);

// The grep-stable token for a content type (diagnostics + the manifest projection).
[[nodiscard]] const char* content_type_token(ContentType type);

// The grep-stable token for an instance mode (diagnostics + the manifest projection).
//
// ⚠ CROSS-LANGUAGE AND GATED SINCE editor-UX c3. Every C++ inverse searches THIS table rather than
// keeping a second copy — `package_store.cpp`'s `read_instance_mode` walks `kInstanceModes` below
// and `scaffold.cpp` emits through this function — and `panels.ts`'s `PANEL_INSTANCE_MODES`, which
// is a hand-written mirror, is now compared against this switch SET vs SET by
// `tools/check_webui_assets.py --panel-contract` (ctest `webui-panel-contract`), reading the TS side
// out of the BUILT bundle.
//
// The gap that closed, recorded because it was live for a whole contract major: until c3 the check
// read only `panel_state.h` from this directory, so renaming a token here red NOTHING on the TS
// side — the parser fell to its `?? "singleton"` default and EVERY panel silently became a
// singleton, which is the exact failure the instance runtime exists to prevent. `dock_zone_token`
// and `content_type_token` sat in the same position and are enrolled by the same generalisation
// (`_read_cpp_token_switch`), so all three closed vocabularies are now gated together.
[[nodiscard]] const char* instance_mode_token(InstanceMode mode);

// Every instance mode, in declaration order, so a reader can invert `instance_mode_token` by search
// instead of by a hand-written table that could drift from it.
inline constexpr InstanceMode kInstanceModes[] = {InstanceMode::singleton, InstanceMode::limited,
                                                  InstanceMode::unlimited};

} // namespace context::editor::gui::contract
