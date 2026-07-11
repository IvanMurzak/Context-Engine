// R-EDIT-001 editor-UI extension contract (issue #152): the versioned descriptor a package (or a
// built-in) registers to contribute editor UI — a component inspector, a viewport gizmo, a panel, or
// an asset-kind editor. Every built-in panel is built ON this contract from day one (the Unity
// lesson), so opening it to third parties in v2 hardens an existing boundary instead of retrofitting.

#pragma once

#include "context/editor/gui/contract/sandbox.h"

#include <cstdint>
#include <string>

namespace context::editor::gui::contract
{

// The R-EDIT-001 extension-contract major. Starts at 1 (the M5 design-time contract). A contribution
// declaring a different major is refused by the registry — the compatibility window is exactly
// {kContractMajor} while one major exists, so the surface can evolve without silently breaking a
// contribution (mirrors the R-CLI-010 protocol-negotiation discipline).
inline constexpr std::uint32_t kContractMajor = 1;

// The kinds of editor UI a package may contribute.
enum class ContributionKind
{
    panel,             // a free-floating panel (e.g. Problems, a custom tool)
    inspector,         // a component inspector, keyed by component type
    gizmo,             // a viewport gizmo, keyed by component type
    asset_kind_editor, // an editor for an authored asset kind, keyed by kind id
};

// One registered editor-UI contribution.
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
};

// The grep-stable token for a contribution kind (used in diagnostics + describe output).
[[nodiscard]] const char* contribution_kind_token(ContributionKind kind);

} // namespace context::editor::gui::contract
