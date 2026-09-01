// Pure data helpers over FilesModel (see files_model.h). Mirrors
// gui/panels/scenetree/src/scene_tree_model.cpp — the kernel-typed builder that PRODUCES a model
// lives in gui/panels/builders/files_builder.cpp (M9 e1, D10); this file only searches it.

#include "context/editor/gui/panels/files/files_model.h"

namespace context::editor::gui::panels::files
{

namespace
{

[[nodiscard]] const FileNode* find_in(const FileNode& node, const std::string& identity)
{
    if (node.identity == identity)
    {
        return &node;
    }
    for (const FileNode& child : node.children)
    {
        if (const FileNode* hit = find_in(child, identity))
        {
            return hit;
        }
    }
    return nullptr;
}

} // namespace

const FileNode* find_node(const FilesModel& model, const std::string& identity)
{
    for (const FileNode& root : model.roots)
    {
        if (const FileNode* hit = find_in(root, identity))
        {
            return hit;
        }
    }
    return nullptr;
}

} // namespace context::editor::gui::panels::files
