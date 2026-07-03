// Slow-client queue overflow (R-QA-010 -> R-BRIDGE-008): a bounded subscriber is flooded past its
// capacity without draining. The stream must NEVER block (seqs stay strictly monotonic), the
// overflow must raise the gap marker, and a defined recovery — replay-since when the ring still
// holds the missed events, else a fresh snapshot — must reach the latest seq. Fuzzed across a
// committed seed sweep; every seed must converge.

#include "context/testing/fault_scenarios.h"
#include "testing_test.h"

#include <cstdint>

using namespace context::testing;

// Committed regression seeds (R-QA-011): any minimized failing seed is pinned here forever.
static const std::uint64_t kRegressionSeeds[] = {0, 2, 13, 64, 2048, 0xBADF00Du};

int main()
{
    bool saw_gapped = false; // at least one seed evicted the missed events -> fresh-snapshot recovery
    bool saw_replay = false; // at least one seed kept them in the ring     -> replay-since recovery

    for (std::uint64_t seed = 0; seed < 512; ++seed)
    {
        const SlowClientScenarioResult r = run_slow_client_scenario(seed);
        CHECK(r.never_blocked);    // the stream advanced seqs monotonically — never stalled on the client
        CHECK(r.gap_flagged);      // the overflow raised the gap marker
        CHECK(r.recovery_defined); // replay-since or a fresh snapshot reaches the latest seq
        CHECK(r.converged());
        CHECK(r.published > r.capacity); // the flood really overflowed the bounded queue
        saw_gapped = saw_gapped || r.recovery_gapped;
        saw_replay = saw_replay || !r.recovery_gapped;
    }

    for (const std::uint64_t seed : kRegressionSeeds)
        CHECK(run_slow_client_scenario(seed).converged());

    // The sweep must actually drive BOTH recovery paths (else the test proves nothing).
    CHECK(saw_gapped);
    CHECK(saw_replay);

    TESTING_TEST_MAIN_END();
}
