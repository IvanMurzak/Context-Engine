// R-EDIT-001 editor-UI extension contract (issue #152): the versioned descriptor a package (or a
// built-in) registers to contribute editor UI — a component inspector, a viewport gizmo, a panel, or
// an asset-kind editor. Every built-in panel is built ON this contract from day one (the Unity
// lesson), so opening it to third parties in v2 hardens an existing boundary instead of retrofitting.
//
// M9 e05b extends the descriptor into the full PANEL MANIFEST v2 (design 04 §3): the icon, docking
// defaults, the content type (a headless uitree panel vs a sandboxed third-party iframe), the D6 state
// schema version, the capability grants, the contributed commands, and the theme contributions. That
// is a BREAKING contract change, so kContractMajor moves 1 -> 2 (see below).

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
inline constexpr std::uint32_t kContractMajor = 2;

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
};

// Docking defaults for a panel contribution (04 §3 `dock`).
struct DockDefaults
{
    DockZone default_zone = DockZone::center;
    // A singleton panel may be open at most once per window (a second open focuses the existing one).
    bool singleton = false;
    // Minimum content size in logical pixels. 0 = "no minimum stated"; negatives are refused.
    int min_width = 0;
    int min_height = 0;
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
};

// The grep-stable token for a contribution kind (used in diagnostics + describe output).
[[nodiscard]] const char* contribution_kind_token(ContributionKind kind);

// The grep-stable token for a dock zone (diagnostics + the manifest projection).
[[nodiscard]] const char* dock_zone_token(DockZone zone);

// The grep-stable token for a content type (diagnostics + the manifest projection).
[[nodiscard]] const char* content_type_token(ContentType type);

} // namespace context::editor::gui::contract
