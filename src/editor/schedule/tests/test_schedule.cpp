// R-QA-013 tests for the safe parallel scheduler's ENGINE-INDEPENDENT core (schedule.h): DAG
// construction from declared access, TS-excluded-from-the-DAG, cache-invalidation-only-on-composition-
// change, the small-system batching policy, and REAL fork-join parallel execution over a World.
//
// These need no JS host, so they are a LOCAL gate on every leg — AND, crucially, the full subject of
// the CI ASan/UBSan + TSan legs: the parallel-mutation test spins real std::threads that mutate a
// shared World over disjoint declared writes, which is exactly the parallel-safety property R-SIM-006
// promises. A data race here is a REAL bug (the sanitizers catch it), not a benign V8 artifact — this
// test links no V8, so it needs no suppressions. The C++-and-TS-both-mutate-World keystone (needs the
// V8 host) is in test_schedule_in_v8.cpp.

#include "context/editor/component/component_registry.h"
#include "context/editor/schedule/schedule.h"
#include "context/kernel/entity.h"
#include "context/kernel/world.h"

#include "schedule_test.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace csch = context::editor::schedule;
namespace ccomp = context::editor::component;
namespace ck = context::kernel;

namespace
{

// --- helpers --------------------------------------------------------------------------------------

// A one-u32-field declarative component `$id`.
ccomp::ComponentTypeSchema u32_comp(const std::string& id)
{
    const std::string def = "{ \"$id\": \"" + id
        + "\", \"version\": 1, \"fields\": [{\"name\": \"n\", \"x-ctx-storage\": \"u32\"}] }";
    std::vector<std::string> problems;
    std::optional<ccomp::ComponentTypeSchema> t = ccomp::compile_component_type(def, problems);
    CHECK(t.has_value());
    CHECK(problems.empty());
    return t.value_or(ccomp::ComponentTypeSchema{});
}

// A native system that adds `delta` to the `n` field of component `cid` on every entity in `ents`.
// Reads only the record bytes of its ONE declared-write component, so two such systems over DISJOINT
// cids touch disjoint memory and are safe to run concurrently.
csch::SystemFn make_incr(ck::World& world, std::vector<ck::Entity> ents, ck::ComponentId cid,
                         std::size_t off, std::uint32_t delta)
{
    return [&world, ents = std::move(ents), cid, off, delta]
    {
        for (const ck::Entity e : ents)
        {
            auto* r = static_cast<unsigned char*>(world.get_raw(e, cid));
            if (r != nullptr)
            {
                std::uint32_t v = 0;
                std::memcpy(&v, r + off, sizeof(v));
                v += delta;
                std::memcpy(r + off, &v, sizeof(v));
            }
        }
    };
}

std::uint32_t read_n(const ck::World& w, ck::Entity e, ck::ComponentId cid, std::size_t off)
{
    const auto* r = static_cast<const unsigned char*>(w.get_raw(e, cid));
    std::uint32_t v = 0;
    if (r != nullptr)
    {
        std::memcpy(&v, r + off, sizeof(v));
    }
    return v;
}

// Every pair of systems in one batch must be non-conflicting (the parallel-safety invariant).
[[nodiscard]] bool batch_is_conflict_free(const csch::SystemRegistry& reg,
                                          const std::vector<csch::SystemId>& batch)
{
    for (std::size_t i = 0; i < batch.size(); ++i)
    {
        for (std::size_t j = i + 1; j < batch.size(); ++j)
        {
            if (csch::conflicts(reg.at(batch[i]).access, reg.at(batch[j]).access))
            {
                return false;
            }
        }
    }
    return true;
}

csch::SystemDesc native(std::string name, csch::AccessSet access,
                        csch::CostHint cost = csch::CostHint::Normal, csch::SystemFn run = {})
{
    csch::SystemDesc d;
    d.name = std::move(name);
    d.lane = csch::Lane::Native;
    d.access = std::move(access);
    d.cost = cost;
    d.run = std::move(run);
    return d;
}

// --- DAG structure ---------------------------------------------------------------------------------

void test_dag_levels()
{
    // Components A=1, B=2, C=3 (raw ids — build_schedule cares only about the AccessSets).
    csch::SystemRegistry reg;
    const csch::SystemId s0 = reg.add(native("writeA", csch::AccessSet::make({}, {1})));
    const csch::SystemId s1 = reg.add(native("writeB", csch::AccessSet::make({}, {2})));
    const csch::SystemId s2 = reg.add(native("readA", csch::AccessSet::make({1}, {})));
    const csch::SystemId s3 = reg.add(native("writeA2", csch::AccessSet::make({}, {1})));
    const csch::SystemId s4 = reg.add(native("writeC", csch::AccessSet::make({}, {3})));

    const csch::Schedule sched = csch::build_schedule(reg);
    const auto& b = sched.native_batches();
    CHECK(b.size() == 3);
    // batch 0: writeA, writeB, writeC (mutually disjoint); readA conflicts writeA → batch 1;
    // writeA2 conflicts writeA (batch0) AND readA (batch1) → batch 2.
    CHECK(b[0].size() == 3);
    CHECK((b[0] == std::vector<csch::SystemId>{s0, s1, s4})); // ascending registration order
    CHECK((b[1] == std::vector<csch::SystemId>{s2}));
    CHECK((b[2] == std::vector<csch::SystemId>{s3}));

    // Every batch is internally conflict-free; conflicting systems preserve registration order.
    for (const auto& batch : b)
    {
        CHECK(batch_is_conflict_free(reg, batch));
    }
    CHECK(sched.ts_lane().empty());
}

void test_ts_excluded_from_dag()
{
    csch::SystemRegistry reg;
    const csch::SystemId n0 = reg.add(native("nativeWriteA", csch::AccessSet::make({}, {1})));
    csch::SystemDesc tsd;
    tsd.name = "tsSystem";
    tsd.lane = csch::Lane::Ts;
    tsd.access = csch::AccessSet::make({}, {1}); // access is IGNORED for a TS system
    const csch::SystemId t0 = reg.add(std::move(tsd));
    const csch::SystemId n1 = reg.add(native("nativeWriteA2", csch::AccessSet::make({}, {1})));
    csch::SystemDesc tsd2;
    tsd2.name = "tsSystem2";
    tsd2.lane = csch::Lane::Ts;
    const csch::SystemId t1 = reg.add(std::move(tsd2));

    const csch::Schedule sched = csch::build_schedule(reg);

    // TS systems appear ONLY on the single lane, in registration order.
    CHECK((sched.ts_lane() == std::vector<csch::SystemId>{t0, t1}));

    // The two native writeA systems conflict with each OTHER but NOT with the excluded TS systems, so
    // they occupy two batches (n0 then n1) — the TS systems never affect native DAG placement.
    const auto& b = sched.native_batches();
    CHECK(b.size() == 2);
    CHECK((b[0] == std::vector<csch::SystemId>{n0}));
    CHECK((b[1] == std::vector<csch::SystemId>{n1}));
    for (const auto& batch : b)
    {
        for (const csch::SystemId id : batch)
        {
            CHECK(id != t0 && id != t1); // no TS id ever lands in a native batch
        }
    }
}

// --- cache: built once, invalidated ONLY on a composition change ----------------------------------

void test_cache_invalidation()
{
    csch::SystemRegistry reg;
    reg.add(native("a", csch::AccessSet::make({}, {1})));
    reg.add(native("b", csch::AccessSet::make({}, {2})));

    csch::ParallelScheduler sched({/*worker_count*/ 2, /*small_batch_target*/ 0});
    CHECK(sched.rebuild_count() == 0);

    // First schedule()/tick builds once.
    (void)sched.schedule(reg);
    CHECK(sched.rebuild_count() == 1);

    // Many ticks with NO composition change → NEVER rebuilds (the "computed once, not per frame" law).
    for (int i = 0; i < 100; ++i)
    {
        sched.run_tick(reg);
    }
    CHECK(sched.rebuild_count() == 1);

    // A composition change (add) invalidates the cache → exactly one rebuild on next use.
    const csch::SystemId added = reg.add(native("c", csch::AccessSet::make({}, {3})));
    sched.run_tick(reg);
    CHECK(sched.rebuild_count() == 2);
    for (int i = 0; i < 10; ++i)
    {
        sched.run_tick(reg);
    }
    CHECK(sched.rebuild_count() == 2);

    // A composition change (remove) also invalidates.
    CHECK(reg.remove(added));
    (void)sched.schedule(reg);
    CHECK(sched.rebuild_count() == 3);
    // A no-op remove is NOT a composition change — no generation bump, no rebuild.
    CHECK(!reg.remove(added));
    (void)sched.schedule(reg);
    CHECK(sched.rebuild_count() == 3);
}

// --- small-system batching policy -----------------------------------------------------------------

void test_batching_policy()
{
    // 100 mutually-non-conflicting Small native systems (each writes a DISTINCT component id) → all in
    // batch 0. The batching policy must coalesce them into at most `worker_count` chunks so scheduling
    // overhead cannot dominate, with every system in exactly one chunk, order preserved.
    csch::SystemRegistry reg;
    std::vector<csch::SystemId> batch;
    for (ck::ComponentId k = 0; k < 100; ++k)
    {
        batch.push_back(
            reg.add(native("tiny" + std::to_string(k),
                           csch::AccessSet::make({}, {static_cast<ck::ComponentId>(1000 + k)}),
                           csch::CostHint::Small)));
    }

    const csch::SchedulePolicy policy{/*worker_count*/ 4, /*small_batch_target*/ 8};
    const auto chunks = csch::plan_batch_chunks(batch, reg, policy);
    CHECK(chunks.size() <= 4); // never more chunks than workers
    CHECK(!chunks.empty());

    // Every system appears exactly once; each chunk is ascending in id.
    std::vector<bool> seen(reg.size(), false);
    std::size_t total = 0;
    for (const auto& chunk : chunks)
    {
        for (std::size_t i = 0; i < chunk.size(); ++i)
        {
            CHECK(!seen[chunk[i]]);
            seen[chunk[i]] = true;
            ++total;
            if (i > 0)
            {
                CHECK(chunk[i - 1] < chunk[i]);
            }
        }
    }
    CHECK(total == 100);

    // Normal-cost systems are NEVER coalesced with a neighbour: 3 Normal systems → 3 fine chunks (≤ cap
    // 4, so returned as-is, one system each).
    csch::SystemRegistry reg2;
    std::vector<csch::SystemId> b2;
    for (ck::ComponentId k = 0; k < 3; ++k)
    {
        b2.push_back(reg2.add(native("norm" + std::to_string(k),
                                     csch::AccessSet::make({}, {static_cast<ck::ComponentId>(k)}),
                                     csch::CostHint::Normal)));
    }
    const auto chunks2 = csch::plan_batch_chunks(b2, reg2, policy);
    CHECK(chunks2.size() == 3);
    for (const auto& c : chunks2)
    {
        CHECK(c.size() == 1);
    }
}

// --- real fork-join parallel execution over a World (the TSan/ASan/UBSan subject) -----------------

void test_parallel_mutation_over_world()
{
    // Four declared components, four native systems each writing a DISTINCT one (disjoint declared
    // writes) → all land in batch 0 and run CONCURRENTLY. Each system increments the `n` field of every
    // entity's record of its component. worker_count=4 forces a real multi-thread fan-out.
    ck::World world;
    ccomp::ComponentTypeRegistry creg;
    struct Comp
    {
        ck::ComponentId id;
        std::size_t off;
    };
    std::vector<Comp> comps;
    std::vector<const ccomp::RegisteredComponentType*> types;
    for (int c = 0; c < 4; ++c)
    {
        const ccomp::RegisteredComponentType& t =
            creg.register_type(u32_comp("demo:cnt" + std::to_string(c)));
        const ccomp::ComponentField* f = t.schema.field("n");
        CHECK(f != nullptr);
        comps.push_back(Comp{t.component_id, f != nullptr ? f->offset : 0});
        types.push_back(&t);
    }

    // 200 entities, each carrying all four components (one shared archetype).
    std::vector<ck::Entity> ents;
    for (int i = 0; i < 200; ++i)
    {
        const ck::Entity e = world.create();
        for (const auto* t : types)
        {
            (void)creg.add_default(world, e, *t);
        }
        ents.push_back(e);
    }

    csch::SystemRegistry reg;
    for (const Comp& c : comps)
    {
        reg.add(native("incr", csch::AccessSet::make({}, {c.id}), csch::CostHint::Normal,
                       make_incr(world, ents, c.id, c.off, 1)));
    }

    csch::ParallelScheduler sched({/*worker_count*/ 4, 0});

    // Run 50 ticks. Every tick each component is incremented exactly once for every entity; a lost
    // update from a data race would show up as a value < 50 (and TSan would flag the race outright).
    constexpr std::uint32_t kTicks = 50;
    for (std::uint32_t t = 0; t < kTicks; ++t)
    {
        sched.run_tick(reg);
    }
    CHECK(sched.rebuild_count() == 1); // DAG built once across all 50 ticks

    for (const ck::Entity e : ents)
    {
        for (const Comp& c : comps)
        {
            CHECK(read_n(world, e, c.id, c.off) == kTicks);
        }
    }
}

void test_parallel_matches_sequential_reference()
{
    // Determinism (L-54): the parallel schedule produces the SAME World state as a single-threaded
    // registration-order run. Build two identical compositions over two fresh Worlds — one driven by
    // the scheduler (worker_count=4), one run strictly sequentially — and compare byte-for-byte.
    auto build_world = [](ck::World& world, ccomp::ComponentTypeRegistry& creg,
                          std::vector<ck::ComponentId>& ids, std::vector<std::size_t>& offs,
                          std::vector<ck::Entity>& ents)
    {
        std::vector<const ccomp::RegisteredComponentType*> types;
        for (int c = 0; c < 3; ++c)
        {
            const ccomp::RegisteredComponentType& t =
                creg.register_type(u32_comp("demo:seq" + std::to_string(c)));
            ids.push_back(t.component_id);
            const ccomp::ComponentField* f = t.schema.field("n");
            offs.push_back(f != nullptr ? f->offset : 0);
            types.push_back(&t);
        }
        for (int i = 0; i < 64; ++i)
        {
            const ck::Entity e = world.create();
            for (const auto* t : types)
            {
                (void)creg.add_default(world, e, *t);
            }
            ents.push_back(e);
        }
    };

    // Parallel run.
    ck::World wp;
    ccomp::ComponentTypeRegistry cp;
    std::vector<ck::ComponentId> idp;
    std::vector<std::size_t> offp;
    std::vector<ck::Entity> ep;
    build_world(wp, cp, idp, offp, ep);
    csch::SystemRegistry regp;
    for (std::size_t c = 0; c < idp.size(); ++c)
    {
        regp.add(native("incr", csch::AccessSet::make({}, {idp[c]}), csch::CostHint::Normal,
                        make_incr(wp, ep, idp[c], offp[c], static_cast<std::uint32_t>(c + 1))));
    }
    csch::ParallelScheduler sched({4, 0});
    for (int t = 0; t < 10; ++t)
    {
        sched.run_tick(regp);
    }

    // Sequential reference: same seeds, same per-system delta, run in registration order, no scheduler.
    ck::World ws;
    ccomp::ComponentTypeRegistry cs;
    std::vector<ck::ComponentId> ids;
    std::vector<std::size_t> offs;
    std::vector<ck::Entity> es;
    build_world(ws, cs, ids, offs, es);
    std::vector<csch::SystemFn> seq;
    for (std::size_t c = 0; c < ids.size(); ++c)
    {
        seq.push_back(make_incr(ws, es, ids[c], offs[c], static_cast<std::uint32_t>(c + 1)));
    }
    for (int t = 0; t < 10; ++t)
    {
        for (auto& fn : seq)
        {
            fn();
        }
    }

    // The two Worlds match component-for-component, entity-for-entity.
    CHECK(ep.size() == es.size());
    for (std::size_t i = 0; i < ep.size(); ++i)
    {
        for (std::size_t c = 0; c < idp.size(); ++c)
        {
            CHECK(read_n(wp, ep[i], idp[c], offp[c]) == read_n(ws, es[i], ids[c], offs[c]));
        }
    }
}

void test_conflict_serialization()
{
    // A writer (writes X) registered BEFORE a reader (reads X, copies it to Y) → they conflict → the
    // reader lands in a strictly later batch → the writer's value is visible to the reader within the
    // same tick, deterministically.
    ck::World world;
    ccomp::ComponentTypeRegistry creg;
    const ccomp::RegisteredComponentType& cx = creg.register_type(u32_comp("demo:x"));
    const ccomp::RegisteredComponentType& cy = creg.register_type(u32_comp("demo:y"));
    const std::size_t xoff = cx.schema.field("n")->offset;
    const std::size_t yoff = cy.schema.field("n")->offset;

    const ck::Entity e = world.create();
    (void)creg.add_default(world, e, cx);
    (void)creg.add_default(world, e, cy);

    csch::SystemRegistry reg;
    // writer: X = 42.
    reg.add(native("writeX", csch::AccessSet::make({}, {cx.component_id}), csch::CostHint::Normal,
                   [&world, e, id = cx.component_id, xoff]
                   {
                       auto* r = static_cast<unsigned char*>(world.get_raw(e, id));
                       std::uint32_t v = 42;
                       std::memcpy(r + xoff, &v, sizeof(v));
                   }));
    // reader: Y = X (reads X, writes Y).
    reg.add(native("copyXtoY",
                   csch::AccessSet::make({cx.component_id}, {cy.component_id}), csch::CostHint::Normal,
                   [&world, e, xid = cx.component_id, yid = cy.component_id, xoff, yoff]
                   {
                       const auto* rx = static_cast<const unsigned char*>(world.get_raw(e, xid));
                       auto* ry = static_cast<unsigned char*>(world.get_raw(e, yid));
                       std::uint32_t v = 0;
                       std::memcpy(&v, rx + xoff, sizeof(v));
                       std::memcpy(ry + yoff, &v, sizeof(v));
                   }));

    const csch::Schedule& s = csch::build_schedule(reg);
    CHECK(s.native_batches().size() == 2); // serialized into two batches

    csch::ParallelScheduler sched({4, 0});
    sched.run_tick(reg);
    CHECK(read_n(world, e, cx.component_id, xoff) == 42);
    CHECK(read_n(world, e, cy.component_id, yoff) == 42); // reader saw the writer's value this tick
}

void test_cpp_system_mutates_world()
{
    // The plain DoD statement: a single C++ (native) system mutates a declared World component,
    // observable in the derived World after one tick.
    ck::World world;
    ccomp::ComponentTypeRegistry creg;
    const ccomp::RegisteredComponentType& c = creg.register_type(u32_comp("demo:hp"));
    const std::size_t off = c.schema.field("n")->offset;
    const ck::Entity e = world.create();
    auto* r = static_cast<unsigned char*>(creg.add_default(world, e, c));
    CHECK(r != nullptr);
    std::uint32_t seed = 5;
    std::memcpy(r + off, &seed, sizeof(seed));

    csch::SystemRegistry reg;
    reg.add(native("bumpHp", csch::AccessSet::make({}, {c.component_id}), csch::CostHint::Normal,
                   make_incr(world, {e}, c.component_id, off, 3)));

    csch::ParallelScheduler sched;
    sched.run_tick(reg);
    CHECK(read_n(world, e, c.component_id, off) == 8);
}

} // namespace

int main()
{
    test_dag_levels();
    test_ts_excluded_from_dag();
    test_cache_invalidation();
    test_batching_policy();
    test_parallel_mutation_over_world();
    test_parallel_matches_sequential_reference();
    test_conflict_serialization();
    test_cpp_system_mutates_world();
    if (scheduletest::g_failures == 0)
    {
        std::printf("schedule DAG/cache/batching/parallel: all checks passed\n");
    }
    SCHEDULE_TEST_MAIN_END();
}
