// The REAL play session-control adapter (R-QA-013): RuntimeSessionControl drives an actual
// context::runtime::session::Session (the demo scenario) — start/step/stop/hot-reload — and extracts
// the render frame the F1 viewport observes. Proves the playbar drives the runtime session for real
// (not just a double), that stepping advances the fixed-tick simulation, and that L-51/L-22 semantics
// hold on the real session. The fail-closed play.* codes are covered by the fault double in
// test_playbar_model.cpp; here the adapter's own defensive guards are asserted.

#include "context/editor/gui/playbar/playbar_model.h"
#include "context/editor/gui/playbar/session_control.h"

#include "context/runtime/session/session.h"

#include "playbar_test.h"

#include <cstdint>

using namespace context::editor::gui::playbar;
namespace rsession = context::runtime::session;

namespace
{

[[nodiscard]] rsession::SessionConfig demo_config()
{
    rsession::SessionConfig cfg;
    cfg.seed = 42;
    cfg.tick_hz = 60;
    cfg.scenario = "demo";
    return cfg;
}

} // namespace

int main()
{
    // --- start drives a real session; step advances the fixed-tick simulation --------------------
    {
        RuntimeSessionControl control(demo_config());
        CHECK(!control.running());

        ControlOutcome started = control.start();
        CHECK(started.ok);
        CHECK(control.running());
        CHECK(started.frame.sim_tick == 0);

        ControlOutcome stepped = control.step(3);
        CHECK(stepped.ok);
        CHECK(stepped.frame.sim_tick == 3); // the REAL session advanced exactly 3 fixed ticks
        // the extracted frame stamps the same tick it was extracted at (L-39 snapshot).
        CHECK(stepped.frame.snapshot.sim_tick == 3);

        control.discard();
        CHECK(!control.running());
    }

    // --- two adapters, same config + same steps => identical simTick (determinism sanity) ---------
    {
        RuntimeSessionControl a(demo_config());
        RuntimeSessionControl b(demo_config());
        CHECK(a.start().ok);
        CHECK(b.start().ok);
        const ControlOutcome sa = a.step(10);
        const ControlOutcome sb = b.step(10);
        CHECK(sa.frame.sim_tick == sb.frame.sim_tick);
        CHECK(sa.frame.sim_tick == 10);
    }

    // --- L-22 live-preserving reload keeps the running session's tick; restart-class resets it -----
    {
        RuntimeSessionControl control(demo_config());
        CHECK(control.start().ok);
        (void)control.step(5);

        HotReloadOutcome live = control.apply_hot_reload(LiveEdit{"/components/x/value", false});
        CHECK(live.ok);
        CHECK(live.reload_class == HotReloadClass::live_preserving);
        CHECK(live.state_preserved);
        CHECK(live.frame.sim_tick == 5); // preserved

        HotReloadOutcome restart = control.apply_hot_reload(LiveEdit{"/components/x", true});
        CHECK(restart.ok);
        CHECK(restart.reload_class == HotReloadClass::restart_class);
        CHECK(!restart.state_preserved);
        CHECK(restart.frame.sim_tick == 0); // re-instantiated from the new derivation
        CHECK(control.running());
    }

    // --- defensive adapter guards: step / hot-reload before start fail closed ----------------------
    {
        RuntimeSessionControl control(demo_config());
        ControlOutcome s = control.step(1);
        CHECK(!s.ok);
        CHECK(s.error_code == kPlayStepFailedCode);

        HotReloadOutcome h = control.apply_hot_reload(LiveEdit{"/x", false});
        CHECK(!h.ok);
        CHECK(h.error_code == kPlayHotReloadFailedCode);
    }

    PLAYBAR_TEST_MAIN_END();
}
