// The playbar transport (R-QA-013: happy / edge / failure) over a PlayControlGateway double — M9 e08b.
//
// ⚠ THE DOUBLE IS DELIBERATELY NO MORE CAPABLE THAN THE DAEMON. `DaemonLikeGateway` below reproduces
// `EditorSessionState`'s reply shapes exactly (kernel_server.cpp / editor_session_state.cpp):
//   * a real transition          -> ok=true,  changed=true,  the new state + simTick
//   * a benign idempotent no-op  -> ok=true,  changed=false, the UNCHANGED state (nothing published)
//   * a refusal                  -> ok=false, error_code=play.not_running
// A double that answered `ok=true, changed=true` for a no-op — or that invented a state the daemon
// would never return — would let this suite pass over a model the real daemon breaks. The cross-
// process `editor-session-panels-t2` drill is what proves the two agree; this suite is what makes the
// transport's own edges cheap to cover.
//
// Covers the four RPC writes, the `play-state` SUBSCRIBER half (a second client's change), the
// ok<-changed mapping, refusal propagation, the no-gateway posture, and the R-HUX-011 control-loop
// listener + generation counter.

#include "context/editor/gui/playbar/playbar_model.h"

#include "playbar_test.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace context::editor::gui::playbar;

namespace
{

// The daemon's L-51 machine, reply-for-reply (see the header note above).
class DaemonLikeGateway final : public PlayControlGateway
{
public:
    int plays = 0;
    int pauses = 0;
    int stops = 0;
    int steps = 0;
    // Force every command to fail-close with this code (empty = behave like the real machine).
    std::string force_error;

    [[nodiscard]] int calls() const noexcept { return plays + pauses + stops + steps; }
    [[nodiscard]] PlayState daemon_state() const noexcept { return state_; }

    PlayCommandResult play() override
    {
        ++plays;
        if (!force_error.empty())
        {
            return refusal();
        }
        if (state_ == PlayState::playing)
        {
            return no_op(); // already playing — benign
        }
        state_ = PlayState::playing;
        return changed();
    }

    PlayCommandResult pause() override
    {
        ++pauses;
        if (!force_error.empty())
        {
            return refusal();
        }
        if (state_ == PlayState::edit)
        {
            return refusal(kPlayNotRunningCode); // nothing to pause
        }
        if (state_ == PlayState::paused)
        {
            return no_op();
        }
        state_ = PlayState::paused;
        return changed();
    }

    PlayCommandResult stop() override
    {
        ++stops;
        if (!force_error.empty())
        {
            return refusal();
        }
        if (state_ == PlayState::edit)
        {
            return no_op(); // idempotent, like a stopped transport bar
        }
        state_ = PlayState::edit;
        tick_ = 0; // L-51: the runtime tick counter is discarded on stop
        return changed();
    }

    PlayCommandResult step(std::uint64_t ticks) override
    {
        ++steps;
        if (!force_error.empty())
        {
            return refusal();
        }
        if (state_ == PlayState::edit)
        {
            return refusal(kPlayNotRunningCode);
        }
        tick_ += ticks; // stepping leaves playing/paused alone
        return changed();
    }

private:
    [[nodiscard]] PlayCommandResult changed() const
    {
        return PlayCommandResult{true, true, "", state_, tick_};
    }
    [[nodiscard]] PlayCommandResult no_op() const
    {
        return PlayCommandResult{true, false, "", state_, tick_};
    }
    [[nodiscard]] PlayCommandResult refusal(const char* code = nullptr) const
    {
        PlayCommandResult out;
        out.ok = false;
        out.error_code = code != nullptr ? std::string(code) : force_error;
        out.state = state_;
        out.sim_tick = tick_;
        return out;
    }

    PlayState state_ = PlayState::edit;
    std::uint64_t tick_ = 0;
};

} // namespace

