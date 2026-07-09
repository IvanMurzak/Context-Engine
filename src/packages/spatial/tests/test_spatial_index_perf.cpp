// Perf-STRUCTURE sanity for the R-SIM-007 broad-phase index. This is deterministic (no wall-clock
// timing — CI-flaky), asserting the STRUCTURAL properties that make queries O(result + log N):
//   1. the tree stays near-balanced (height ~log2(N), never ~N);
//   2. a small fixed-region query visits far fewer nodes than the whole tree, and that visited-node
//      count grows SUB-linearly as N grows 8x (the O(log N + result) query cost, not O(N));
//   3. the sub-linear cost survives a bulk incremental update (entities moved, not rebuilt).
// Counting nodes visited (SpatialIndex::last_query_visited_nodes) is a deterministic proxy for query
// cost, so the assertions are stable across machines and toolchains.

#include "context/kernel/entity.h"
#include "context/packages/spatial/spatial_index.h"

#include "spatial_test.h"

#include <cstdint>
#include <vector>

using context::kernel::Entity;
using context::packages::spatial::Aabb;
using context::packages::spatial::SpatialIndex;
using context::packages::spatial::Vec3;

namespace
{

// Ceiling of log2(n) for n >= 1.
int ceil_log2(std::size_t n)
{
    int bits = 0;
    std::size_t v = 1;
    while (v < n)
    {
        v <<= 1;
        ++bits;
    }
    return bits;
}

// Fill `idx` with a side x side x side grid of unit-ish cubes at integer coordinates. Entity ids are
// 1..side^3.
void build_grid(SpatialIndex& idx, std::uint32_t side)
{
    std::uint32_t id = 1;
    for (std::uint32_t z = 0; z < side; ++z)
    {
        for (std::uint32_t y = 0; y < side; ++y)
        {
            for (std::uint32_t x = 0; x < side; ++x)
            {
                idx.insert(Entity{id, 1},
                           Aabb::from_center_half_extents(
                               Vec3{static_cast<float>(x), static_cast<float>(y),
                                    static_cast<float>(z)},
                               Vec3{0.3f, 0.3f, 0.3f}));
                ++id;
            }
        }
    }
}

// A small fixed query window near the origin corner (covers a 2x2x2 block of cells).
Aabb corner_window()
{
    return Aabb::from_center_half_extents(Vec3{0.5f, 0.5f, 0.5f}, Vec3{0.7f, 0.7f, 0.7f});
}

std::size_t query_cost(SpatialIndex& idx)
{
    std::vector<Entity> out;
    idx.query_aabb(corner_window(), out);
    return idx.last_query_visited_nodes();
}

} // namespace

int main()
{
    const std::uint32_t side1 = 10; // N1 = 1000
    const std::uint32_t side2 = 20; // N2 = 8000  (8x N1)

    SpatialIndex small;
    SpatialIndex large;
    build_grid(small, side1);
    build_grid(large, side2);

    const std::size_t n1 = side1 * side1 * side1;
    const std::size_t n2 = side2 * side2 * side2;
    CHECK(small.size() == n1);
    CHECK(large.size() == n2);
    CHECK(small.validate());
    CHECK(large.validate());

    // (1) Height is logarithmic, not linear. A near-balanced tree of N leaves has height ~log2(N);
    // the generous bound below still excludes any linear (~N) degeneration by orders of magnitude.
    CHECK(small.height() <= 3 * ceil_log2(n1) + 8);
    CHECK(large.height() <= 3 * ceil_log2(n2) + 8);
    // Growing N 8x grows height by only a few levels (log-scaling), never ~8x.
    CHECK(large.height() < small.height() + 12);

    // (2) The same small query visits far fewer nodes than the whole tree, and stays sub-linear as N
    // grows 8x. Linear scaling would multiply the visited count by ~8; log scaling barely moves it.
    const std::size_t cost1 = query_cost(small);
    const std::size_t cost2 = query_cost(large);
    CHECK(cost1 < n1);       // did not scan everything
    CHECK(cost2 < n2 / 4);   // visits a small fraction of an 8000-entity tree
    CHECK(cost2 <= 3 * cost1); // sub-linear: 8x more entities, well under 8x the query cost

    // The query still returns the correct corner block in both (2x2x2 = 8 cells: x,y,z in {0,1}).
    CHECK(large.query_aabb(corner_window()).size() == 8);

    // (3) Bulk INCREMENTAL update (shift every entity by +100 on x) — kept current, not rebuilt.
    // Afterwards the old window is empty, a shifted window finds the block, cost is still sub-linear.
    {
        std::uint32_t id = 1;
        for (std::uint32_t z = 0; z < side2; ++z)
        {
            for (std::uint32_t y = 0; y < side2; ++y)
            {
                for (std::uint32_t x = 0; x < side2; ++x)
                {
                    large.update(Entity{id, 1},
                                 Aabb::from_center_half_extents(
                                     Vec3{static_cast<float>(x) + 100.0f, static_cast<float>(y),
                                          static_cast<float>(z)},
                                     Vec3{0.3f, 0.3f, 0.3f}));
                    ++id;
                }
            }
        }
        CHECK(large.size() == n2); // no entities lost or duplicated by the moves
        CHECK(large.validate());
        CHECK(large.query_aabb(corner_window()).empty()); // moved away from the origin

        const Aabb shifted =
            Aabb::from_center_half_extents(Vec3{100.5f, 0.5f, 0.5f}, Vec3{0.7f, 0.7f, 0.7f});
        std::vector<Entity> out;
        large.query_aabb(shifted, out);
        CHECK(out.size() == 8);
        CHECK(large.last_query_visited_nodes() < n2 / 4); // still sub-linear after the bulk update
    }

    SPATIAL_TEST_MAIN_END();
}
