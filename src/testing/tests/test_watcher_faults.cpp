// Watcher event loss / duplication / reordering (R-QA-010 -> R-FILE-002): external edits whose
// watcher hints are dropped / duplicated / reordered must still converge. Content hashing is
// authoritative and the full re-hash crawl is the safety net, so the reconciler's index must end up
// matching true on-disk content for every path regardless of what happened to the hints. Fuzzed
// across a committed seed sweep; every seed must converge.

#include "context/testing/fault_scenarios.h"
#include "testing_test.h"

#include <cstdint>

using namespace context::testing;

// Committed regression seeds (R-QA-011): any minimized failing seed is pinned here forever.
static const std::uint64_t kRegressionSeeds[] = {0, 3, 11, 99, 4096, 0xC0FFEEu};

int main()
{
    bool saw_drop = false;
    bool saw_dup = false;
    bool saw_reorder = false;

    for (std::uint64_t seed = 0; seed < 512; ++seed)
    {
        const WatcherScenarioResult r = run_watcher_fault_scenario(seed);
        CHECK(r.converged); // index == true on-disk content for every path after the crawl
        saw_drop = saw_drop || r.dropped;
        saw_dup = saw_dup || r.duplicated;
        saw_reorder = saw_reorder || r.reordered;
    }

    for (const std::uint64_t seed : kRegressionSeeds)
        CHECK(run_watcher_fault_scenario(seed).converged);

    // The sweep must actually exercise all three fault classes (else the test proves nothing).
    CHECK(saw_drop);
    CHECK(saw_dup);
    CHECK(saw_reorder);

    TESTING_TEST_MAIN_END();
}
