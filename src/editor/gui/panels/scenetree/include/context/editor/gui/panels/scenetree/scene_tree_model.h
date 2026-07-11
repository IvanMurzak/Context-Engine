// Derived-world scene-tree view model (M5-F2, R-EDIT-001 / L-35): the panel-facing, CEF-free
// hierarchy the scene-tree panel renders. Kept separate from the panel's uitree rendering so the
// tree-building logic — weaving the FLAT composed entities (each keyed by its L-35 id-path) into a
// nested hierarchy with instances/overrides visible — is asserted on its own, and the panel renders
// a stable projection. Read-only: an observer view of the composed derived world, never a write path.

#pragma once

#include "context/editor/compose/flatten.h"

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

// Build the scene-tree view model from a flattened composed scene (the real derived world). The flat
// ComposedScene::entities are woven into a hierarchy by id-path prefix; a prefix with no own composed
// entity becomes a synthetic instance-boundary node (NodeKind::instance). An entity whose
// field_provenance carries an override contributor is marked `overridden`. Total and deterministic.
[[nodiscard]] SceneTreeModel build_scene_tree(const compose::ComposedScene& scene);

// Depth-first search for the node whose `identity` equals `identity` (the selection key). Returns
// nullptr when absent. Searches the whole forest.
[[nodiscard]] const SceneTreeNode* find_node(const SceneTreeModel& model, const std::string& identity);

} // namespace context::editor::gui::panels::scenetree
