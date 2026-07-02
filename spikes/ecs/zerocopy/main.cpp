// spikes/ecs — RUNNABLE zero-copy proof (deliverable 4).
//
// Demonstrates, for the two archetype/SoA options (flecs and the custom sketch), that
// component storage is exposable as a raw contiguous span + layout metadata — exactly the
// shape a JS typed-array view would bind to across the JS/WASM seam (R-LANG-008):
//
//   1. obtain the raw column pointer/count/elem-size for a component column,
//   2. MUTATE the storage through the aliased span (no ECS API involved),
//   3. read the mutation back through the normal ECS API (proves aliasing, not a copy),
//   4. show that a structural change (component add => archetype/table migration) moves the
//      entity OUT of the aliased memory — the R-LANG-009 motivation for end-of-system view
//      invalidation + deferred structural commands,
//   5. (custom) show per-column change versions advancing on writes — the cheap dirty
//      tracking the L-39 render-world extract consumes.
//
// Exit code 0 = all assertions held.

#include "common/workload.h"
#include "custom/mini_ecs.h"

#include <flecs.h>

#include <cstdint>
#include <cstdio>
#include <span>

namespace {

using namespace spike;

constexpr int kDemoEntities = 8;

int demoFlecs()
{
    std::printf("[zerocopy] --- flecs (table columns) ---\n");
    flecs::world world;
    world.component<Position>();
    world.component<Velocity>();

    flecs::entity probe{};
    for (int i = 0; i < kDemoEntities; ++i)
    {
        flecs::entity e = world.entity();
        e.insert([i](Position& p, Velocity& v) {
            p = Position{1000.0f + static_cast<float>(i), 0.5f};
            v = Velocity{static_cast<float>(i), -static_cast<float>(i)};
        });
        if (i == 3) probe = e;
    }

    // Raw column access through the C API — one contiguous SoA column per component.
    const ecs_table_t* table = ecs_get_table(world, probe);
    const ecs_id_t posId = world.component<Position>().id();
    const ecs_id_t velId = world.component<Velocity>().id();
    const std::int32_t posCol = ecs_table_get_column_index(world, table, posId);
    const std::int32_t velCol = ecs_table_get_column_index(world, table, velId);
    const std::int32_t count = ecs_table_count(table);
    auto* posData = static_cast<float*>(
        ecs_table_get_column(const_cast<ecs_table_t*>(table), posCol, 0));
    auto* velData = static_cast<float*>(
        ecs_table_get_column(const_cast<ecs_table_t*>(table), velCol, 0));
    if (posData == nullptr || velData == nullptr || count != kDemoEntities)
    {
        std::printf("[zerocopy] FAIL: could not obtain flecs table columns\n");
        return 1;
    }

    // Layout metadata — what a JS Float32Array view would bind to.
    std::printf("[zerocopy] Position column: base=%p elemSize=%zu stride=%zu count=%d "
                "(JS: new Float32Array(memory, base, count*2))\n",
                static_cast<void*>(posData), sizeof(Position), sizeof(Position), count);
    std::printf("[zerocopy] Velocity column: base=%p (same row order — lockstep per-archetype "
                "view-set, the R-LANG-012 batched-handout shape)\n",
                static_cast<void*>(velData));

    // Find the probe entity's row by its unique marker value, then mutate via the raw span.
    std::span<float> posView(posData, static_cast<std::size_t>(count) * 2);
    std::int32_t row = -1;
    for (std::int32_t r = 0; r < count; ++r)
        if (posView[static_cast<std::size_t>(r) * 2] == 1003.0f) row = r;
    if (row < 0)
    {
        std::printf("[zerocopy] FAIL: probe row not found in raw column\n");
        return 1;
    }
    posView[static_cast<std::size_t>(row) * 2] = 777.0f; // write through the aliased span

    const Position& back = probe.get<Position>(); // read back through the ECS API
    if (back.x != 777.0f)
    {
        std::printf("[zerocopy] FAIL: mutation through raw span not visible via ECS API\n");
        return 1;
    }
    std::printf("[zerocopy] PASS: raw-span write (row %d) visible through flecs get<Position>() "
                "=> the span ALIASES component storage (zero-copy)\n", row);

    // Structural change invalidates the view: adding a component moves the entity to a
    // different table — the aliased memory no longer holds this entity's data.
    struct Tag { int v; };
    world.component<Tag>();
    probe.set<Tag>({1});
    const ecs_table_t* after = ecs_get_table(world, probe);
    if (after == table)
    {
        std::printf("[zerocopy] FAIL: expected table migration on component add\n");
        return 1;
    }
    std::printf("[zerocopy] PASS: component add migrated entity to a different table — "
                "outstanding views are STALE after structural changes; hence R-LANG-009 "
                "end-of-system detach + deferred command buffers\n");
    return 0;
}

int demoCustom()
{
    std::printf("[zerocopy] --- custom sketch (archetype columns, runtime-registered) ---\n");
    mini::World world;
    const mini::TypeId tPos = world.registerType(sizeof(Position), "Position");
    const mini::TypeId tVel = world.registerType(sizeof(Velocity), "Velocity");
    mini::World::Archetype* arch = world.archetypeFor({tPos, tVel});

    mini::Handle probe{};
    for (int i = 0; i < kDemoEntities; ++i)
    {
        const Position p{1000.0f + static_cast<float>(i), 0.5f};
        const Velocity v{static_cast<float>(i), -static_cast<float>(i)};
        const void* data[] = {&p, &v};
        const mini::Handle h = world.createIn(*arch, data);
        if (i == 3) probe = h;
    }

    const mini::ColumnView cv = world.columnView(*arch, tPos);
    std::printf("[zerocopy] Position column: base=%p elemSize=%u stride=%u count=%u version=%llu\n",
                cv.data, cv.elemSize, cv.elemSize, cv.count,
                static_cast<unsigned long long>(cv.version));

    std::span<float> posView(static_cast<float*>(cv.data),
                             static_cast<std::size_t>(cv.count) * 2);
    std::int32_t row = -1;
    for (std::uint32_t r = 0; r < cv.count; ++r)
        if (posView[static_cast<std::size_t>(r) * 2] == 1003.0f) row = static_cast<std::int32_t>(r);
    if (row < 0)
    {
        std::printf("[zerocopy] FAIL: probe row not found in raw column\n");
        return 1;
    }
    posView[static_cast<std::size_t>(row) * 2] = 777.0f;

    const auto* back = static_cast<const Position*>(world.get(probe, tPos));
    if (back == nullptr || back->x != 777.0f)
    {
        std::printf("[zerocopy] FAIL: mutation through raw span not visible via get()\n");
        return 1;
    }
    std::printf("[zerocopy] PASS: raw-span write (row %d) visible through mini get() "
                "=> zero-copy on a RUNTIME-REGISTERED (schema-driven, R-LANG-010) layout\n", row);

    // Dirty tracking (L-39): a declared-write iteration bumps the column version once.
    const std::uint64_t v0 = world.columnView(*arch, tPos).version;
    const mini::TypeId req[] = {tPos};
    world.forEach(req, req,
                  [](mini::World::Archetype&, std::uint32_t n, void* const* cols,
                     const std::uint32_t*) {
                      auto* p = static_cast<Position*>(cols[0]);
                      for (std::uint32_t i = 0; i < n; ++i) p[i].y += 1.0f;
                  });
    const std::uint64_t v1 = world.columnView(*arch, tPos).version;
    if (v1 <= v0)
    {
        std::printf("[zerocopy] FAIL: column version did not advance on declared write\n");
        return 1;
    }
    std::printf("[zerocopy] PASS: column change-version advanced %llu -> %llu on a declared "
                "write pass (cheap extract-to-render-world dirty tracking, L-39)\n",
                static_cast<unsigned long long>(v0), static_cast<unsigned long long>(v1));
    return 0;
}

} // namespace

int main()
{
    const int a = demoFlecs();
    const int b = demoCustom();
    if (a == 0 && b == 0)
    {
        std::printf("[zerocopy] ALL PASS\n");
        return 0;
    }
    return 1;
}
