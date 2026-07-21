// Scene-tree view-model queries. The kernel-typed BUILDER (flat composed entities -> nested
// hierarchy) moved to gui/panels/builders/scene_tree_builder.cpp (M9 e05d3, D10) so this library
// stays boundary-clean; what remains here is pure over the plain model.

#include "context/editor/gui/panels/scenetree/scene_tree_model.h"

#include <string>

namespace context::editor::gui::panels::scenetree
{

namespace
{

[[nodiscard]] const SceneTreeNode* find_in(const SceneTreeNode& node, const std::string& identity)
{
    if (node.identity == identity)
    {
        return &node;
    }
    for (const SceneTreeNode& child : node.children)
    {
        if (const SceneTreeNode* hit = find_in(child, identity))
        {
            return hit;
        }
    }
    return nullptr;
}

} // namespace

const SceneTreeNode* find_node(const SceneTreeModel& model, const std::string& identity)
{
    for (const SceneTreeNode& root : model.roots)
    {
        if (const SceneTreeNode* hit = find_in(root, identity))
        {
            return hit;
        }
    }
    return nullptr;
}

} // namespace context::editor::gui::panels::scenetree
