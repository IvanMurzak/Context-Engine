// R-QA-013 test for the Session per-system span sink (a15 — the L-47 / R-OBS-004 profiler-span
// seam). Asserts the sink's mechanics: it fires once per system per tick in scheduler order (name +
// index), across split step() calls, with a finite non-negative duration; it is clearable; and —
// the load-bearing determinism invariant — installing the sink leaves the hierarchical state hash
// byte-identical (a span is a wall-clock measurement OFF the logical state path, so it cannot
// perturb the World).

#include "context/runtime/session/session.h"

#include "session_test.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using context::runtime::session::ActionActivation;
using context::runtime::session::Session;
using context::runtime::session::SessionConfig;
using context::runtime::session::StateHash;
using context::runtime::session::StepResult;

namespace
{
Session make_session()
{
    SessionConfig config;
    config.seed = 11;
    Session s(config);
    s.inject_action_at(0, ActionActivation{"move_x", "performed", 3});
    s.inject_action_at(2, ActionActivation{"move_y", "performed", -1});
    return s;
}

struct Span
{
    std::uint64_t tick;
    std::size_t index;
    std::string name;
    double duration_ms;
};

// The sink fires once per system per tick, in scheduler order, across split step() calls.
void test_fires_per_system_in_order()
{
    Session s = make_session();
    const std::vector<std::string> names = s.system_names(); // input / control / motion
    CHECK(!names.empty());

    std::vector<Span> spans;
    CHECK(!s.has_system_span_sink());
    s.set_system_span_sink(
        [&](std::uint64_t tick, std::size_t index, const std::string& name, double duration_ms)
        { spans.push_back(Span{tick, index, name, duration_ms}); });
    CHECK(s.has_system_span_sink());

    s.step(2);
    s.step(1); // split call — the sink keeps firing

    const std::size_t per_tick = names.size();
    CHECK(spans.size() == per_tick * 3); // 3 ticks

    for (std::size_t t = 0; t < 3; ++t)
    {
        for (std::size_t i = 0; i < per_tick; ++i)
        {
            const Span& sp = spans[t * per_tick + i];
            CHECK(sp.tick == t);
            CHECK(sp.index == i);
            CHECK(sp.name == names[i]);
            CHECK(std::isfinite(sp.duration_ms));
            CHECK(sp.duration_ms >= 0.0);
        }
    }

    // Clearable: no more spans after clear.
    s.clear_system_span_sink();
    CHECK(!s.has_system_span_sink());
    const std::size_t before = spans.size();
    s.step(1);
    CHECK(spans.size() == before);
}

// The determinism invariant: a session steps to a byte-identical hierarchical hash whether or not a
// span sink is installed (spans never touch the World).
void test_sink_does_not_perturb_state()
{
    Session plain = make_session();
    const StepResult r_plain = plain.step(5);

    Session sunk = make_session();
    std::uint64_t count = 0;
    sunk.set_system_span_sink(
        [&](std::uint64_t, std::size_t, const std::string&, double) { ++count; });
    const StepResult r_sunk = sunk.step(5);

    CHECK(count > 0); // the sink actually ran
    CHECK(r_plain.sim_tick == r_sunk.sim_tick);
    CHECK(r_plain.state_hash.root == r_sunk.state_hash.root); // bit-identical
}
} // namespace

int main()
{
    test_fires_per_system_in_order();
    test_sink_does_not_perturb_state();
    SESSION_TEST_MAIN_END();
}
