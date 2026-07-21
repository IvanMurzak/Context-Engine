// The KERNEL-SIDE scene-tree model builder (M9 e05d3, D10/D18): weaves the FLAT composed entities of
// a compose::ComposedScene (each keyed by its L-35 id-path) into the boundary-clean SceneTreeModel
// hierarchy the panel renders. Split OUT of context_gui_panel_scenetree (owner ruling 2026-07-20) so
// the panel library carries NO kernel type on its public link interface: whatever needs compose::
// types sits on the kernel side of the wire (this library, linked by the daemon / the in-process M5
// harnesses / tests) and reaches the panel as data.

#pragma once

#include "context/editor/compose/flatten.h" // compose::ComposedScene

#include "context/editor/gui/panels/scenetree/scene_tree_model.h"

namespace context::editor::gui::panels::builders
{

// Build the scene-tree view model from a flattened composed scene (the real derived world). The flat
// ComposedScene::entities are woven into a hierarchy by id-path prefix; a prefix with no own composed
// entity becomes a synthetic instance-boundary node (NodeKind::instance). An entity whose
// field_provenance carries an override contributor is marked `overridden`. Total and deterministic.
[[nodiscard]] scenetree::SceneTreeModel build_scene_tree(const compose::ComposedScene& scene);

} // namespace context::editor::gui::panels::builders
