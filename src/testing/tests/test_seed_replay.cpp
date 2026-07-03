// Seed reproducibility (R-QA-011): the core guarantee that makes failing seeds actionable — the SAME
// seed drives the SAME scenario outcome for every fault class, so a failure can be replayed,
// minimized, and committed. Runs each driver twice per seed and asserts identical results.

#include "context/testing/fault_scenarios.h"
#include "testing_test.h"

#include <cstdint>

using namespace context::testing;

int main()
{
    for (std::uint64_t seed = 0; seed < 128; ++seed)
    {
        // Crash scenario replays identically.
        {
            const CrashScenarioResult a = run_crash_recovery_scenario(seed);
            const CrashScenarioResult b = run_crash_recovery_scenario(seed);
            CHECK(a.writes == b.writes);
            CHECK(a.crash_index == b.crash_index);
            CHECK(a.observed_crash == b.observed_crash);
            CHECK(a.no_torn_state == b.no_torn_state);
            CHECK(a.recovered == b.recovered);
            CHECK(a.idempotent_replay == b.idempotent_replay);
            CHECK(a.converged() == b.converged());
        }
        // Watcher scenario replays identically.
        {
            const WatcherScenarioResult a = run_watcher_fault_scenario(seed);
            const WatcherScenarioResult b = run_watcher_fault_scenario(seed);
            CHECK(a.files == b.files);
            CHECK(a.mutations == b.mutations);
            CHECK(a.dropped == b.dropped);
            CHECK(a.duplicated == b.duplicated);
            CHECK(a.reordered == b.reordered);
            CHECK(a.converged == b.converged);
        }
        // Slow-client scenario replays identically.
        {
            const SlowClientScenarioResult a = run_slow_client_scenario(seed);
            const SlowClientScenarioResult b = run_slow_client_scenario(seed);
            CHECK(a.published == b.published);
            CHECK(a.capacity == b.capacity);
            CHECK(a.never_blocked == b.never_blocked);
            CHECK(a.gap_flagged == b.gap_flagged);
            CHECK(a.recovery_defined == b.recovery_defined);
            CHECK(a.converged() == b.converged());
        }
    }

    TESTING_TEST_MAIN_END();
}
