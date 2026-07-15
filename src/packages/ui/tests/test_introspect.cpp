// Unit tests for the headless UI-tree introspection shims (M7 T5 / a5, introspect.h): the pre-order
// live-node walk, name -> node lookup, and the string<->Role bridge that back the `context ui …`
// drive/assert verbs. Pure stdlib, no GPU — the same introspection the CLI serializes, tested here.

#include "context/packages/ui/introspect.h"
#include "context/packages/ui/ui_node.h"
#include "context/packages/ui/ui_tree.h"

#include "ui_test.h"

#include <string>
#include <vector>

using namespace context::packages::ui;

int main()
{
    // --- live_nodes_preorder: document order (parent before children, children in order) -----------
    {
        UiTree tree; // starts with the Root node
        const NodeId panel = tree.create_node(Role::Panel, tree.root());
        const NodeId a = tree.create_node(Role::Label, panel);
        const NodeId b = tree.create_node(Role::Button, panel);
        const NodeId grandchild = tree.create_node(Role::Label, a);

        const std::vector<NodeId> order = live_nodes_preorder(tree);
        // root, panel, a, grandchild, b — pre-order DFS.
        CHECK(order.size() == 5);
        CHECK(order[0] == tree.root());
        CHECK(order[1] == panel);
        CHECK(order[2] == a);
        CHECK(order[3] == grandchild);
        CHECK(order[4] == b);
    }

    // --- a removed subtree drops out of the walk ---------------------------------------------------
    {
        UiTree tree;
        const NodeId panel = tree.create_node(Role::Panel, tree.root());
        const NodeId keep = tree.create_node(Role::Label, panel);
        const NodeId drop = tree.create_node(Role::Group, panel);
        tree.create_node(Role::Label, drop); // a descendant of the to-be-removed node
        CHECK(tree.remove_node(drop));

        const std::vector<NodeId> order = live_nodes_preorder(tree);
        CHECK(order.size() == 3); // root, panel, keep
        for (NodeId id : order)
            CHECK(id != drop);
        CHECK(order[2] == keep);
    }

    // --- find_by_name: first pre-order match; empty name never matches -----------------------------
    {
        UiTree tree;
        const NodeId panel = tree.create_node(Role::Panel, tree.root());
        const NodeId label = tree.create_node(Role::Label, panel);
        tree.node(label)->name = "score-label";
        const NodeId button = tree.create_node(Role::Button, panel);
        tree.node(button)->name = "play-button";

        CHECK(find_by_name(tree, "score-label") == label);
        CHECK(find_by_name(tree, "play-button") == button);
        CHECK(find_by_name(tree, "missing") == kInvalidNode);
        CHECK(find_by_name(tree, "") == kInvalidNode); // unnamed nodes are never matched by empty name
    }

    // --- role_from_name: inverse of role_name(), round-trips the whole closed vocabulary -----------
    {
        for (Role role : {Role::Root, Role::Panel, Role::Group, Role::Label, Role::Button, Role::Image,
                          Role::Slider, Role::Checkbox, Role::TextInput, Role::ProgressBar, Role::List,
                          Role::ListItem})
        {
            Role decoded = Role::Root;
            CHECK(role_from_name(role_name(role), decoded));
            CHECK(decoded == role);
        }
        Role sink = Role::Root;
        CHECK(!role_from_name("not-a-role", sink));
        CHECK(!role_from_name("", sink));
    }

    UI_TEST_MAIN_END();
}
