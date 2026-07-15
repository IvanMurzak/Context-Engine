// Headless layout + hit-testing + focus order (M7 T2, R-UI-006/003): computed rects from the flow /
// absolute layout model, resize/reflow driving a1's damage, top-most visibility/opacity-aware hit
// testing (overlap / nesting / hidden / zero-opacity), and deterministic document-order focus. No GPU.

#include "context/packages/ui/layout.h"
#include "context/packages/ui/ui_tree.h"

#include "ui_test.h"

using namespace context::packages::ui;

int main()
{
    // --- Column flow: padding-inset content box, gap-spaced children, explicit sizes ---------------
    {
        UiTree tree;
        Style rs;
        rs.padding = 10.0f;
        CHECK(tree.set_style(tree.root(), rs));
        Layout col;
        col.flow = Flow::Column;
        col.gap = 4.0f;
        CHECK(tree.set_layout(tree.root(), col));

        const NodeId a = tree.create_node(Role::Panel, tree.root());
        const NodeId b = tree.create_node(Role::Panel, tree.root());
        Layout la;
        la.size = Vec2{30.0f, 20.0f};
        Layout lb;
        lb.size = Vec2{30.0f, 25.0f};
        CHECK(tree.set_layout(a, la));
        CHECK(tree.set_layout(b, lb));

        compute_layout(tree, Rect{0, 0, 100, 200});

        // root fills the viewport; content box = viewport inset by padding 10 = {10,10,80,180}
        CHECK((tree.node(tree.root())->bounds == Rect{0, 0, 100, 200}));
        // column pen starts at content.y=10: a occupies [10,30), then +gap4 → b starts at y=34
        CHECK((tree.node(a)->bounds == Rect{10, 10, 30, 20}));
        CHECK((tree.node(b)->bounds == Rect{10, 34, 30, 25}));
    }

    // --- Row flow: cross-axis (height) stretches to the content box when a child leaves it unset ----
    {
        UiTree tree;
        Layout row;
        row.flow = Flow::Row;
        row.gap = 5.0f;
        CHECK(tree.set_layout(tree.root(), row));

        const NodeId x = tree.create_node(Role::Panel, tree.root());
        const NodeId y = tree.create_node(Role::Panel, tree.root());
        Layout lx;
        lx.size = Vec2{40.0f, 0.0f}; // height unset → stretch to content height
        Layout ly;
        ly.size = Vec2{60.0f, 0.0f};
        CHECK(tree.set_layout(x, lx));
        CHECK(tree.set_layout(y, ly));

        compute_layout(tree, Rect{0, 0, 200, 50});

        CHECK((tree.node(x)->bounds == Rect{0, 0, 40, 50}));  // stretched height 50
        CHECK((tree.node(y)->bounds == Rect{45, 0, 60, 50})); // 40 + gap5 = 45
    }

    // --- Absolute anchoring: pinned edge, opposing-edge stretch, and centered --------------------
    {
        UiTree tree;

        // bottom-right pinned with a 5px margin
        const NodeId br = tree.create_node(Role::Panel, tree.root());
        Layout lbr;
        lbr.position = Positioning::Absolute;
        lbr.anchor = Anchor::Right | Anchor::Bottom;
        lbr.size = Vec2{20.0f, 10.0f};
        lbr.offset = Vec2{5.0f, 5.0f};
        CHECK(tree.set_layout(br, lbr));

        // stretched to fill (both edges of each axis) with an 8px symmetric margin — size ignored
        const NodeId st = tree.create_node(Role::Panel, tree.root());
        Layout lst;
        lst.position = Positioning::Absolute;
        lst.anchor = Anchor::Left | Anchor::Right | Anchor::Top | Anchor::Bottom;
        lst.offset = Vec2{8.0f, 8.0f};
        CHECK(tree.set_layout(st, lst));

        // centered (no horizontal/vertical anchor) with no offset
        const NodeId cen = tree.create_node(Role::Panel, tree.root());
        Layout lcen;
        lcen.position = Positioning::Absolute;
        lcen.size = Vec2{20.0f, 20.0f};
        CHECK(tree.set_layout(cen, lcen));

        compute_layout(tree, Rect{0, 0, 100, 100});

        CHECK((tree.node(br)->bounds == Rect{75, 85, 20, 10}));  // 100-5-20, 100-5-10
        CHECK((tree.node(st)->bounds == Rect{8, 8, 84, 84}));    // 100-2*8 each axis
        CHECK((tree.node(cen)->bounds == Rect{40, 40, 20, 20})); // (100-20)/2 each axis
    }

    // --- resize/reflow → damage propagation (layout drives a1's damage); idempotent = no damage ----
    {
        UiTree tree;
        Layout col;
        col.flow = Flow::Column;
        CHECK(tree.set_layout(tree.root(), col));
        const NodeId a = tree.create_node(Role::Panel, tree.root());
        const NodeId b = tree.create_node(Role::Panel, tree.root());
        Layout la;
        la.size = Vec2{50.0f, 10.0f};
        Layout lb;
        lb.size = Vec2{50.0f, 10.0f};
        CHECK(tree.set_layout(a, la));
        CHECK(tree.set_layout(b, lb));

        compute_layout(tree, Rect{0, 0, 100, 100});
        CHECK((tree.node(a)->bounds == Rect{0, 0, 50, 10}));
        CHECK((tree.node(b)->bounds == Rect{0, 10, 50, 10}));
        (void)tree.take_damage(); // clear the build + first-layout damage

        // re-running with identical inputs must NOT re-damage (set_bounds only fires on a change)
        compute_layout(tree, Rect{0, 0, 100, 100});
        CHECK(tree.take_damage().empty());

        // grow a's height 10 → 30: a's rect changes AND b reflows downward
        la.size = Vec2{50.0f, 30.0f};
        CHECK(tree.set_layout(a, la));
        compute_layout(tree, Rect{0, 0, 100, 100});
        CHECK((tree.node(a)->bounds == Rect{0, 0, 50, 30}));
        CHECK((tree.node(b)->bounds == Rect{0, 30, 50, 10})); // shifted from y=10 to y=30

        const DamageList d = tree.take_damage();
        CHECK(!d.empty()); // the reflow produced region damage
        CHECK(!d.full);    // a resize is region damage, not a structural full-surface repaint
    }

    // --- hit-testing: overlap (top-most sibling), nesting (deepest), hidden + zero-opacity culls ----
    {
        UiTree tree;
        const NodeId a = tree.create_node(Role::Panel, tree.root());
        const NodeId b = tree.create_node(Role::Panel, tree.root()); // painted after a → on top
        CHECK(tree.set_bounds(a, Rect{0, 0, 50, 50}));
        CHECK(tree.set_bounds(b, Rect{25, 0, 50, 50})); // overlaps a on x∈[25,50)

        CHECK(hit_test(tree, 30.0f, 10.0f) == b); // overlap → the later sibling wins
        CHECK(hit_test(tree, 10.0f, 10.0f) == a); // only a covers this
        CHECK(hit_test(tree, 200.0f, 200.0f) == kInvalidNode); // outside everything (root unset)

        // nesting: a child fully inside a
        const NodeId c = tree.create_node(Role::Label, a);
        CHECK(tree.set_bounds(c, Rect{5, 5, 10, 10}));
        CHECK(hit_test(tree, 8.0f, 8.0f) == c);   // deepest node wins
        CHECK(hit_test(tree, 20.0f, 40.0f) == a); // inside a but not c, and left of b (x<25)

        // hidden node is not hittable — the point falls through to what's beneath it
        CHECK(tree.set_visible(b, false));
        CHECK(hit_test(tree, 30.0f, 10.0f) == a); // b hidden → a underneath

        // zero-opacity culls the node AND its subtree
        Style transparent;
        transparent.opacity = 0.0f;
        CHECK(tree.set_style(a, transparent));
        CHECK(hit_test(tree, 8.0f, 8.0f) == kInvalidNode); // a (opacity 0) culls child c too
    }

    // --- hit-testing: the root is the background hit when a point misses every child --------------
    {
        UiTree tree;
        compute_layout(tree, Rect{0, 0, 100, 100}); // gives the root real bounds
        const NodeId p = tree.create_node(Role::Panel, tree.root());
        CHECK(tree.set_bounds(p, Rect{10, 10, 20, 20}));

        CHECK(hit_test(tree, 15.0f, 15.0f) == p);              // over the panel
        CHECK(hit_test(tree, 50.0f, 50.0f) == tree.root());    // over the root background only
        CHECK(hit_test(tree, 150.0f, 150.0f) == kInvalidNode); // outside the viewport
    }

    // --- deterministic focus order: document order, focusable roles only, hidden subtree culled ----
    {
        UiTree tree;
        const NodeId panel = tree.create_node(Role::Panel, tree.root());     // not focusable
        const NodeId label = tree.create_node(Role::Label, panel);           // not focusable
        const NodeId button = tree.create_node(Role::Button, panel);         // focusable
        const NodeId slider = tree.create_node(Role::Slider, tree.root());   // focusable
        const NodeId hidden = tree.create_node(Role::Group, tree.root());    // container, hidden below
        const NodeId buried = tree.create_node(Role::Button, hidden);        // focusable but culled
        CHECK(label != kInvalidNode && buried != kInvalidNode);
        CHECK(tree.set_visible(hidden, false));

        const std::vector<NodeId> order = focus_order(tree);
        CHECK(order.size() == 2);
        CHECK(order[0] == button); // panel's subtree comes before slider in document order
        CHECK(order[1] == slider);

        // a focusable node that is itself hidden drops out
        const NodeId ghost = tree.create_node(Role::Checkbox, tree.root());
        CHECK(tree.set_visible(ghost, false));
        CHECK(focus_order(tree).size() == 2); // unchanged — ghost is invisible

        // the role predicate directly
        CHECK(is_focusable(Role::Button));
        CHECK(is_focusable(Role::TextInput));
        CHECK(is_focusable(Role::ListItem));
        CHECK(!is_focusable(Role::Label));
        CHECK(!is_focusable(Role::Panel));
        CHECK(!is_focusable(Role::Root));
    }

    UI_TEST_MAIN_END();
}
