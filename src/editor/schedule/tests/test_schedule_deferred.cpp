// R-QA-013 tests for the scheduler's DEFERRED STRUCTURAL CHANGES (#88 item 3 — the R-LANG-009
// command-buffer clause): a running system records add/remove/create/destroy into its per-system
// kernel::CommandBuffer and the World-taking run_tick applies the buffers BETWEEN systems — at native
// batch boundaries in ascending registration order (deterministic regardless of thread completion
// order, L-54) and after each TS system on the single JS-VM lane. Proven here: mid-system structural
// changes are invisible until the system returns (same-batch siblings see the stable pre-batch
// World); a dependent (conflicting, later-batch) system sees them the SAME tick; memory never moves
// under a live component pointer while a batch runs; apply order is deterministic across worker
// counts; a throwing system discards every unapplied buffer; and parallel recording into per-system
// buffers is race-free (this test links no V8, so the CI ASan/UBSan + TSan legs fully instrument it —
// the parallel-safety gate extends to the deferred-structural path).

#include "context/editor/schedule/schedule.h"
#include "context/kernel/command_buffer.h"
#include "context/kernel/entity.h"
#include "context/kernel/world.h"

#include "schedule_test.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace csch = context::editor::schedule;
namespace ck = context::kernel;

namespace
{

struct CompA
{
    std::uint32_t n = 0;
};

struct CompB
{
    std::uint32_t n = 0;
};

struct Tag
{
    std::uint32_t n = 0;
};

// Collect (entity index → Tag value) for every live Tag carrier, ordered by the World's canonical
// iteration — the observable structural outcome a determinism assertion can compare across runs.
std::vector<std::pair<std::uint32_t, std::uint32_t>> tag_snapshot(ck::World& w)
{
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    w.each<Tag>([&out](ck::Entity e, Tag& t) { out.emplace_back(e.index, t.n); });
    return out;
}

// --- a dependent (later-batch) system sees the flush the SAME tick; the issuer never does ----------

void test_dependent_sees_flush_same_tick()
{
    ck::World w;
    const ck::Entity e = w.create();
    w.add(e, CompA{1});

    std::atomic<std::size_t> issuer_saw_alive{0};
    std::atomic<std::uint32_t> dependent_saw_tag{0};
    std::atomic<std::size_t> dependent_saw_alive{0};

    csch::SystemRegistry reg;

    // S0 (writes A) defers: create an entity carrying Tag{7}. Its OWN invocation observes the stable
    // pre-tick World even after recording (deferred-invisibility).
    reg.add({"issuer",
             csch::Lane::Native,
             csch::AccessSet::make({}, {ck::component_id<CompA>()}),
             csch::CostHint::Normal,
             {},
             [&w, &issuer_saw_alive](ck::CommandBuffer& buf)
             {
                 const ck::CommandBuffer::PendingEntity p = buf.create();
                 buf.add(p, Tag{7});
                 issuer_saw_alive = w.alive_count(); // still the pre-tick World
             }});

    // S1 (reads A → conflicts with S0 → strictly later batch, the DEPENDENT). Runs after the batch-0
    // boundary flush, so it observes S0's structural changes within the SAME tick.
    reg.add({"dependent",
             csch::Lane::Native,
             csch::AccessSet::make({ck::component_id<CompA>()}, {}),
             csch::CostHint::Normal,
             [&w, &dependent_saw_tag, &dependent_saw_alive]
             {
                 dependent_saw_alive = w.alive_count();
                 w.each<Tag>([&dependent_saw_tag](ck::Entity, Tag& t)
                             { dependent_saw_tag = t.n; });
             },
             {}});

    csch::ParallelScheduler sched;
    // The DAG must have placed the conflicting pair in two batches (the "dependent" shape).
    CHECK(sched.schedule(reg).native_batches().size() == 2);

    sched.run_tick(reg, w);

    CHECK(issuer_saw_alive == 1);    // stable World during the issuing invocation
    CHECK(dependent_saw_alive == 2); // flushed before the dependent ran
    CHECK(dependent_saw_tag == 7);
    CHECK(w.alive_count() == 2);

    // Deferred ticks reuse the cached DAG like ordinary ticks — no per-tick rebuild.
    sched.run_tick(reg, w);
    CHECK(sched.rebuild_count() == 1);
}

// --- same-batch siblings observe the stable pre-batch World (and memory never moves) --------------

void test_same_batch_invisibility_and_no_move()
{
    ck::World w;
    const ck::Entity e = w.create();
    w.add(e, CompA{11});

    const CompA* live = w.get<CompA>(e); // the live-view stand-in: a raw pointer into A's storage
    CHECK(live != nullptr);

    std::atomic<std::size_t> sibling_saw_alive{0};
    std::atomic<bool> sibling_saw_stable_ptr{false};

    csch::SystemRegistry reg;

    // S0 (writes A): records a structurally heavy deferral — many creates carrying CompA (would grow
    // or migrate A's column if applied mid-batch).
    reg.add({"issuer",
             csch::Lane::Native,
             csch::AccessSet::make({}, {ck::component_id<CompA>()}),
             csch::CostHint::Normal,
             {},
             [](ck::CommandBuffer& buf)
             {
                 for (std::uint32_t i = 0; i < 64; ++i)
                 {
                     const ck::CommandBuffer::PendingEntity p = buf.create();
                     buf.add(p, CompA{i});
                 }
             }});

    // S1 (writes B — disjoint, SAME batch): observes the stable pre-batch World while S0 records, and
    // the live pointer has not moved. Reads only World metadata + the pinned record — no mutation —
    // so running concurrently with S0's recording is race-free (the TSan leg proves it).
    reg.add({"sibling",
             csch::Lane::Native,
             csch::AccessSet::make({}, {ck::component_id<CompB>()}),
             csch::CostHint::Normal,
             [&w, e, live, &sibling_saw_alive, &sibling_saw_stable_ptr]
             {
                 sibling_saw_alive = w.alive_count();
                 sibling_saw_stable_ptr = (w.get<CompA>(e) == live) && (live->n == 11);
             },
             {}});

    csch::ParallelScheduler sched;
    CHECK(sched.schedule(reg).native_batches().size() == 1); // disjoint ⇒ one batch

    sched.run_tick(reg, w);

    CHECK(sibling_saw_alive == 1);     // nothing landed mid-batch
    CHECK(sibling_saw_stable_ptr);     // memory never moved under the live pointer
    CHECK(w.alive_count() == 65);      // …but everything landed at the batch boundary
}

// --- apply order is deterministic: ascending registration order, not thread completion order -------

void test_apply_order_deterministic_across_worker_counts()
{
    // Four creator systems (disjoint — empty access sets never conflict — so ONE batch). Each creates
    // one Tag-carrying entity; system k sleeps so that completion order INVERTS registration order
    // under parallel execution. If apply order followed completion, the parallel run would assign
    // entity indices in a different order than the sequential run.
    const auto run_once = [](unsigned worker_count)
    {
        ck::World w;
        csch::SystemRegistry reg;
        for (std::uint32_t k = 0; k < 4; ++k)
        {
            reg.add({"creator-" + std::to_string(k),
                     csch::Lane::Native,
                     csch::AccessSet{},
                     csch::CostHint::Normal,
                     {},
                     [k](ck::CommandBuffer& buf)
                     {
                         std::this_thread::sleep_for(std::chrono::milliseconds((4 - k) * 10));
                         const ck::CommandBuffer::PendingEntity p = buf.create();
                         buf.add(p, Tag{100 + k});
                     }});
        }
        csch::SchedulePolicy policy;
        policy.worker_count = worker_count;
        csch::ParallelScheduler sched(policy);
        sched.run_tick(reg, w);
        return tag_snapshot(w);
    };

    const auto sequential = run_once(1);
    const auto parallel = run_once(4);

    CHECK(sequential.size() == 4);
    CHECK(sequential == parallel); // identical entity ids AND values — the L-54 determinism proof
}

// --- TS lane: flushed after EACH TS system, and the native phase flushed before the lane -----------

void test_ts_lane_flush_between_systems()
{
    ck::World w;

    std::atomic<std::uint32_t> t0_saw_native_tag{0};
    std::atomic<std::uint32_t> t1_saw_ts_tag{0};

    csch::SystemRegistry reg;

    // Native system defers a Tag{1} entity — must be visible to the TS lane (the last native batch
    // flushes before the TS phase).
    reg.add({"native-creator",
             csch::Lane::Native,
             csch::AccessSet{},
             csch::CostHint::Normal,
             {},
             [](ck::CommandBuffer& buf)
             {
                 const ck::CommandBuffer::PendingEntity p = buf.create();
                 buf.add(p, Tag{1});
             }});

    // TS system 0: sees the native flush; defers Tag{2}.
    reg.add({"ts-0",
             csch::Lane::Ts,
             csch::AccessSet{},
             csch::CostHint::Normal,
             {},
             [&w, &t0_saw_native_tag](ck::CommandBuffer& buf)
             {
                 w.each<Tag>([&t0_saw_native_tag](ck::Entity, Tag& t)
                             { t0_saw_native_tag = std::max(t0_saw_native_tag.load(), t.n); });
                 const ck::CommandBuffer::PendingEntity p = buf.create();
                 buf.add(p, Tag{2});
             }});

    // TS system 1: the single lane is sequential, so ts-0's buffer was applied before ts-1 runs.
    reg.add({"ts-1",
             csch::Lane::Ts,
             csch::AccessSet{},
             csch::CostHint::Normal,
             [&w, &t1_saw_ts_tag]
             {
                 w.each<Tag>([&t1_saw_ts_tag](ck::Entity, Tag& t)
                             { t1_saw_ts_tag = std::max(t1_saw_ts_tag.load(), t.n); });
             },
             {}});

    csch::ParallelScheduler sched;
    sched.run_tick(reg, w);

    CHECK(t0_saw_native_tag == 1); // native deferral flushed before the TS lane
    CHECK(t1_saw_ts_tag == 2);     // ts-0's deferral flushed before ts-1 ran
    CHECK(w.alive_count() == 2);
}

// --- the World-free tick skips run_cmd systems; the World tick services them -----------------------

void test_legacy_tick_skips_run_cmd()
{
    ck::World w;
    csch::SystemRegistry reg;
    reg.add({"cmd-only",
             csch::Lane::Native,
             csch::AccessSet{},
             csch::CostHint::Normal,
             {},
             [](ck::CommandBuffer& buf) { (void)buf.create(); }});

    csch::ParallelScheduler sched;
    sched.run_tick(reg); // no World — the run_cmd system is skipped (nothing to apply into)
    CHECK(w.alive_count() == 0);

    sched.run_tick(reg, w); // the World tick services it
    CHECK(w.alive_count() == 1);
}

// --- a throwing system discards every recorded-but-unapplied buffer --------------------------------

void test_exception_discards_unapplied_buffers()
{
    ck::World w;
    std::atomic<bool> first_tick{true};

    csch::SystemRegistry reg;

    // Same batch (disjoint access): the recorder's buffer is NOT yet applied when the thrower's
    // exception propagates after the batch join — it must be discarded, not leak into the next tick.
    reg.add({"recorder",
             csch::Lane::Native,
             csch::AccessSet::make({}, {ck::component_id<CompA>()}),
             csch::CostHint::Normal,
             {},
             [&first_tick](ck::CommandBuffer& buf)
             {
                 if (first_tick)
                 {
                     const ck::CommandBuffer::PendingEntity p = buf.create();
                     buf.add(p, Tag{9});
                 }
             }});
    reg.add({"thrower",
             csch::Lane::Native,
             csch::AccessSet::make({}, {ck::component_id<CompB>()}),
             csch::CostHint::Normal,
             [&first_tick]
             {
                 if (first_tick)
                 {
                     throw std::runtime_error("system failure");
                 }
             },
             {}});

    csch::ParallelScheduler sched;
    bool threw = false;
    try
    {
        sched.run_tick(reg, w);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
    CHECK(w.alive_count() == 0); // nothing from the failed tick landed

    // The next tick must not apply the failed tick's stale commands.
    first_tick = false;
    sched.run_tick(reg, w);
    CHECK(w.alive_count() == 0);
}

// --- TS-lane throw: earlier TS flushes already landed (per contract); later ones discarded ---------

void test_ts_exception_keeps_prior_flushes()
{
    ck::World w;
    std::atomic<bool> first_tick{true};
    csch::SystemRegistry reg;

    reg.add({"ts-creator",
             csch::Lane::Ts,
             csch::AccessSet{},
             csch::CostHint::Normal,
             {},
             [](ck::CommandBuffer& buf)
             {
                 const ck::CommandBuffer::PendingEntity p = buf.create();
                 buf.add(p, Tag{3});
             }});
    reg.add({"ts-thrower",
             csch::Lane::Ts,
             csch::AccessSet{},
             csch::CostHint::Normal,
             {},
             [&first_tick](ck::CommandBuffer& buf)
             {
                 if (first_tick)
                 {
                     (void)buf.create(); // recorded, then discarded by the propagating exception
                     throw std::runtime_error("ts failure");
                 }
             }});

    csch::ParallelScheduler sched;
    bool threw = false;
    try
    {
        sched.run_tick(reg, w);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
    // ts-creator's buffer was applied BETWEEN the two TS systems (the contract), so its entity
    // exists; the thrower's recorded-but-unapplied create was discarded.
    CHECK(w.alive_count() == 1);

    // No stale commands leak into the next tick: the thrower's discarded create must NOT land now.
    // (ts-creator records again each tick by design, so exactly ONE new entity appears.)
    first_tick = false;
    sched.run_tick(reg, w);
    CHECK(w.alive_count() == 2);
}

// --- parallel recording into per-system buffers is race-free (the TSan/ASan subject) ---------------

void test_parallel_recording()
{
    ck::World w;
    csch::SystemRegistry reg;

    // 16 non-conflicting creator systems in ONE batch, forked across 4 workers: each records into its
    // OWN buffer concurrently. The join + calling-thread apply gives the happens-before edge; the CI
    // TSan leg instruments exactly this path.
    for (std::uint32_t k = 0; k < 16; ++k)
    {
        reg.add({"par-creator-" + std::to_string(k),
                 csch::Lane::Native,
                 csch::AccessSet{},
                 csch::CostHint::Normal,
                 {},
                 [k](ck::CommandBuffer& buf)
                 {
                     for (std::uint32_t i = 0; i < 10; ++i)
                     {
                         const ck::CommandBuffer::PendingEntity p = buf.create();
                         buf.add(p, Tag{k * 100 + i});
                     }
                 }});
    }

    csch::SchedulePolicy policy;
    policy.worker_count = 4;
    csch::ParallelScheduler sched(policy);
    CHECK(sched.schedule(reg).native_batches().size() == 1);

    sched.run_tick(reg, w);
    CHECK(w.alive_count() == 160);

    // Determinism holds under parallel recording too: a second identical run on a fresh world yields
    // the identical snapshot.
    ck::World w2;
    csch::ParallelScheduler sched2(policy);
    sched2.run_tick(reg, w2);
    CHECK(tag_snapshot(w) == tag_snapshot(w2));
}

} // namespace

int main()
{
    test_dependent_sees_flush_same_tick();
    test_same_batch_invisibility_and_no_move();
    test_apply_order_deterministic_across_worker_counts();
    test_ts_lane_flush_between_systems();
    test_legacy_tick_skips_run_cmd();
    test_exception_discards_unapplied_buffers();
    test_ts_exception_keeps_prior_flushes();
    test_parallel_recording();
    SCHEDULE_TEST_MAIN_END();
}
