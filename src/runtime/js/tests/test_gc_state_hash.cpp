// R-QA-013 — the M6 X1 determinism proof: a JS-heap GC inside the scheduled inter-tick window
// leaves the session's hierarchical state hash UNCHANGED. Two identical sessions (same seed, same
// injected input stream, trace mode) are stepped; one runs bare, the other runs mid-tick JS churn
// (via the read-only system observer) + a forced GC window at every tick boundary (via the
// inter-tick hook), with the L-47 GC-pause channel draining the pauses. Every per-tick root,
// per-system hash, and archetype hash must match byte-for-byte: GC changes TIMING, never logical
// STATE (the kernel World is unreachable from the VM's collector by construction — session.h).
//
// CI-only for its V8 dependency path (the CONTEXT_JS_HAS_V8 split); the local stub toolchain
// proves the inter-tick hook MECHANICS instead — the hook fires once per completed tick, in
// order, and a hook doing pure C++ work leaves the trace identical — so the session half of the
// invariant is still gated locally.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "context/runtime/js/js_host.h"
#include "context/runtime/profile/gc_channel.h"
#include "context/runtime/session/session.h"

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

namespace cjs = context::runtime::js;
namespace profile = context::runtime::profile;
namespace session = context::runtime::session;

namespace
{
constexpr std::uint64_t kSeed = 42;
constexpr std::uint64_t kTicks = 8;

// A deterministic, input-driven session: the same seed + stream on every run.
session::Session make_session()
{
    session::SessionConfig config;
    config.seed = kSeed;
    session::Session s(config);
    s.set_trace(true);
    s.inject_action_at(0, session::ActionActivation{"move_x", "performed", 2});
    s.inject_action_at(2, session::ActionActivation{"move_y", "performed", -1});
    s.inject_action_at(4, session::ActionActivation{"fire", "performed", 1});
    s.inject_event_at(5, session::InputEvent{"keyboard", "W", 1});
    return s;
}

// Compare two hash traces plus the final full hash, byte-for-byte.
void check_traces_equal(const session::Session& a, const session::Session& b)
{
    const session::HashTrace& ta = a.trace();
    const session::HashTrace& tb = b.trace();
    CHECK(ta.size() == tb.size());
    for (std::size_t i = 0; i < ta.size() && i < tb.size(); ++i)
    {
        CHECK(ta[i].tick == tb[i].tick);
        CHECK(ta[i].root == tb[i].root);
        CHECK(ta[i].per_system.size() == tb[i].per_system.size());
        for (std::size_t j = 0; j < ta[i].per_system.size() && j < tb[i].per_system.size(); ++j)
        {
            CHECK(ta[i].per_system[j].system == tb[i].per_system[j].system);
            CHECK(ta[i].per_system[j].hash == tb[i].per_system[j].hash);
        }
        CHECK(ta[i].per_archetype.size() == tb[i].per_archetype.size());
        for (std::size_t j = 0; j < ta[i].per_archetype.size() && j < tb[i].per_archetype.size();
             ++j)
        {
            CHECK(ta[i].per_archetype[j].signature == tb[i].per_archetype[j].signature);
            CHECK(ta[i].per_archetype[j].hash == tb[i].per_archetype[j].hash);
        }
    }
    const session::StateHash ha = a.state_hash();
    const session::StateHash hb = b.state_hash();
    CHECK(ha.root == hb.root);
}
} // namespace

#ifdef CONTEXT_JS_HAS_V8

int main()
{
    // Baseline: the bare session.
    session::Session baseline = make_session();
    const session::StepResult baseline_result = baseline.step(kTicks);
    CHECK(baseline_result.sim_tick == kTicks);

    // The GC run: identical session + mid-tick JS churn + a forced GC window every tick boundary.
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (engine == nullptr)
    {
        std::fprintf(stderr, "createV8Engine failed: %s\n", err.c_str());
        return 1;
    }

    session::Session gc_run = make_session();
    profile::GcPauseChannel channel;

    // Mid-tick JS allocation churn, injected through the read-only system observer (it runs
    // DURING the tick, after each system — exactly where gameplay JS would allocate).
    gc_run.set_system_observer(
        [&engine](std::uint64_t, std::size_t, const std::string&, const context::kernel::World&)
        {
            std::string eval_err;
            const bool ok = engine->eval(
                "(function () { var a = []; for (var i = 0; i < 3000; i++) { a.push({x: i}); } "
                "return a.length; })()",
                nullptr, eval_err);
            CHECK(ok);
        });

    // The scheduled inter-tick GC window (R-SIM-008): collect in the gap, drain into the channel.
    std::uint64_t windows = 0;
    gc_run.set_inter_tick_hook(
        [&engine, &channel, &windows](std::uint64_t completed_tick)
        {
            cjs::GcWindowOptions options;
            options.budget_ms = 8.0;
            options.force_collect = true;
            cjs::GcWindowResult result;
            std::string window_err;
            CHECK(engine->gcWindow(options, result, window_err));
            CHECK(result.collected);
            channel.drain(*engine, completed_tick);
            ++windows;
        });
    CHECK(gc_run.has_inter_tick_hook());

    const session::StepResult gc_result = gc_run.step(kTicks);
    CHECK(gc_result.sim_tick == kTicks);
    CHECK(windows == kTicks);

    // THE invariant: every hash is byte-identical — the GC touched timing, never logical state.
    CHECK(baseline_result.state_hash.root == gc_result.state_hash.root);
    check_traces_equal(baseline, gc_run);

    // And the L-47 channel actually observed attributed window pauses while proving it.
    CHECK(channel.aggregates().pause_count >= 1);
    CHECK(channel.aggregates().in_window_count >= 1);
    CHECK(channel.aggregates().dropped == 0);

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("gc_state_hash (V8): GC in the inter-tick window left the state hash unchanged\n");
    return 0;
}

#else // !CONTEXT_JS_HAS_V8 — the local stub toolchain: prove the hook mechanics + invariance

int main()
{
    session::Session baseline = make_session();
    const session::StepResult baseline_result = baseline.step(kTicks);

    session::Session hooked = make_session();
    std::vector<std::uint64_t> seen;
    hooked.set_inter_tick_hook(
        [&seen](std::uint64_t completed_tick)
        {
            // Pure C++ churn in the gap — off the logical state path, like a GC window.
            std::vector<int> scratch(1024, static_cast<int>(completed_tick));
            seen.push_back(completed_tick);
            (void)scratch;
        });
    const session::StepResult hooked_result = hooked.step(kTicks);

    CHECK(seen.size() == kTicks);
    for (std::uint64_t i = 0; i < kTicks; ++i)
        CHECK(seen[i] == i); // fires once per completed tick, in order
    CHECK(baseline_result.state_hash.root == hooked_result.state_hash.root);
    check_traces_equal(baseline, hooked);

    // The V8 backend is correctly absent on this toolchain (the CI legs run the full proof).
    std::string err;
    CHECK(cjs::createV8Engine(err) == nullptr);

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("gc_state_hash (stub): hook mechanics + invariance proven; V8 proof on CI legs\n");
    return 0;
}

#endif
