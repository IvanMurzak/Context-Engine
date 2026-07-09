// Correctness tests for the R-SIM-007 broad-phase spatial index: insert / update / remove keep the
// dynamic AABB tree correct without a full rebuild, and radius + AABB queries return exactly the
// overlapping set (happy path, edge cases, AND failure paths). Query results are treated as a set
// (order is unspecified), so every comparison sorts first.

#include "context/kernel/entity.h"
#include "context/packages/spatial/spatial_index.h"

#include "spatial_test.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using context::kernel::Entity;
using context::packages::spatial::Aabb;
using context::packages::spatial::SpatialIndex;
using context::packages::spatial::Vec3;

namespace
{

Entity ent(std::uint32_t index)
{
    return Entity{index, /*generation=*/1};
}

// A small cube of side `s` centered on (cx, cy, cz).
Aabb cube(float cx, float cy, float cz, float s = 0.5f)
{
    return Aabb::from_center_half_extents(Vec3{cx, cy, cz}, Vec3{s, s, s});
}

std::vector<std::uint32_t> ids(const std::vector<Entity>& es)
{
    std::vector<std::uint32_t> v;
    v.reserve(es.size());
    for (const Entity& e : es)
    {
        v.push_back(e.index);
    }
    std::sort(v.begin(), v.end());
    return v;
}

bool same(std::vector<Entity> got, std::vector<std::uint32_t> want)
{
    std::sort(want.begin(), want.end());
    return ids(got) == want;
}

} // namespace

