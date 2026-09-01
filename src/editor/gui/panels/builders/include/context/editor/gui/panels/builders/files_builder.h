// The KERNEL-SIDE file-tree model builder (M9 e1, D10): weaves the project's real file list (already
// filtered to asset-candidate paths — assetdb::is_asset_candidate) and the asset database's live
// GUID/kind index into the boundary-clean FilesModel the Files panel renders (D10/D18, the same split
// scene_tree_builder.h documents). Mirrors that builder's TreeBuilder shape, nesting by PATH SEGMENT
// instead of by L-35 id-path.

#pragma once

#include "context/editor/assetdb/asset_database.h"
#include "context/editor/gui/panels/files/files_model.h"

#include <string>
#include <vector>

namespace context::editor::gui::panels::builders
{

// Build the file-tree view model from `paths` — a project-relative, '/'-separated path list (the
// SAME normalized form `filesync::FileStore::list` returns; sorted input yields the deterministic
// first-appearance node order files_model.h documents, but this function does not itself require
// sorted input — it places whatever it is given). Every path NOT an asset candidate
// (assetdb::is_asset_candidate — a `.meta.json` sidecar, atomic-write temp residue, or a
// dot-segment/tool-internal path) is SKIPPED, mirroring the daemon's own asset-index scope: those are
// engine/tool territory, not project files a human browses. `db` supplies each candidate FILE's
// `guid` / `asset_kind` via `find_by_path` — "" for either when the path has no live
// `<asset>.meta.json` sidecar yet (unknown = not enforced, the same seam contract
// AssetDatabase::kind_of documents), which is an ordinary state, not a defect.
[[nodiscard]] files::FilesModel build_files_model(const std::vector<std::string>& paths,
                                                   const assetdb::AssetDatabase& db);

} // namespace context::editor::gui::panels::builders
