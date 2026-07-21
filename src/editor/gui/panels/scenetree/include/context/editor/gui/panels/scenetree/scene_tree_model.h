// Derived-world scene-tree view model (M5-F2 / M9 e05d3, R-EDIT-001 / L-35): the panel-facing,
// CEF-free hierarchy the scene-tree panel renders. BOUNDARY-CLEAN by construction (D10/D18, owner
// ruling 2026-07-20): plain data only — no compose::/schema:: type appears here, so the panel
// library is Shell-hostable under the D10 shell-boundary gate. The kernel-typed builder that weaves
// the FLAT composed entities (each keyed by its L-35 id-path) into this hierarchy lives on the
// kernel side of the wire: gui/panels/builders/scene_tree_builder.h (context_gui_panel_builders).
// Read-only: an observer view of the composed derived world, never a write path.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace context::editor::gui::panels::scenetree
{

// What a node represents in the composed hierarchy (L-35). Every non-final id-path segment is an
// instance id and the final segment is an entity id, so a prefix with no composed entity of its own
// is an instance boundary (the sub-scene root the instance composes under).
enum class NodeKind
{
    entity,   // a composed entity (an authored/added entity, or a composable instanced root)
    instance, // a synthetic scene-instance boundary — an id-path prefix with no own composed entity
};

// One node of the derived-world scene tree. `identity` is the L-35 id-path joined with '/', the
// stable selection key — unique across the tree (stable ids / the $root token never contain '/').
// `identity_hash` is the L-37 composed identity hash (0 for a synthetic instance boundary that has
// no own composed entity). `overridden` marks a node whose composed value has at least one L-35
// override contributor, so overrides are visible in the tree, not hidden.
struct SceneTreeNode
{
    std::string identity;
    std::uint64_t identity_hash = 0;
    std::string display_name;
    NodeKind kind = NodeKind::entity;
    bool overridden = false;
    std::vector<SceneTreeNode> children;
};

// The derived-world scene tree the panel renders: the forest of top-level nodes for one composed
// scene plus the metadata the panel surfaces in its status line. Deterministic — node order is the
// composed entities' expansion order (root entity, authored entities, structural adds, instances
// depth-first), with synthetic instance boundaries ordered by first appearance.
struct SceneTreeModel
{
    std::string root_scene;           // ComposedScene::root_path
    bool ok = true;                   // false iff the flatten emitted a blocking diagnostic
    std::size_t entity_count = 0;     // number of composed entities represented
    std::vector<SceneTreeNode> roots; // top-level nodes (id-path length 1)
};

// Depth-first search for the node whose `identity` equals `identity` (the selection key). Returns
// nullptr when absent. Searches the whole forest.
[[nodiscard]] const SceneTreeNode* find_node(const SceneTreeModel& model, const std::string& identity);

} // namespace context::editor::gui::panels::scenetree
