// R-QA-013 test for the Session inter-tick hook (M6 X1 — the R-SIM-008 / R-HEAD-002 tick-boundary
// service point the JS GC window is scheduled through). Asserts the hook's mechanics: it fires
// exactly once per completed tick (in order, across multiple step() calls), it observes the gap
// BETWEEN ticks (the trace already recorded the tick when it runs), it is clearable, and a
// well-behaved hook leaves the hierarchical state hash byte-identical (the local half of the
// invariance the V8-backed js-test_gc_state_hash proves with a real GC).

#include "context/runtime/session/session.h"

#include "session_test.h"

#include <cstdint>
#include <vector>

using context::runtime::session::ActionActivation;
using context::runtime::session::Session;
using context::runtime::session::SessionConfig;
using context::runtime::session::StepResult;

namespace
{
Session make_session()
{
    SessionConfig config;
    config.seed = 7;
    Session s(config);
    s.set_trace(true);
    s.inject_action_at(0, ActionActivation{"move_x", "performed", 3});
    s.inject_action_at(3, ActionActivation{"move_y", "performed", -2});
    return s;
}

// The hook fires once per completed tick, in order, across split step() calls — and it runs
// AFTER the tick's trace entry exists (the inter-tick gap, not mid-tick).
void test_fires_per_tick_in_order()
{
    Session s = make_session();
    std::vector<std::uint64_t> seen;
    std::vector<std::size_t> trace_sizes;
    s.set_inter_tick_hook(
        [&](std::uint64_t completed_tick)
        {
            seen.push_back(completed_tick);
            trace_sizes.push_back(s.trace().size());
        });
    CHECK(s.has_inter_tick_hook());

    s.step(3);
    s.step(2);

    CHECK(seen.size() == 5);
    for (std::uint64_t i = 0; i < 5; ++i)
        CHECK(seen[i] == i);
    // When the hook for tick T ran, T's trace entry (T+1 entries total) was already recorded.
    for (std::size_t i = 0; i < trace_sizes.size(); ++i)
        CHECK(trace_sizes[i] == i + 1);
}

void test_clear()
{
    Session s = make_session();
    std::size_t fired = 0;
    s.set_inter_tick_hook([&fired](std::uint64_t) { ++fired; });
    s.step(2);
    CHECK(fired == 2);
    s.clear_inter_tick_hook();
    CHECK(!s.has_inter_tick_hook());
    s.step(2);
    CHECK(fired == 2); // cleared: no further fires
}

// A hook doing off-state work leaves every hash identical to a bare run (the invariance contract
// session.h states; the JS-GC version of this proof is js-test_gc_state_hash on the CI legs).
void test_hash_invariance()
{
    Session bare = make_session();
    const StepResult bare_result = bare.step(6);

    Session hooked = make_session();
    hooked.set_inter_tick_hook(
        [](std::uint64_t completed_tick)
        {
            std::vector<int> scratch(512, static_cast<int>(completed_tick)); // off-state churn
            (void)scratch;
        });
    const StepResult hooked_result = hooked.step(6);

    CHECK(bare_result.sim_tick == hooked_result.sim_tick);
    CHECK(bare_result.state_hash.root == hooked_result.state_hash.root);
    CHECK(bare.trace().size() == hooked.trace().size());
    for (std::size_t i = 0; i < bare.trace().size(); ++i)
    {
        CHECK(bare.trace()[i].tick == hooked.trace()[i].tick);
        CHECK(bare.trace()[i].root == hooked.trace()[i].root);
    }
}
} // namespace

int main()
{
    test_fires_per_tick_in_order();
    test_clear();
    test_hash_invariance();
    SESSION_TEST_MAIN_END();
}
