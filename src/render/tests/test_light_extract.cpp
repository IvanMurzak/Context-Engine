// Light + PBR-material extraction (R-REND-004, R-REND-003 one-way): directional/point lights and
// per-drawable materials are copied out of the sim World into the RenderSnapshot, including the
// malformed/absent edge paths (zero-direction directional light, point light without a Transform,
// drawable without a material) and the double-buffer clearing semantics for the new vectors.

#include "context/render/extract.h"
#include "context/render/render_world.h"

#include "context/kernel/world.h"

#include "render_test.h"

using namespace context::render;
using context::kernel::Entity;
using context::kernel::World;

namespace
{

void test_directional_light_extracted_and_normalized()
{
    World world;
    const Entity sun = world.create();
    DirectionalLight light;
    light.direction[0] = 0.0f;
    light.direction[1] = -2.0f; // deliberately unnormalized
    light.direction[2] = 0.0f;
    light.color[0] = 0.5f;
    light.intensity = 2.0f;
    light.cast_shadows = false;
    world.add<DirectionalLight>(sun, light);

    RenderSnapshot snap;
    extract_render_world(world, 7u, snap);

    CHECK(snap.directional_lights.size() == 1u);
    CHECK(snap.point_lights.empty());
    CHECK(snap.items.empty());
    if (!snap.directional_lights.empty())
    {
        const DirectionalLightItem& item = snap.directional_lights.front();
        CHECK(item.entity == sun);
        CHECK(item.light.direction[0] == 0.0f);
        CHECK(item.light.direction[1] == -1.0f); // normalized by the extract
        CHECK(item.light.direction[2] == 0.0f);
        CHECK(item.light.color[0] == 0.5f);
        CHECK(item.light.intensity == 2.0f);
        CHECK(item.light.cast_shadows == false);
    }

    // Extraction is read-only: the World still holds the AUTHORED (unnormalized) component.
    const DirectionalLight* stored = world.get<DirectionalLight>(sun);
    CHECK(stored != nullptr);
    if (stored != nullptr)
    {
        CHECK(stored->direction[1] == -2.0f);
    }
}

void test_zero_direction_light_is_skipped()
{
    World world;
    const Entity bad = world.create();
    DirectionalLight light;
    light.direction[0] = 0.0f;
    light.direction[1] = 0.0f;
    light.direction[2] = 0.0f; // malformed: no light frame can be derived
    world.add<DirectionalLight>(bad, light);

    const Entity good = world.create();
    world.add<DirectionalLight>(good, DirectionalLight{});

    RenderSnapshot snap;
    extract_render_world(world, 1u, snap);

    // Only the well-formed light survives the extract.
    CHECK(snap.directional_lights.size() == 1u);
    if (!snap.directional_lights.empty())
    {
        CHECK(snap.directional_lights.front().entity == good);
    }
}

void test_point_light_requires_transform()
{
    World world;

    // A point light WITHOUT a Transform has no position — skipped (the absent-component edge).
    const Entity floating = world.create();
    world.add<PointLight>(floating, PointLight{});

    // With a Transform the position rides along.
    const Entity lamp = world.create();
    Transform t;
    t.position[0] = 1.0f;
    t.position[1] = 2.0f;
    t.position[2] = 3.0f;
    world.add<Transform>(lamp, t);
    PointLight p;
    p.intensity = 4.0f;
    p.range = 9.0f;
    world.add<PointLight>(lamp, p);

    RenderSnapshot snap;
    extract_render_world(world, 1u, snap);

    CHECK(snap.point_lights.size() == 1u);
    if (!snap.point_lights.empty())
    {
        const PointLightItem& item = snap.point_lights.front();
        CHECK(item.entity == lamp);
        CHECK(item.position[0] == 1.0f);
        CHECK(item.position[1] == 2.0f);
        CHECK(item.position[2] == 3.0f);
        CHECK(item.light.intensity == 4.0f);
        CHECK(item.light.range == 9.0f);
    }
}

void test_material_rides_on_drawables()
{
    World world;

    // Drawable WITH a material.
    const Entity a = world.create();
    world.add<Transform>(a, Transform{});
    world.add<Renderable>(a, Renderable{{1, 1, 1, 1}, 3u});
    PbrMaterial m;
    m.base_color[0] = 0.25f;
    m.metallic = 1.0f;
    m.roughness = 0.1f;
    m.lightmap_tex = 5u;        // R-REND-006 hook fields round-trip through the extract
    m.lightmap_uv_channel = 1u;
    world.add<PbrMaterial>(a, m);

    // Drawable WITHOUT a material gets the default.
    const Entity b = world.create();
    world.add<Transform>(b, Transform{});
    world.add<Renderable>(b, Renderable{{1, 1, 1, 1}, 4u});

    // A material on a NON-drawable entity produces no item.
    const Entity c = world.create();
    world.add<PbrMaterial>(c, PbrMaterial{});

    RenderSnapshot snap;
    extract_render_world(world, 1u, snap);

    CHECK(snap.items.size() == 2u);
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
        CHECK(item_a->material.base_color[0] == 0.25f);
        CHECK(item_a->material.metallic == 1.0f);
        CHECK(item_a->material.roughness == 0.1f);
        CHECK(item_a->material.lightmap_tex == 5u);
        CHECK(item_a->material.lightmap_uv_channel == 1u);
    }
    if (item_b != nullptr)
    {
        const PbrMaterial defaults;
        CHECK(item_b->material.base_color[0] == defaults.base_color[0]);
        CHECK(item_b->material.metallic == defaults.metallic);
        CHECK(item_b->material.roughness == defaults.roughness);
        CHECK(item_b->material.lightmap_tex == 0u);
    }
}

void test_double_buffer_clears_lights()
{
    World world;
    const Entity sun = world.create();
    world.add<DirectionalLight>(sun, DirectionalLight{});
    const Entity lamp = world.create();
    world.add<Transform>(lamp, Transform{});
    world.add<PointLight>(lamp, PointLight{});

    RenderDoubleBuffer db;
    extract_render_world(world, 0u, db.back());
    CHECK(db.back().directional_lights.size() == 1u);
    CHECK(db.back().point_lights.size() == 1u);
    db.swap();
    CHECK(db.front().directional_lights.size() == 1u);
    CHECK(db.front().point_lights.size() == 1u);

    // The new back buffer is fully cleared — lights included, not just items.
    CHECK(db.back().directional_lights.empty());
    CHECK(db.back().point_lights.empty());
}

} // namespace

int main()
{
    test_directional_light_extracted_and_normalized();
    test_zero_direction_light_is_skipped();
    test_point_light_requires_transform();
    test_material_rides_on_drawables();
    test_double_buffer_clears_lights();
    RENDER_TEST_MAIN_END();
}
