// Crash points between durable steps (R-QA-010 -> R-FILE-004): a multi-file write op is crashed at a
// seeded durable step and must resume-or-diagnose. Fuzzed across a committed seed sweep; every seed
// must converge — the crash fires, no torn/partial state is ever observed, recovery completes every
// write, and a second recovery is a no-op.

#include "context/testing/fault_scenarios.h"
#include "testing_test.h"

#include <cstdint>

using namespace context::testing;

// Committed regression seeds (R-QA-011): any minimized failing seed is pinned here forever so its
// exact fault schedule replays on every platform.
static const std::uint64_t kRegressionSeeds[] = {0, 1, 7, 42, 1337, 0xDEADBEEFu};

int main()
{
    for (std::uint64_t seed = 0; seed < 512; ++seed)
    {
        const CrashScenarioResult r = run_crash_recovery_scenario(seed);
        CHECK(r.observed_crash);    // the injected crash actually fired
        CHECK(r.no_torn_state);     // every file held prev OR target — never a partial write
        CHECK(r.recovered);         // resumed to completion, log cleared, no diagnostics
        CHECK(r.idempotent_replay); // a second recovery changed nothing
        CHECK(r.converged());
    }

    for (const std::uint64_t seed : kRegressionSeeds)
        CHECK(run_crash_recovery_scenario(seed).converged());

    // Shape assertions: the scenario really planned a multi-file op and armed a crash within it.
    const CrashScenarioResult sample = run_crash_recovery_scenario(42);
    CHECK(sample.writes >= 2);
    CHECK(sample.crash_index < sample.writes);

    TESTING_TEST_MAIN_END();
}
