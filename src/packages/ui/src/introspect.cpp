// Headless UI-tree introspection shims (see introspect.h).

#include "context/packages/ui/introspect.h"

#include "context/packages/ui/ui_tree.h"

#include <string>
#include <vector>

namespace context::packages::ui
{

namespace
{
// Append `id` then recurse into its live children, pre-order. The tree's UiNode::children vector is the
// document order; a dead child is skipped (a live node never links a dead child, but the guard keeps the
// walk robust to a tombstone in the store).
void walk_preorder(const UiTree& tree, NodeId id, std::vector<NodeId>& out)
{
    const UiNode* n = tree.node(id);
    if (n == nullptr || !n->alive)
        return;
    out.push_back(id);
    for (NodeId child : n->children)
        walk_preorder(tree, child, out);
}
} // namespace

std::vector<NodeId> live_nodes_preorder(const UiTree& tree)
{
    std::vector<NodeId> out;
    walk_preorder(tree, tree.root(), out);
    return out;
}

NodeId find_by_name(const UiTree& tree, const std::string& name)
{
    if (name.empty())
        return kInvalidNode;
    for (NodeId id : live_nodes_preorder(tree))
    {
        const UiNode* n = tree.node(id);
        if (n != nullptr && n->name == name)
            return id;
    }
    return kInvalidNode;
}

bool role_from_name(const std::string& name, Role& out) noexcept
{
    // Exhaustive over the closed Role vocabulary (ui_node.h). Mirrors role_name() the other direction;
    // kept in lockstep with it (a new role added there needs a row here).
    static constexpr Role kAll[] = {Role::Root,     Role::Panel,       Role::Group,
                                    Role::Label,    Role::Button,      Role::Image,
                                    Role::Slider,   Role::Checkbox,    Role::TextInput,
                                    Role::ProgressBar, Role::List,      Role::ListItem};
    for (Role role : kAll)
    {
        if (name == role_name(role))
        {
            out = role;
            return true;
        }
    }
    return false;
}

} // namespace context::packages::ui
