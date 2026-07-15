// UI draw-order / batching / damage draw-set selection (context/render/ui/batch.h). The DoD
// "damage -> minimal draw set" STRUCTURAL draw-count assertions: a full repaint draws every quad, a
// damage repaint draws ONLY the quads overlapping the dirty regions.

#include "context/render/ui/batch.h"

#include "context/packages/ui/provider.h" // RepaintPlan
#include "context/packages/ui/ui_tree.h"
#include "context/render/ui/hud_scene.h"
#include "context/render/ui/snapshot.h"

#include "render_test.h"

using namespace context::render::ui;
using namespace context::packages::ui;

namespace
{

UiQuad make_quad(float x, float y, float w, float h, bool opaque, std::uint32_t order)
{
    UiQuad q;
    q.rect = Rect{x, y, w, h};
    q.color = Color{200, 200, 200, static_cast<std::uint8_t>(opaque ? 255 : 128)};
    q.opacity = opaque ? 1.0f : 0.5f;
    q.order = order;
    return q;
}

UiRenderSnapshot three_separated_quads()
{
    UiRenderSnapshot snap;
    snap.surface = Rect{0, 0, 256, 256};
    snap.quads.push_back(make_quad(0, 0, 10, 10, true, 0));    // 0
    snap.quads.push_back(make_quad(20, 20, 10, 10, true, 1));  // 1
    snap.quads.push_back(make_quad(40, 40, 10, 10, true, 2));  // 2
    return snap;
}

void test_draw_order_and_batching()
{
    UiRenderSnapshot snap = three_separated_quads();

    const std::vector<std::uint32_t> order = sort_ui_draw_order(snap);
    CHECK(order.size() == 3);
    CHECK(order[0] == 0 && order[1] == 1 && order[2] == 2);

    // All opaque ⇒ one coalesced batch of all three (adjacent same-key run).
    std::vector<UiBatch> batches = build_ui_batches(snap);
    CHECK(batches.size() == 1);
    CHECK(batches[0].opaque);
    CHECK(batches[0].quad_indices.size() == 3);

    // Insert a translucent quad in the middle ⇒ a batch boundary (opaque | translucent | opaque).
    snap.quads.insert(snap.quads.begin() + 1, make_quad(60, 60, 10, 10, /*opaque=*/false, /*order=*/1));
    snap.quads[2].order = 2; // keep the tail order strictly increasing
    snap.quads[3].order = 3;
    batches = build_ui_batches(snap);
    CHECK(batches.size() == 3);
    CHECK(batches[0].opaque && batches[0].quad_indices.size() == 1);
    CHECK(!batches[1].opaque && batches[1].quad_indices.size() == 1);
    CHECK(batches[2].opaque && batches[2].quad_indices.size() == 2);
}

void test_full_repaint_draws_everything()
{
    const UiRenderSnapshot snap = three_separated_quads();
    RepaintPlan plan;
    plan.full_repaint = true;
    const std::vector<std::uint32_t> set = select_draw_set(snap, plan);
    CHECK(set.size() == 3); // the whole surface — the fallback / first-frame path
}

void test_damage_selects_only_overlapping_quads()
{
    const UiRenderSnapshot snap = three_separated_quads();

    // A dirty region over quad 1 only ⇒ the MINIMAL draw set is just {1}.
    RepaintPlan one;
    one.regions.push_back(Rect{22, 22, 4, 4});
    const std::vector<std::uint32_t> got_one = select_draw_set(snap, one);
    CHECK(got_one.size() == 1);
    CHECK(got_one[0] == 1);

    // Two dirty regions over quads 0 and 2 ⇒ {0,2}, still fewer than the full set, in draw order.
    RepaintPlan two;
    two.regions.push_back(Rect{5, 5, 2, 2});
    two.regions.push_back(Rect{45, 45, 2, 2});
    const std::vector<std::uint32_t> got_two = select_draw_set(snap, two);
    CHECK(got_two.size() == 2);
    CHECK(got_two[0] == 0 && got_two[1] == 2);

    // No damage at all (not full, no regions) ⇒ nothing to redraw.
    RepaintPlan none;
    CHECK(select_draw_set(snap, none).empty());

    // Half-open: a region merely TOUCHING a quad's far edge does not select it (no intersection).
    RepaintPlan touch;
    touch.regions.push_back(Rect{10, 0, 5, 5}); // quad 0 spans [0,10) — x==10 is the excluded edge
    CHECK(select_draw_set(snap, touch).empty());
}

void test_hud_damage_is_a_strict_subset()
{
    // A realistic damage repaint over the reference HUD: dirtying the health-fill region redraws only
    // the bar background + the fill (the quads it overlaps), not the minimap / badge / status bar.
    UiTree tree;
    build_reference_hud(tree);
    UiRenderSnapshot snap;
    extract_ui(tree, Rect{0, 0, 256, 256}, snap);
    CHECK(snap.quads.size() == 5);

    RepaintPlan full;
    full.full_repaint = true;
    CHECK(select_draw_set(snap, full).size() == 5);

    RepaintPlan fill_damage;
    fill_damage.regions.push_back(Rect{18, 18, 80, 16}); // the health fill's bounds
    const std::vector<std::uint32_t> damaged = select_draw_set(snap, fill_damage);
    CHECK(damaged.size() == 2); // bar background + fill only — strictly fewer than all 5
    CHECK(damaged.size() < snap.quads.size());
}

} // namespace

int main()
{
    test_draw_order_and_batching();
    test_full_repaint_draws_everything();
    test_damage_selects_only_overlapping_quads();
    test_hud_damage_is_a_strict_subset();
    RENDER_TEST_MAIN_END();
}
