// The KERNEL-SIDE scene-tree model builder (M9 e05d3, D10/D18): weaves the FLAT composed entities of
// a compose::ComposedScene (each keyed by its L-35 id-path) into the boundary-clean SceneTreeModel
// hierarchy the panel renders. Split OUT of context_gui_panel_scenetree (owner ruling 2026-07-20) so
// the panel library carries NO kernel type on its public link interface: whatever needs compose::
// types sits on the kernel side of the wire (this library, linked by the daemon / the in-process M5
// harnesses / tests) and reaches the panel as data.

#pragma once

#include "context/editor/compose/flatten.h" // compose::ComposedScene

#include "context/editor/gui/panels/scenetree/scene_tree_model.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::gui::panels::builders
{

// The L-35 id-path joined with '/' — THE stable selection/identity key every wire consumer keys by
// (tree rows, the inspector model's `identity`, the daemon's `editor.inspect` idPath param). The
// join is INJECTIVE: stable ids are lowercase hex and the $root token carries no '/', so distinct
// id-paths never collide. One exported definition so the encoding cannot drift between producers
// (a drift silently breaks selection).
[[nodiscard]] std::string join_identity(const std::vector<std::string>& id_path);

// The INVERSE of join_identity: split a wire identity key back into its L-35 id-path segments.
// nullopt when the key is malformed — empty, or carrying an empty segment (a leading, trailing, or
// doubled '/') — because join_identity can never PRODUCE such a key, so accepting one would invent
// an id-path no composed entity has.
//
// Exported alongside the join (M9 e09b-1) so the encoding and its inverse stay one definition. Two
// consumers need it and each would otherwise hand-roll a copy: find_entity_by_identity (below, which
// had the original file-local copy) and the daemon's pointer/value `edit`, which must turn the wire
// `idPath` into the compose::WriteRequest id-path vector. A splitter that drifts from the join reads
// as a mysterious "selection does not resolve".
//
// The two consumers treat a malformed key differently BY DESIGN, and both are correct: a READ
// (find_entity_by_identity) answers "no such entity" — the honest present:false panel state — while
// a WRITE answers a usage error, because silently writing nothing is the one outcome a write path
// must never produce.
[[nodiscard]] std::optional<std::vector<std::string>> split_identity(std::string_view identity);

// Build the scene-tree view model from a flattened composed scene (the real derived world). The flat
// ComposedScene::entities are woven into a hierarchy by id-path prefix; a prefix with no own composed
// entity becomes a synthetic instance-boundary node (NodeKind::instance). An entity whose
// field_provenance carries an override contributor is marked `overridden`. Total and deterministic.
[[nodiscard]] scenetree::SceneTreeModel build_scene_tree(const compose::ComposedScene& scene);

} // namespace context::editor::gui::panels::builders
