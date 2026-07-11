// The headless playbar state machine (see playbar_model.h): start / pause / stop / step / hot-reload
// over the SessionControl seam, the L-51 edit/play provenance state, and the R-HUX-011 control loop.

#include "context/editor/gui/playbar/playbar_model.h"

#include <utility>

namespace context::editor::gui::playbar
{

const char* state_token(PlayState state)
{
    switch (state)
    {
    case PlayState::edit:
        return "edit";
    case PlayState::playing:
        return "playing";
    case PlayState::paused:
        return "paused";
    }
    return "edit";
}

void PlaybarModel::add_control_listener(ControlListener listener)
{
    listeners_.push_back(std::move(listener));
}

void PlaybarModel::notify(const std::string& transition)
{
    ++control_generation_;
    const PlayControlEvent event{control_generation_, state_, last_frame_.sim_tick, transition};
    for (const ControlListener& listener : listeners_)
    {
        if (listener)
        {
            listener(event);
        }
    }
}

PlayAction PlaybarModel::play()
{
    if (state_ == PlayState::playing)
    {
        // Already running — a benign no-op (not an error; nothing changed).
        return PlayAction{false, "", state_};
    }

    if (state_ == PlayState::paused)
    {
        // Resume the SAME live session (no seam start — the session is retained).
        state_ = PlayState::playing;
        last_error_.clear();
        notify("resume");
        return PlayAction{true, "", state_};
    }

    // edit -> playing: begin a play session over the edit-state view (L-51).
    if (control_ != nullptr)
    {
        const ControlOutcome outcome = control_->start();
        if (!outcome.ok)
        {
            // Start refused (fail-closed) — stay in edit; surface the seam's code (default
            // play.session_failed when the seam gave no specific code).
            last_error_ = outcome.error_code.empty() ? kPlaySessionFailedCode : outcome.error_code;
            return PlayAction{false, last_error_, state_};
        }
        last_frame_ = outcome.frame;
    }
    state_ = PlayState::playing;
    last_error_.clear();
    notify("play");
    return PlayAction{true, "", state_};
}

PlayAction PlaybarModel::pause()
{
    if (state_ == PlayState::edit)
    {
        last_error_ = kPlayNotRunningCode;
        return PlayAction{false, last_error_, state_};
    }
    if (state_ == PlayState::paused)
    {
        return PlayAction{false, "", state_}; // already paused — benign no-op
    }
    state_ = PlayState::paused;
    last_error_.clear();
    notify("pause");
    return PlayAction{true, "", state_};
}

PlayAction PlaybarModel::stop()
{
    if (state_ == PlayState::edit)
    {
        return PlayAction{true, "", state_}; // idempotent — nothing to stop
    }
    // L-51: discard the runtime session state (never persisted to authored files).
    if (control_ != nullptr)
    {
        control_->discard();
    }
    state_ = PlayState::edit;
    last_frame_ = PlayFrame{}; // the observed frame is cleared — edit state shows no live scene
    last_error_.clear();
    notify("stop");
    return PlayAction{true, "", state_};
}

PlayAction PlaybarModel::step(std::uint64_t ticks)
{
    if (state_ == PlayState::edit)
    {
        last_error_ = kPlayNotRunningCode;
        return PlayAction{false, last_error_, state_};
    }
    if (control_ != nullptr)
    {
        const ControlOutcome outcome = control_->step(ticks);
        if (!outcome.ok)
        {
            last_error_ = outcome.error_code.empty() ? kPlayStepFailedCode : outcome.error_code;
            return PlayAction{false, last_error_, state_};
        }
        last_frame_ = outcome.frame;
    }
    // Stepping does not change play/paused state (you may step while playing OR paused).
    last_error_.clear();
    notify("step");
    return PlayAction{true, "", state_};
}

HotReloadOutcome PlaybarModel::hot_reload(const LiveEdit& edit)
{
    if (state_ == PlayState::edit)
    {
        last_error_ = kPlayNotRunningCode;
        HotReloadOutcome out;
        out.error_code = kPlayNotRunningCode;
        return out;
    }
    if (control_ == nullptr)
    {
        // Pure-state model: a live-preserving no-op reload (nothing to re-derive without a seam).
        last_error_.clear();
        notify("hot-reload");
        HotReloadOutcome out;
        out.ok = true;
        return out;
    }

    HotReloadOutcome outcome = control_->apply_hot_reload(edit);
    if (!outcome.ok)
    {
        last_error_ = outcome.error_code.empty() ? kPlayHotReloadFailedCode : outcome.error_code;
        return outcome;
    }
    last_frame_ = outcome.frame;
    last_error_.clear();
    notify("hot-reload");
    return outcome;
}

} // namespace context::editor::gui::playbar