int main()
{
    // --- the happy L-51 path, driven entirely over RPC --------------------------------------------
    {
        DaemonLikeGateway gateway;
        PlaybarModel model(&gateway);
        CHECK(model.state() == PlayState::edit);
        CHECK(!model.is_running());

        const PlayAction played = model.play();
        CHECK(played.ok);
        CHECK(played.error_code.empty());
        CHECK(model.state() == PlayState::playing);
        CHECK(gateway.plays == 1);

        const PlayAction stepped = model.step(3);
        CHECK(stepped.ok);
        CHECK(model.sim_tick() == 3); // the DAEMON's simTick, not a locally incremented one
        CHECK(model.state() == PlayState::playing);

        const PlayAction paused = model.pause();
        CHECK(paused.ok);
        CHECK(model.state() == PlayState::paused);
        CHECK(model.is_running()); // paused is still a live session (L-51)

        const PlayAction resumed = model.play();
        CHECK(resumed.ok);
        CHECK(model.state() == PlayState::playing);

        const PlayAction stopped = model.stop();
        CHECK(stopped.ok);
        CHECK(model.state() == PlayState::edit);
        CHECK(model.sim_tick() == 0); // the runtime tick counter went with the session
    }

    // --- benign no-ops: ok=false with NO error code (the ok<-changed mapping) ----------------------
    {
        DaemonLikeGateway gateway;
        PlaybarModel model(&gateway);
        CHECK(model.play().ok);

        // play while playing: the daemon answers ok=true/changed=false, which the playbar reports as
        // ok=false with no code — its `ok` has always meant "something actually happened".
        const PlayAction again = model.play();
        CHECK(!again.ok);
        CHECK(again.error_code.empty());
        CHECK(again.state == PlayState::playing);
        CHECK(model.last_error().empty()); // a no-op is not an error and must not churn last_error

        CHECK(model.stop().ok);
        const PlayAction stop_again = model.stop(); // stop in edit is idempotent
        CHECK(!stop_again.ok);
        CHECK(stop_again.error_code.empty());
    }

    // --- refusals propagate the daemon's catalog code VERBATIM ------------------------------------
    {
        DaemonLikeGateway gateway;
        PlaybarModel model(&gateway);

        const PlayAction paused = model.pause(); // in edit: nothing to pause
        CHECK(!paused.ok);
        CHECK(paused.error_code == std::string(kPlayNotRunningCode));
        CHECK(model.last_error() == std::string(kPlayNotRunningCode));
        CHECK(model.state() == PlayState::edit); // a refusal changes nothing

        const PlayAction stepped = model.step();
        CHECK(!stepped.ok);
        CHECK(stepped.error_code == std::string(kPlayNotRunningCode));

        // A transport-level failure carries whatever code the wire layer reported.
        gateway.force_error = kPlaySessionFailedCode;
        const PlayAction failed = model.play();
        CHECK(!failed.ok);
        CHECK(failed.error_code == std::string(kPlaySessionFailedCode));
        CHECK(model.state() == PlayState::edit);
    }

    // --- NO PARALLEL TRUTH: a second client's change lands with ZERO local writes ------------------
    // This is the structural half of the e08b DoD for play state. The gateway records every RPC the
    // model issued; a `play-state` fact published by ANOTHER client must move the rendered state
    // without the model issuing anything at all.
    {
        DaemonLikeGateway gateway;
        PlaybarModel model(&gateway);
        CHECK(gateway.calls() == 0);

        CHECK(model.apply_play_state(PlayState::playing, 12));
        CHECK(model.state() == PlayState::playing);
        CHECK(model.sim_tick() == 12);
        CHECK(gateway.calls() == 0); // the panel wrote NOTHING to reach the state it renders

        CHECK(model.apply_play_state(PlayState::edit, 0));
        CHECK(model.state() == PlayState::edit);
        CHECK(gateway.calls() == 0);

        // A restatement of what is already rendered changes nothing and notifies nobody (the daemon
        // never publishes one, but a `sinceSeq: 0` ring replay can deliver one twice).
        CHECK(!model.apply_play_state(PlayState::edit, 0));
    }

    // --- with NO gateway the model is render-only: it cannot invent a transition -------------------
    {
        PlaybarModel model;
        const PlayAction action = model.play();
        CHECK(!action.ok);
        CHECK(action.error_code.empty());
        CHECK(model.state() == PlayState::edit);
        CHECK(!model.pause().ok);
        CHECK(!model.stop().ok);
        CHECK(!model.step().ok);
        CHECK(model.state() == PlayState::edit);
        CHECK(model.control_generation() == 0); // nothing happened, so nothing was notified
    }

    // --- R-HUX-011: the control loop fires on every OBSERVED transition, from either door ----------
    {
        DaemonLikeGateway gateway;
        PlaybarModel model(&gateway);
        std::vector<PlayControlEvent> seen;
        model.add_control_listener([&seen](const PlayControlEvent& e) { seen.push_back(e); });

        CHECK(model.play().ok);   // "play"
        CHECK(model.pause().ok);  // "pause"
        CHECK(model.play().ok);   // "resume" — a LABEL over the same daemon transition
        CHECK(model.step(2).ok);  // "step"
        (void)model.play();       // a no-op publishes nothing, so it fires NOTHING
        CHECK(model.stop().ok);   // "stop"
        CHECK(model.apply_play_state(PlayState::playing, 4)); // another client — "session"

        CHECK(seen.size() == 6);
        if (seen.size() == 6)
        {
            CHECK(seen[0].transition == "play");
            CHECK(seen[1].transition == "pause");
            CHECK(seen[2].transition == "resume");
            CHECK(seen[3].transition == "step");
            CHECK(seen[4].transition == "stop");
            CHECK(seen[5].transition == "session");
            CHECK(seen[5].state == PlayState::playing);
            CHECK(seen[5].sim_tick == 4);
            // The generation counter is monotonic across BOTH doors.
            for (std::size_t i = 1; i < seen.size(); ++i)
            {
                CHECK(seen[i].control_generation == seen[i - 1].control_generation + 1);
            }
        }
        CHECK(model.control_generation() == 6);
    }

    // --- the L-51 state tokens (the indicator vocabulary e08a mirrors byte-for-byte) ---------------
    {
        CHECK(std::string(state_token(PlayState::edit)) == "edit");
        CHECK(std::string(state_token(PlayState::playing)) == "playing");
        CHECK(std::string(state_token(PlayState::paused)) == "paused");
    }

    PLAYBAR_TEST_MAIN_END();
}
