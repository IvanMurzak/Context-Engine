// The headless playbar (see playbar_model.h): the four transport WRITES over the PlayControlGateway
// seam, the `play-state` subscriber half, and the R-HUX-011 control loop. Since M9 e08b this file
// computes NO state transition of its own — every rendered state came from the daemon.

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
    const PlayControlEvent event{control_generation_, state_, sim_tick_, transition};
    for (const ControlListener& listener : listeners_)
    {
        if (listener)
        {
            listener(event);
        }
    }
}

PlayAction PlaybarModel::adopt(const PlayCommandResult& result, const char* transition)
{
    if (!result.ok)
    {
        // A refusal changes nothing — the daemon still holds whatever state it held. Surface its
        // catalog code verbatim rather than guessing one: each maps to a different exit class.
        last_error_ = result.error_code;
        return PlayAction{false, last_error_, state_};
    }

    last_error_.clear();
    // `ok=true, changed=false` is the daemon's benign no-op (re-play while playing, stop in edit). It
    // published nothing, so there is nothing to render and nothing to notify — and PlayAction::ok
    // reports `changed`, which is what this model's `ok` has always meant.
    if (result.changed)
    {
        state_ = result.state;
        sim_tick_ = result.sim_tick;
        notify(transition);
    }
    return PlayAction{result.changed, "", state_};
}

PlayAction PlaybarModel::play()
{
    if (gateway_ == nullptr)
    {
        return PlayAction{false, "", state_};
    }
    // "play" vs "resume" is a LABEL, not a second transition: the daemon answers one `playing` either
    // way, so the transition name is derived from what the state was BEFORE the reply landed.
    const char* transition = state_ == PlayState::paused ? "resume" : "play";
    return adopt(gateway_->play(), transition);
}

PlayAction PlaybarModel::pause()
{
    if (gateway_ == nullptr)
    {
        return PlayAction{false, "", state_};
    }
    return adopt(gateway_->pause(), "pause");
}

PlayAction PlaybarModel::stop()
{
    if (gateway_ == nullptr)
    {
        return PlayAction{false, "", state_};
    }
    return adopt(gateway_->stop(), "stop");
}

PlayAction PlaybarModel::step(std::uint64_t ticks)
{
    if (gateway_ == nullptr)
    {
        return PlayAction{false, "", state_};
    }
    return adopt(gateway_->step(ticks), "step");
}

bool PlaybarModel::apply_play_state(PlayState state, std::uint64_t sim_tick)
{
    if (state == state_ && sim_tick == sim_tick_)
    {
        return false; // the daemon restated what is already rendered — no listener churn
    }
    state_ = state;
    sim_tick_ = sim_tick;
    last_error_.clear();
    notify("session");
    return true;
}

} // namespace context::editor::gui::playbar