int main()
{
    // --- empty index: every query is empty, remove/update on absent entities are no-ops -----------
    {
        SpatialIndex idx;
        CHECK(idx.empty());
        CHECK(idx.size() == 0);
        CHECK(idx.height() == -1);
        CHECK(idx.node_count() == 0);
        CHECK(idx.validate());

        CHECK(!idx.remove(ent(1)));           // remove-absent
        CHECK(!idx.update(ent(1), cube(0, 0, 0))); // update-absent
        CHECK(!idx.contains(ent(1)));
        CHECK(idx.query_aabb(cube(0, 0, 0, 100.0f)).empty());
        CHECK(idx.query_radius(Vec3{0, 0, 0}, 100.0f).empty());
        CHECK(idx.last_query_visited_nodes() == 0); // nothing to visit
    }

    // --- invalid entity is rejected --------------------------------------------------------------
    {
        SpatialIndex idx;
        CHECK(!idx.insert(Entity{}, cube(0, 0, 0))); // generation 0 == null entity
        CHECK(idx.empty());
    }

    // --- single insert then hit / miss queries ---------------------------------------------------
    {
        SpatialIndex idx;
        CHECK(idx.insert(ent(7), cube(10, 0, 0)));
        CHECK(idx.size() == 1);
        CHECK(idx.contains(ent(7)));
        CHECK(idx.height() == 0);
        CHECK(idx.node_count() == 1);
        CHECK(idx.validate());

        // AABB hit / miss.
        CHECK(same(idx.query_aabb(cube(10, 0, 0, 1.0f)), {7}));
        CHECK(idx.query_aabb(cube(-10, 0, 0, 1.0f)).empty());
        // Radius hit / miss (entity at x=10, box half-extent 0.5).
        CHECK(same(idx.query_radius(Vec3{10, 0, 0}, 1.0f), {7}));
        CHECK(idx.query_radius(Vec3{0, 0, 0}, 5.0f).empty());  // 5 < ~9.5 gap → miss
        CHECK(same(idx.query_radius(Vec3{0, 0, 0}, 10.0f), {7})); // reaches the box

        // Re-inserting the same entity updates rather than duplicating (returns false).
        CHECK(!idx.insert(ent(7), cube(-10, 0, 0)));
        CHECK(idx.size() == 1);
        CHECK(idx.query_aabb(cube(10, 0, 0, 1.0f)).empty());
        CHECK(same(idx.query_aabb(cube(-10, 0, 0, 1.0f)), {7}));
    }

    // --- many entities: exact AABB + radius membership -------------------------------------------
    {
        SpatialIndex idx;
        // A 5x5 grid of unit cubes at integer x,y in [0,4], z=0.
        for (std::uint32_t y = 0; y < 5; ++y)
        {
            for (std::uint32_t x = 0; x < 5; ++x)
            {
                const std::uint32_t id = y * 5 + x + 1; // 1..25
                idx.insert(ent(id), cube(static_cast<float>(x), static_cast<float>(y), 0.0f, 0.3f));
            }
        }
        CHECK(idx.size() == 25);
        CHECK(idx.node_count() == 49); // 2*25 - 1
        CHECK(idx.validate());

        // A box tightly around the 2x2 block x in [0,1], y in [0,1] → ids {1,2,6,7}.
        const Aabb block = Aabb::from_center_half_extents(Vec3{0.5f, 0.5f, 0.0f}, Vec3{0.7f, 0.7f, 0.5f});
        CHECK(same(idx.query_aabb(block), {1, 2, 6, 7}));

        // A radius around the corner cube (0,0): only id 1 within r=0.5 of its box.
        CHECK(same(idx.query_radius(Vec3{0, 0, 0}, 0.5f), {1}));

        // A big radius covering everything.
        CHECK(idx.query_radius(Vec3{2, 2, 0}, 100.0f).size() == 25);

        // Cost sanity: the tight 2x2 query must visit far fewer nodes than the whole tree.
        std::vector<Entity> sink;
        idx.query_aabb(block, sink);
        CHECK(idx.last_query_visited_nodes() < idx.node_count());
    }

    // --- incremental update: fast (in-fat-box) path and reinsert path both stay correct ----------
    {
        SpatialIndex idx;
        idx.insert(ent(1), cube(0, 0, 0, 0.5f));
        idx.insert(ent(2), cube(50, 0, 0, 0.5f));
        idx.insert(ent(3), cube(0, 50, 0, 0.5f));
        CHECK(idx.validate());

        // Tiny move within the fat margin (0.1): the O(1) fast path — still found near origin.
        CHECK(idx.update(ent(1), cube(0.05f, 0, 0, 0.5f)));
        CHECK(same(idx.query_aabb(cube(0, 0, 0, 1.0f)), {1}));
        CHECK(idx.validate());

        // Large move that leaves the fat box: reinsert path. Entity 1 relocates on top of entity 2.
        CHECK(idx.update(ent(1), cube(50, 0, 0, 0.5f)));
        CHECK(same(idx.query_aabb(cube(50, 0, 0, 1.0f)), {1, 2}));
        CHECK(idx.query_aabb(cube(0, 0, 0, 1.0f)).empty()); // no longer at origin
        CHECK(idx.size() == 3);
        CHECK(idx.validate());
    }

    // --- remove keeps the rest queryable and the structure valid ---------------------------------
    {
        SpatialIndex idx;
        for (std::uint32_t i = 1; i <= 10; ++i)
        {
            idx.insert(ent(i), cube(static_cast<float>(i), 0, 0, 0.3f));
        }
        CHECK(idx.size() == 10);

        CHECK(idx.remove(ent(5)));
        CHECK(!idx.remove(ent(5))); // second remove is a no-op
        CHECK(idx.size() == 9);
        CHECK(!idx.contains(ent(5)));
        CHECK(idx.validate());

        // The removed entity is gone from queries; its neighbors remain.
        CHECK(idx.query_aabb(cube(5, 0, 0, 0.4f)).empty());
        CHECK(same(idx.query_aabb(cube(4, 0, 0, 0.4f)), {4}));
        CHECK(same(idx.query_aabb(cube(6, 0, 0, 0.4f)), {6}));

        // A wide query returns exactly the 9 survivors.
        CHECK(idx.query_aabb(cube(5, 0, 0, 100.0f)).size() == 9);

        // Remove everything → empty, valid, height -1.
        for (std::uint32_t i = 1; i <= 10; ++i)
        {
            idx.remove(ent(i));
        }
        CHECK(idx.empty());
        CHECK(idx.height() == -1);
        CHECK(idx.validate());
    }

    // --- degenerate inputs: inverted box is normalized; point (zero-volume) entity is queryable ---
    {
        SpatialIndex idx;
        // Pass corners in the wrong order — must be normalized, not dropped.
        Aabb inverted;
        inverted.min = Vec3{5, 5, 5};
        inverted.max = Vec3{4, 4, 4};
        CHECK(idx.insert(ent(1), inverted));
        CHECK(same(idx.query_aabb(cube(4.5f, 4.5f, 4.5f, 1.0f)), {1}));
        CHECK(idx.validate());

        // A true point (min == max): still overlaps a box / sphere that touches it.
        idx.insert(ent(2), Aabb::from_point(Vec3{20, 20, 20}));
        CHECK(same(idx.query_aabb(cube(20, 20, 20, 0.01f)), {2}));
        CHECK(same(idx.query_radius(Vec3{20, 20, 20}, 0.0f), {2})); // zero radius touches the point
        CHECK(idx.validate());

        // A negative radius yields nothing (failure path).
        CHECK(idx.query_radius(Vec3{20, 20, 20}, -1.0f).empty());
    }

    // --- clear resets everything -----------------------------------------------------------------
    {
        SpatialIndex idx;
        for (std::uint32_t i = 1; i <= 20; ++i)
        {
            idx.insert(ent(i), cube(static_cast<float>(i), 0, 0));
        }
        idx.clear();
        CHECK(idx.empty());
        CHECK(idx.height() == -1);
        CHECK(idx.node_count() == 0);
        CHECK(idx.validate());
        // Reusable after clear.
        CHECK(idx.insert(ent(1), cube(0, 0, 0)));
        CHECK(same(idx.query_aabb(cube(0, 0, 0, 1.0f)), {1}));
    }

    SPATIAL_TEST_MAIN_END();
}
