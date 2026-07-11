// The playbar state machine (R-QA-013: happy / edge / failure) over a fault-injecting SessionControl
// double — the DoD's "runtime-session fault-injection" without a live daemon. Covers the L-51 edit/play
// transitions (play/pause/resume/stop/step), the L-22 hot-reload classification, every reserved play.*
// failure code, and the R-HUX-011 control-loop listener + generation counter.

#include "context/editor/gui/playbar/playbar_model.h"
#include "context/editor/gui/playbar/session_control.h"

#include "playbar_test.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace context::editor::gui::playbar;

namespace
{

// A configurable SessionControl double: records calls, advances a simulated simTick, and can fail-close
// any op to exercise the reserved play.* codes (runtime-session fault-injection).
class StubSessionControl : public SessionControl
{
public:
    bool fail_start = false;
    bool fail_step = false;
    bool fail_hot_reload = false;
    std::uint32_t drawables = 0; // how many drawables the produced frame carries

    int starts = 0;
    int steps = 0;
    int discards = 0;
    int reloads = 0;
    std::uint64_t tick = 0;
    bool running = false;

    ControlOutcome start() override
    {
        ++starts;
        ControlOutcome out;
        if (fail_start)
        {
            out.error_code = kPlaySessionFailedCode;
            return out;
        }
        running = true;
        tick = 0;
        out.ok = true;
        out.frame = frame();
        return out;
    }

    ControlOutcome step(std::uint64_t ticks) override
    {
        ++steps;
        ControlOutcome out;
        if (fail_step)
        {
            out.error_code = kPlayStepFailedCode;
            return out;
        }
        tick += ticks;
        out.ok = true;
        out.frame = frame();
        return out;
    }

    void discard() override
    {
        ++discards;
        running = false;
        tick = 0;
    }

    HotReloadOutcome apply_hot_reload(const LiveEdit& edit) override
    {
        ++reloads;
        HotReloadOutcome out;
        if (fail_hot_reload)
        {
            out.error_code = kPlayHotReloadFailedCode;
            return out;
        }
        if (edit.shape_or_layout_change)
        {
            out.reload_class = HotReloadClass::restart_class;
            out.state_preserved = false;
            tick = 0; // restart-class re-instantiates from tick 0
        }
        else
        {
            out.reload_class = HotReloadClass::live_preserving;
            out.state_preserved = true;
        }
        out.ok = true;
        out.frame = frame();
        return out;
    }

private:
    [[nodiscard]] PlayFrame frame() const
    {
        PlayFrame f;
        f.sim_tick = tick;
        f.snapshot.sim_tick = tick;
        f.snapshot.items.resize(drawables);
        return f;
    }
};

} // namespace

