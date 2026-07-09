// sim->render extract + double-buffer tests (L-39, R-REND-003).

#include "context/render/extract.h"
#include "context/render/render_world.h"

#include "context/kernel/world.h"

#include "render_test.h"

using namespace context::render;
using context::kernel::Entity;
using context::kernel::World;

namespace
{

void test_extract_selects_only_drawables()
{
    World world;

    // A + B are drawable (Transform + Renderable); C is Transform-only; D is Renderable-only; E bare.
    const Entity a = world.create();
    world.add<Transform>(a, Transform{{1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {1, 1, 1}});
    world.add<Renderable>(a, Renderable{{1.0f, 0.0f, 0.0f, 1.0f}, 7u});

    const Entity b = world.create();
    world.add<Transform>(b, Transform{{4.0f, 5.0f, 6.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {2, 2, 2}});
    world.add<Renderable>(b, Renderable{{0.0f, 1.0f, 0.0f, 1.0f}, 9u});

    const Entity c = world.create();
    world.add<Transform>(c, Transform{});

    const Entity d = world.create();
    world.add<Renderable>(d, Renderable{});

    (void)world.create(); // E: no components

    RenderSnapshot snap;
    extract_render_world(world, 42u, snap);

    CHECK(snap.sim_tick == 42u);
    CHECK(snap.items.size() == 2u);

    // Find A and B in the snapshot (archetype order is not guaranteed).
    const RenderItem* item_a = nullptr;
    const RenderItem* item_b = nullptr;
    for (const RenderItem& it : snap.items)
    {
        if (it.entity == a)
        {
            item_a = &it;
        }
        else if (it.entity == b)
        {
            item_b = &it;
        }
    }
    CHECK(item_a != nullptr);
    CHECK(item_b != nullptr);
    if (item_a != nullptr)
    {
        CHECK(item_a->transform.position[0] == 1.0f);
        CHECK(item_a->transform.position[2] == 3.0f);
        CHECK(item_a->renderable.mesh_id == 7u);
        CHECK(item_a->renderable.color[0] == 1.0f);
    }
    if (item_b != nullptr)
    {
        CHECK(item_b->transform.scale[0] == 2.0f);
        CHECK(item_b->renderable.mesh_id == 9u);
        CHECK(item_b->renderable.color[1] == 1.0f);
    }

    // C, D, E must NOT appear (only entities carrying BOTH components are drawables).
    for (const RenderItem& it : snap.items)
    {
        CHECK(it.entity != c);
        CHECK(it.entity != d);
    }
}

void test_extract_is_read_only_and_reclears()
{
    World world;
    const Entity e = world.create();
    world.add<Transform>(e, Transform{});
    world.add<Renderable>(e, Renderable{});

    RenderSnapshot snap;
    snap.items.push_back(RenderItem{}); // pre-existing junk must be cleared
    snap.sim_tick = 999u;

    extract_render_world(world, 1u, snap);
    CHECK(snap.items.size() == 1u); // exactly the one drawable, junk cleared
    CHECK(snap.sim_tick == 1u);

    // The extract did not disturb the World (still one live entity with its components).
    CHECK(world.alive_count() == 1u);
    CHECK(world.has<Transform>(e));
    CHECK(world.has<Renderable>(e));
}

void test_double_buffer_no_tearing()
{
    World world;
    const Entity e = world.create();
    world.add<Transform>(e, Transform{{9.0f, 0.0f, 0.0f}, {0, 0, 0, 1}, {1, 1, 1}});
    world.add<Renderable>(e, Renderable{});

    RenderDoubleBuffer db;
    // Frame 0: extract into back, then publish.
    extract_render_world(world, 0u, db.back());
    CHECK(db.back().items.size() == 1u);
    db.swap();
    CHECK(db.front().items.size() == 1u);
    CHECK(db.front().sim_tick == 0u);

    // After swap the new back is CLEARED (ready for the next extract) — the front (being read) is
    // untouched while the back is rewritten: no tearing.
    CHECK(db.back().items.empty());

    // Frame 1: extract a fresh tick into back; the front still shows frame 0 until the next swap.
    extract_render_world(world, 1u, db.back());
    CHECK(db.front().sim_tick == 0u); // render side still sees the previous, complete frame
    CHECK(db.back().sim_tick == 1u);
    db.swap();
    CHECK(db.front().sim_tick == 1u);
}

} // namespace

int main()
{
    test_extract_selects_only_drawables();
    test_extract_is_read_only_and_reclears();
    test_double_buffer_no_tearing();
    RENDER_TEST_MAIN_END();
}
