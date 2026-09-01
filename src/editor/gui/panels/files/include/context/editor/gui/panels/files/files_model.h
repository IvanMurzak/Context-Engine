// The project FILE TREE view model (M9 e1, D10): the panel-facing, boundary-clean hierarchy the
// Files panel renders. BOUNDARY-CLEAN by construction (D10, same discipline as
// gui/panels/scenetree/scene_tree_model.h): plain data only — no assetdb::/filesync:: type appears
// here, so this panel library is Shell-hostable under the D10 shell-boundary gate. The kernel-typed
// builder that walks the project's real files + the asset database's live GUID index into this tree
// lives on the kernel side of the wire: gui/panels/builders/files_builder.h
// (context_gui_panel_builders). Read-only: an observer view of the project's file tree, never a
// write path (the write half — rename/move/delete — is task e2).

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace context::editor::gui::panels::files
{

// What a node represents in the project file tree.
enum class FileNodeKind
{
    file,      // a regular file
    directory, // a folder — a grouping node with children, never independently selectable
};

// One node of the project file tree. `identity` is the project-relative path, normalized with '/'
// separators — the row identity the panel selects/keys by (the L-35-adjacent identity the design
// names: stable and unique across the tree, but a plain path rather than an L-35 id-path since a
// file has no composition to address). `guid` / `asset_kind` are populated from the asset
// database's live index; both are "" for a directory, and both are ALSO "" for a file that is a
// candidate asset but has not been given an `<asset>.meta.json` sidecar yet — "unknown = not
// enforced" is the seam's own contract (assetdb::AssetDatabase::kind_of), not a defect here.
struct FileNode
{
    std::string identity;
    std::string display_name; // basename
    FileNodeKind kind = FileNodeKind::file;
    std::string guid;
    std::string asset_kind;
    std::vector<FileNode> children; // only a directory node has children
};

// The project file tree the panel renders. Deterministic — node order is FIRST-APPEARANCE order over
// the (already sorted) input path list, the same discipline scene_tree_builder.h's TreeBuilder
// applies, so a re-read of an unchanged project re-projects byte-identically. Not a re-sorted
// alphabetical tree: a file whose name sorts before a same-prefixed directory (e.g. "a.txt" before
// "a/x.txt", since '.' < '/') can appear before that directory at the same level — deterministic
// either way, just not a second alphabetical pass over the built hierarchy.
struct FilesModel
{
    bool ok = true;               // false iff the underlying read could not enumerate the project
    std::size_t file_count = 0;   // number of FILE rows (directories are not counted)
    std::vector<FileNode> roots;  // top-level nodes (files/directories directly under the project root)
};

// Depth-first search for the node whose `identity` equals `identity` (the selection key). Returns
// nullptr when absent. Searches the whole forest — mirrors scenetree::find_node.
[[nodiscard]] const FileNode* find_node(const FilesModel& model, const std::string& identity);

} // namespace context::editor::gui::panels::files
