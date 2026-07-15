// UI extract (context/render/ui/snapshot.h): the retained tree -> flat draw quads observer, and the
// L-39 double buffer. Covers drawable selection (visible + non-transparent + non-empty), pre-order
// paint order, composited transform + composed opacity in the snapshot, and no-tear double buffering.

#include "context/render/ui/snapshot.h"

#include "context/packages/ui/ui_tree.h"

#include "render_test.h"

using namespace context::render::ui;
using namespace context::packages::ui;

namespace
{

bool rect_eq(const Rect& a, const Rect& b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// Build a small tree: a visible red panel (translated), an INVISIBLE green panel (pruned), and a
// transparent container (no quad) with a visible blue child. Returns the ids by out-params.
void build(UiTree& tree, NodeId& red, NodeId& invisible, NodeId& container, NodeId& blue)
{
    const NodeId root = tree.root();

    red = tree.create_node(Role::Panel, root);
    tree.set_bounds(red, Rect{10, 10, 50, 30});
    Style rs;
    rs.background = Color{255, 0, 0, 255};
    rs.transform.translate = {5.0f, 5.0f}; // composited translate ⇒ quad rect shifts, no relayout
    tree.set_style(red, rs);

    invisible = tree.create_node(Role::Panel, root);
    tree.set_bounds(invisible, Rect{0, 0, 10, 10});
    Style is;
    is.background = Color{0, 255, 0, 255};
    is.visible = false;
    tree.set_style(invisible, is);

    container = tree.create_node(Role::Group, root); // default transparent background ⇒ no quad
    tree.set_bounds(container, Rect{100, 100, 40, 40});

    blue = tree.create_node(Role::Panel, container);
    tree.set_bounds(blue, Rect{100, 100, 20, 20});
    Style bs;
    bs.background = Color{0, 0, 255, 255};
    tree.set_style(blue, bs);
}

void test_extract_selects_drawables_in_paint_order()
{
    UiTree tree;
    NodeId red, invisible, container, blue;
    build(tree, red, invisible, container, blue);

    UiRenderSnapshot snap;
    extract_ui(tree, Rect{0, 0, 256, 256}, snap);

    // Only the two drawables (red panel + blue child); the invisible panel and the transparent
    // container contribute nothing.
    CHECK(snap.quads.size() == 2);
    // Pre-order: red before blue (blue is under a later sibling). Painter order == emission order.
    CHECK(snap.quads[0].node == red);
    CHECK(snap.quads[1].node == blue);
    CHECK(snap.quads[0].order == 0);
    CHECK(snap.quads[1].order == 1);
    CHECK((snap.quads[0].color == Color{255, 0, 0, 255}));
    CHECK((snap.quads[1].color == Color{0, 0, 255, 255}));
    // The composited translate is baked into the rect (10,10 + 5,5 -> 15,15), size unchanged.
    CHECK(rect_eq(snap.quads[0].rect, Rect{15, 15, 50, 30}));
    CHECK(rect_eq(snap.quads[1].rect, Rect{100, 100, 20, 20}));
    CHECK(rect_eq(snap.surface, Rect{0, 0, 256, 256}));
}

void test_extract_composes_opacity_down_the_tree()
{
    UiTree tree;
    NodeId red, invisible, container, blue;
    build(tree, red, invisible, container, blue);

    // Fade the (bg-transparent) container to 0.5 and the child to 0.5 -> the child quad's effective
    // opacity is the product 0.25, applied WITHOUT any relayout.
    Style cs;
    cs.opacity = 0.5f; // container stays background-transparent ⇒ still no quad, but composes opacity
    tree.set_style(container, cs);
    Style bs;
    bs.background = Color{0, 0, 255, 255};
    bs.opacity = 0.5f;
    tree.set_style(blue, bs);

    UiRenderSnapshot snap;
    extract_ui(tree, Rect{0, 0, 256, 256}, snap);
    CHECK(snap.quads.size() == 2);
    CHECK(snap.quads[0].opacity == 1.0f);  // the red panel, unfaded
    CHECK(snap.quads[1].opacity == 0.25f); // blue child under the faded container

    // A fully-transparent opacity prunes the subtree (the child drops out).
    Style hidden;
    hidden.opacity = 0.0f;
    tree.set_style(container, hidden);
    extract_ui(tree, Rect{0, 0, 256, 256}, snap);
    CHECK(snap.quads.size() == 1);
    CHECK(snap.quads[0].node == red);
}

void test_double_buffer_publishes_latest_without_tearing()
{
    UiTree tree;
    NodeId red, invisible, container, blue;
    build(tree, red, invisible, container, blue);

    UiRenderDoubleBuffer db;
    db.extract(tree, Rect{0, 0, 256, 256});
    CHECK(db.front().quads.size() == 2);
    CHECK(db.front().generation == 1);

    // Structural change (remove the red panel) then re-extract: the published front reflects it, and
    // the generation stamp advances (proof the buffers rotated).
    CHECK(tree.remove_node(red));
    db.extract(tree, Rect{0, 0, 256, 256});
    CHECK(db.front().quads.size() == 1);
    CHECK(db.front().quads[0].node == blue);
    CHECK(db.front().generation == 2);
}

} // namespace

int main()
{
    test_extract_selects_drawables_in_paint_order();
    test_extract_composes_opacity_down_the_tree();
    test_double_buffer_publishes_latest_without_tearing();
    RENDER_TEST_MAIN_END();
}
