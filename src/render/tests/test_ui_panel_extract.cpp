// The M7 a9 world-space UI-panel extract (render_world.h UiPanel + extract.cpp additive walk; D4/L-39).
// A UiPanel is float presentation state (D6) placed by the entity's Transform: this pins that the
// extract picks up every Transform+UiPanel entity into RenderSnapshot::ui_panels, copies its RTT-handle
// / size / tint + transform verbatim, skips a UiPanel WITHOUT a Transform (the absent-component edge,
// mirroring point lights), never disturbs the World (R-REND-003 read-only), and clears/rebuilds
// ui_panels across the L-39 double buffer.

#include "context/render/extract.h"
#include "context/render/render_world.h"

#include "context/kernel/world.h"

#include "render_test.h"

using namespace context::render;
using context::kernel::Entity;
using context::kernel::World;

namespace
{

void test_extract_picks_up_ui_panels()
{
    World world;

    // A: a bound world panel (Transform + UiPanel) — extracted.
    const Entity a = world.create();
    world.add<Transform>(a, Transform{{1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {2.0f, 3.0f, 1.0f}});
    UiPanel pa;
    pa.texture = 7u;
    pa.size[0] = 1.5f;
    pa.size[1] = 0.75f;
    pa.tint[0] = 0.5f;
    pa.tint[3] = 1.0f;
    world.add<UiPanel>(a, pa);

    // B: an UNBOUND panel (texture 0) still has a Transform — extracted (binding is the render side's
    // concern; the extract reports the authored component either way).
    const Entity b = world.create();
    world.add<Transform>(b, Transform{{4.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {1, 1, 1}});
    world.add<UiPanel>(b, UiPanel{}); // texture defaults to 0 (unbound)

    // C: a UiPanel with NO Transform — has no world placement, must be SKIPPED (absent-component edge).
    const Entity c = world.create();
    UiPanel pc;
    pc.texture = 99u;
    world.add<UiPanel>(c, pc);

    // D: a plain drawable (Transform + Renderable) — never a panel.
    const Entity d = world.create();
    world.add<Transform>(d, Transform{});
    world.add<Renderable>(d, Renderable{});

    RenderSnapshot snap;
    extract_render_world(world, 5u, snap);

    CHECK(snap.sim_tick == 5u);
    CHECK(snap.items.size() == 1u);      // D is the only drawable
    CHECK(snap.ui_panels.size() == 2u);  // A and B — NOT C (no Transform)

    const UiPanelItem* pa_item = nullptr;
    const UiPanelItem* pb_item = nullptr;
    for (const UiPanelItem& it : snap.ui_panels)
    {
        CHECK(it.entity != c); // the Transform-less panel never appears
        if (it.entity == a)
        {
            pa_item = &it;
        }
        else if (it.entity == b)
        {
            pb_item = &it;
        }
    }
    CHECK(pa_item != nullptr);
    CHECK(pb_item != nullptr);
    if (pa_item != nullptr)
    {
        // The RTT handle + world size + tint + transform copied verbatim.
        CHECK(pa_item->panel.texture == 7u);
        CHECK(pa_item->panel.size[0] == 1.5f);
        CHECK(pa_item->panel.size[1] == 0.75f);
        CHECK(pa_item->panel.tint[0] == 0.5f);
        CHECK(pa_item->transform.position[0] == 1.0f);
        CHECK(pa_item->transform.position[2] == 3.0f);
        CHECK(pa_item->transform.scale[1] == 3.0f);
    }
    if (pb_item != nullptr)
    {
        CHECK(pb_item->panel.texture == 0u); // unbound panel is still extracted
        CHECK(pb_item->transform.position[0] == 4.0f);
    }
}

void test_extract_reclears_ui_panels()
{
    World world;
    const Entity e = world.create();
    world.add<Transform>(e, Transform{});
    world.add<UiPanel>(e, UiPanel{});

    RenderSnapshot snap;
    snap.ui_panels.push_back(UiPanelItem{}); // pre-existing junk must be cleared
    extract_render_world(world, 1u, snap);
    CHECK(snap.ui_panels.size() == 1u); // exactly the one panel, junk cleared

    // The extract did not disturb the World (still one live entity with its components).
    CHECK(world.alive_count() == 1u);
    CHECK(world.has<Transform>(e));
    CHECK(world.has<UiPanel>(e));
}

void test_ui_panels_ride_the_double_buffer()
{
    World world;
    const Entity e = world.create();
    world.add<Transform>(e, Transform{{9.0f, 0.0f, 0.0f}, {0, 0, 0, 1}, {1, 1, 1}});
    UiPanel p;
    p.texture = 3u;
    world.add<UiPanel>(e, p);

    RenderDoubleBuffer db;
    extract_render_world(world, 0u, db.back());
    CHECK(db.back().ui_panels.size() == 1u);
    db.swap();
    CHECK(db.front().ui_panels.size() == 1u);
    CHECK(db.front().ui_panels[0].panel.texture == 3u);
    // After the swap the new back is cleared (no tearing): its ui_panels are empty until re-extract.
    CHECK(db.back().ui_panels.empty());
}

} // namespace

int main()
{
    test_extract_picks_up_ui_panels();
    test_extract_reclears_ui_panels();
    test_ui_panels_ride_the_double_buffer();
    RENDER_TEST_MAIN_END();
}