int main()
{
    // --- default (edit) state ---------------------------------------------------------------------
    {
        PlaybarModel model;
        CHECK(model.state() == PlayState::edit);
        CHECK(!model.is_running());
        CHECK(model.sim_tick() == 0);
        CHECK(model.control_generation() == 0);
        CHECK(model.last_error().empty());
    }

    // --- happy path: play -> step -> pause -> resume -> step -> stop -------------------------------
    {
        StubSessionControl control;
        control.drawables = 3;
        PlaybarModel model(&control);

        std::vector<std::string> transitions;
        model.add_control_listener([&](const PlayControlEvent& e) { transitions.push_back(e.transition); });

        PlayAction a = model.play();
        CHECK(a.ok);
        CHECK(a.state == PlayState::playing);
        CHECK(model.is_running());
        CHECK(control.starts == 1);
        CHECK(model.control_generation() == 1);
        // the play frame flows through as the observed frame (F1 viewport source) — 3 drawables.
        CHECK(model.last_frame().snapshot.items.size() == 3);

        PlayAction s1 = model.step(2);
        CHECK(s1.ok);
        CHECK(model.state() == PlayState::playing);
        CHECK(model.sim_tick() == 2); // the real session advanced 2 fixed ticks
        CHECK(control.steps == 1);

        PlayAction p = model.pause();
        CHECK(p.ok);
        CHECK(model.state() == PlayState::paused);

        // step while PAUSED still advances (transport step), state stays paused.
        PlayAction s2 = model.step(1);
        CHECK(s2.ok);
        CHECK(model.state() == PlayState::paused);
        CHECK(model.sim_tick() == 3);

        PlayAction r = model.play(); // resume
        CHECK(r.ok);
        CHECK(model.state() == PlayState::playing);
        CHECK(control.starts == 1); // resume does NOT re-start a session

        PlayAction st = model.stop();
        CHECK(st.ok);
        CHECK(model.state() == PlayState::edit);
        CHECK(!model.is_running());
        CHECK(control.discards == 1);
        CHECK(model.sim_tick() == 0); // the observed frame is cleared on stop (L-51 discard)

        // R-HUX-011 loop fired once per transition, in order.
        const std::vector<std::string> want{"play", "step", "pause", "step", "resume", "stop"};
        CHECK(transitions == want);
        CHECK(model.control_generation() == 6);
    }

    // --- edit-state guards: pause / step / hot_reload with no live session => play.not_running -----
    {
        PlaybarModel model; // no control
        PlayAction p = model.pause();
        CHECK(!p.ok);
        CHECK(p.error_code == kPlayNotRunningCode);
        CHECK(model.state() == PlayState::edit);

        PlayAction s = model.step();
        CHECK(!s.ok);
        CHECK(s.error_code == kPlayNotRunningCode);

        HotReloadOutcome h = model.hot_reload(LiveEdit{"/components/transform/position", false});
        CHECK(!h.ok);
        CHECK(h.error_code == kPlayNotRunningCode);
    }

    // --- stop is idempotent in edit; already-playing play / already-paused pause are benign no-ops --
    {
        StubSessionControl control;
        PlaybarModel model(&control);
        CHECK(model.stop().ok);              // stop in edit: benign ok, stays edit
        CHECK(model.state() == PlayState::edit);

        CHECK(model.play().ok);
        PlayAction again = model.play();     // already playing
        CHECK(!again.ok);
        CHECK(again.error_code.empty());     // not an error — nothing changed
        CHECK(model.state() == PlayState::playing);

        CHECK(model.pause().ok);
        PlayAction again_p = model.pause();  // already paused
        CHECK(!again_p.ok);
        CHECK(again_p.error_code.empty());
        CHECK(model.state() == PlayState::paused);
    }

    // --- failure: start refused (fail-closed) => play.session_failed, stays edit -------------------
    {
        StubSessionControl control;
        control.fail_start = true;
        PlaybarModel model(&control);
        PlayAction a = model.play();
        CHECK(!a.ok);
        CHECK(a.error_code == kPlaySessionFailedCode);
        CHECK(model.state() == PlayState::edit);
        CHECK(model.last_error() == kPlaySessionFailedCode);
    }

    // --- failure: step refused => play.step_failed, stays playing ---------------------------------
    {
        StubSessionControl control;
        control.fail_step = true;
        PlaybarModel model(&control);
        CHECK(model.play().ok);
        PlayAction s = model.step();
        CHECK(!s.ok);
        CHECK(s.error_code == kPlayStepFailedCode);
        CHECK(model.state() == PlayState::playing); // the session is still live; the step failed
    }

    // --- L-22 hot reload: live-preserving (data value) vs restart-class (shape/layout) -------------
    {
        StubSessionControl control;
        PlaybarModel model(&control);
        CHECK(model.play().ok);
        model.step(5);
        CHECK(model.sim_tick() == 5);

        // data-value edit -> live-preserving; state kept.
        HotReloadOutcome live = model.hot_reload(LiveEdit{"/components/light/intensity", false});
        CHECK(live.ok);
        CHECK(live.reload_class == HotReloadClass::live_preserving);
        CHECK(live.state_preserved);
        CHECK(model.sim_tick() == 5); // runtime state preserved across the reload

        // shape/layout change -> restart-class; state discarded + re-instantiated (loud event).
        HotReloadOutcome restart = model.hot_reload(LiveEdit{"/components/transform", true});
        CHECK(restart.ok);
        CHECK(restart.reload_class == HotReloadClass::restart_class);
        CHECK(!restart.state_preserved);
        CHECK(model.sim_tick() == 0); // re-instantiated from tick 0
        CHECK(model.state() == PlayState::playing);
    }

    // --- failure: hot reload refused => play.hot_reload_failed -------------------------------------
    {
        StubSessionControl control;
        control.fail_hot_reload = true;
        PlaybarModel model(&control);
        CHECK(model.play().ok);
        HotReloadOutcome h = model.hot_reload(LiveEdit{"/x", false});
        CHECK(!h.ok);
        CHECK(h.error_code == kPlayHotReloadFailedCode);
    }

    PLAYBAR_TEST_MAIN_END();
}
