// R-QA-013 unit test for the L-47 GC-pause profiler channel (gc_channel.h): recording +
// attribution + aggregates (happy), capacity overflow + incremental-marking exclusion (edge),
// and the fail-closed budget verdict on drops/breaches (failure). Engine-free — samples are
// recorded directly, so this is a LOCAL gate on every toolchain; the engine-fed drain path is
// covered by js-test_gc_discipline / js-test_gc_state_hash on the V8-capable CI legs.

#include "context/runtime/profile/gc_channel.h"

#include <cstdio>

namespace
{
int g_failures = 0;

void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            fail(__FILE__, __LINE__, #cond);                                                       \
    } while (false)

namespace profile = context::runtime::profile;

namespace
{
profile::GcPauseSample sample(std::uint64_t tick, double ms, std::uint32_t kind, bool in_window)
{
    profile::GcPauseSample s;
    s.tick = tick;
    s.duration_ms = ms;
    s.kind = kind;
    s.in_window = in_window;
    return s;
}

constexpr std::uint32_t kScavenge = 1u << 0;
constexpr std::uint32_t kMarkSweepCompact = 1u << 2;
constexpr std::uint32_t kIncrementalMarking = 1u << 3;

// --- happy path: recording, attribution, aggregates -----------------------------------------------
void test_record_and_aggregates()
{
    profile::GcPauseChannel ch(16);
    CHECK(ch.record(sample(3, 0.5, kScavenge, true)));
    CHECK(ch.record(sample(4, 1.25, kMarkSweepCompact, true)));
    CHECK(ch.record(sample(5, 0.75, kScavenge, false))); // mid-tick pause

    const profile::GcPauseAggregates& a = ch.aggregates();
    CHECK(a.pause_count == 3);
    CHECK(a.in_window_count == 2);
    CHECK(a.total_pause_ms > 2.49 && a.total_pause_ms < 2.51);
    CHECK(a.max_pause_ms == 1.25);
    CHECK(a.max_mid_tick_pause_ms == 0.75);
    CHECK(a.dropped == 0);
    CHECK(ch.samples().size() == 3);
    CHECK(ch.samples()[0].tick == 3);
    CHECK(ch.samples()[2].in_window == false);
    CHECK(!ch.overflowed());

    // Budget verdict: 1.25 ms worst pause fits a 2 ms budget, breaches a 1 ms budget.
    CHECK(ch.within_budget(2.0));
    CHECK(!ch.within_budget(1.0));
}

// --- edge: incremental-marking cycles are samples, never pause aggregates -------------------------
void test_incremental_marking_excluded()
{
    CHECK(profile::gc_kind_is_pause(kScavenge));
    CHECK(profile::gc_kind_is_pause(kMarkSweepCompact));
    CHECK(profile::gc_kind_is_pause(0)); // unknown kind counts as a pause (fail-closed)
    CHECK(!profile::gc_kind_is_pause(kIncrementalMarking));

    profile::GcPauseChannel ch(16);
    // A 50 ms incremental CYCLE bracket must not poison the pause aggregates...
    CHECK(ch.record(sample(1, 50.0, kIncrementalMarking, false)));
    CHECK(ch.aggregates().pause_count == 0);
    CHECK(ch.aggregates().max_pause_ms == 0.0);
    CHECK(ch.within_budget(1.0));
    // ...but IS retained as a kind-tagged sample for attribution.
    CHECK(ch.samples().size() == 1);
    CHECK(ch.samples()[0].kind == kIncrementalMarking);
}

// --- edge: sample-list capacity bounds retention; aggregates never drop ---------------------------
void test_capacity_overflow()
{
    profile::GcPauseChannel ch(2);
    CHECK(ch.sample_capacity() == 2);
    CHECK(ch.record(sample(1, 0.1, kScavenge, true)));
    CHECK(ch.record(sample(2, 0.2, kScavenge, true)));
    CHECK(!ch.record(sample(3, 0.9, kScavenge, false))); // over capacity: not retained...
    CHECK(ch.overflowed());
    CHECK(ch.samples().size() == 2);
    CHECK(ch.aggregates().pause_count == 3); // ...but STILL aggregated
    CHECK(ch.aggregates().max_mid_tick_pause_ms == 0.9);

    // A zero requested capacity clamps to 1 (a channel that can retain nothing is useless).
    profile::GcPauseChannel tiny(0);
    CHECK(tiny.sample_capacity() == 1);
}

// --- failure: engine-side record loss fails the budget verdict (fail-closed) ----------------------
void test_dropped_fails_budget()
{
    profile::GcPauseChannel ch(16);
    CHECK(ch.record(sample(1, 0.1, kScavenge, true)));
    CHECK(ch.within_budget(1.0));
    ch.record_dropped(1); // a lost record could hide a breach
    CHECK(ch.aggregates().dropped == 1);
    CHECK(!ch.within_budget(1.0));
}

// --- clear resets everything -----------------------------------------------------------------------
void test_clear()
{
    profile::GcPauseChannel ch(2);
    ch.record(sample(1, 0.1, kScavenge, true));
    ch.record(sample(2, 0.2, kScavenge, true));
    ch.record(sample(3, 0.3, kScavenge, true));
    ch.record_dropped(2);
    ch.clear();
    CHECK(ch.samples().empty());
    CHECK(ch.aggregates().pause_count == 0);
    CHECK(ch.aggregates().dropped == 0);
    CHECK(!ch.overflowed());
    CHECK(ch.within_budget(0.0));
}
} // namespace

int main()
{
    test_record_and_aggregates();
    test_incremental_marking_excluded();
    test_capacity_overflow();
    test_dropped_fails_budget();
    test_clear();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("gc_channel: all checks passed\n");
    return 0;
}
