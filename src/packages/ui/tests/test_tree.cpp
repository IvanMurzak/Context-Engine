// Tree build/mutate (M7 T1, R-UI-006): create/parent/reparent/remove nodes, mutate style/text/bounds,
// and the closed role vocabulary — all headless, no renderer.

#include "context/packages/ui/ui_tree.h"

#include "ui_test.h"

#include <cstring>

using namespace context::packages::ui;

int main()
{
    // --- a fresh tree is just a Root -------------------------------------------------------------
    UiTree tree;
    CHECK(tree.node_count() == 1);
    CHECK(tree.root() != kInvalidNode);
    const UiNode* root = tree.node(tree.root());
    CHECK(root != nullptr);
    CHECK(root->role == Role::Root);
    CHECK(root->parent == kInvalidNode);
    CHECK(tree.descendant_count(tree.root()) == 0);

    // --- build: panel > (label, button) ----------------------------------------------------------
    const NodeId panel = tree.create_node(Role::Panel, tree.root());
    const NodeId label = tree.create_node(Role::Label, panel);
    const NodeId button = tree.create_node(Role::Button, panel);
    CHECK(panel != kInvalidNode && label != kInvalidNode && button != kInvalidNode);
    CHECK(tree.node_count() == 4);
    CHECK(tree.descendant_count(tree.root()) == 3);
    CHECK(tree.descendant_count(panel) == 2);
    CHECK(tree.node(label)->parent == panel);
    CHECK(tree.node(button)->parent == panel);

    // handles are stable + distinct; ids are not reused
    CHECK(panel != label && label != button);

    // creating under a dead/invalid parent fails
    CHECK(tree.create_node(Role::Group, kInvalidNode) == kInvalidNode);

    // --- mutate: style / text / bounds -----------------------------------------------------------
    CHECK(tree.set_text(label, "Score: 0"));
    CHECK(tree.node(label)->text == "Score: 0");

    Style s;
    s.opacity = 0.5f;
    s.visible = true;
    s.background = Color{10, 20, 30, 255};
    CHECK(tree.set_style(panel, s));
    CHECK((tree.node(panel)->style.background == Color{10, 20, 30, 255})); // extra parens: braces hold commas

    CHECK(tree.set_bounds(button, Rect{4, 4, 40, 12}));
    CHECK(tree.node(button)->bounds.w == 40.0f);

    CHECK(tree.set_visible(button, false));
    CHECK(tree.node(button)->style.visible == false);

    // mutating an invalid id fails cleanly
    CHECK(!tree.set_text(kInvalidNode, "x"));

    // --- reparent: move label under button -------------------------------------------------------
    CHECK(tree.reparent(label, button));
    CHECK(tree.node(label)->parent == button);
    CHECK(tree.descendant_count(panel) == 2); // button + label (label now nested under button)
    CHECK(tree.descendant_count(button) == 1);

    // illegal reparents are refused
    CHECK(!tree.reparent(tree.root(), panel));    // can't move the root
    CHECK(!tree.reparent(panel, panel));          // can't parent to self
    CHECK(!tree.reparent(panel, label));          // can't move into own descendant (panel > button > label)
    CHECK(!tree.reparent(kInvalidNode, panel));   // invalid node

    // --- remove: button subtree (button + label) goes away ---------------------------------------
    CHECK(tree.remove_node(button));
    CHECK(tree.node(button) == nullptr);
    CHECK(tree.node(label) == nullptr); // descendant removed with it
    CHECK(tree.node_count() == 2);      // root + panel
    CHECK(tree.descendant_count(panel) == 0);
    CHECK(!tree.remove_node(tree.root())); // the root cannot be removed
    CHECK(!tree.remove_node(button));      // already dead

    // --- the closed role vocabulary maps to stable names -----------------------------------------
    CHECK(std::strcmp(role_name(Role::Root), "root") == 0);
    CHECK(std::strcmp(role_name(Role::Panel), "panel") == 0);
    CHECK(std::strcmp(role_name(Role::Button), "button") == 0);
    CHECK(std::strcmp(role_name(Role::ProgressBar), "progressbar") == 0);
    CHECK(std::strcmp(role_name(Role::ListItem), "listitem") == 0);

    UI_TEST_MAIN_END();
}
