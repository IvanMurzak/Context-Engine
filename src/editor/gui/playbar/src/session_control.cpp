// The real play session-control adapter: drives a runtime::session::Session and extracts the render
// frame the F1 viewport observes (see session_control.h).

#include "context/editor/gui/playbar/session_control.h"

#include "context/editor/gui/playbar/playbar_model.h" // the reserved play.* code constants

#include "context/render/extract.h" // render::extract_render_world — the L-39 sim->render extract

#include <utility>

namespace context::editor::gui::playbar
{

RuntimeSessionControl::RuntimeSessionControl(context::runtime::session::SessionConfig config)
    : config_(std::move(config))
{
}

PlayFrame RuntimeSessionControl::extract_frame() const
{
    PlayFrame frame;
    if (session_.has_value())
    {
        frame.sim_tick = session_->sim_tick();
        // L-39 / R-REND-003: a READ-ONLY extract of the session World's render-relevant state — the
        // one-way sim->render data flow the F1 viewport observes. NO second render path.
        context::render::extract_render_world(session_->world(), session_->sim_tick(), frame.snapshot);
    }
    return frame;
}

ControlOutcome RuntimeSessionControl::start()
{
    // L-51: instantiate a play session over the edit state (run_setup builds the initial world from the
    // seed in memory) — the authored files are NEVER written. The demo scenario always builds cleanly,
    // so this happy path does not emit play.session_failed (a real host whose scenario build fails
    // returns it; the fault-injecting double in the tests exercises it).
    session_.emplace(config_, /*run_setup=*/true);
    ControlOutcome out;
    out.ok = true;
    out.frame = extract_frame();
    return out;
}

ControlOutcome RuntimeSessionControl::step(std::uint64_t ticks)
{
    ControlOutcome out;
    if (!session_.has_value())
    {
        // Defensive: the model guards this with play.not_running before delegating, but the adapter
        // fails closed too (a step with no live session cannot advance anything).
        out.error_code = kPlayStepFailedCode;
        return out;
    }
    (void)session_->step(ticks); // R-SIM-002 fixed-tick advance; StepResult carried via the frame below
    out.ok = true;
    out.frame = extract_frame();
    return out;
}

void RuntimeSessionControl::discard()
{
    // L-51: the runtime session state is thrown away on stop — never persisted to authored files.
    session_.reset();
}

HotReloadOutcome RuntimeSessionControl::apply_hot_reload(const LiveEdit& edit)
{
    HotReloadOutcome out;
    if (!session_.has_value())
    {
        out.error_code = kPlayHotReloadFailedCode;
        return out;
    }

    if (edit.shape_or_layout_change)
    {
        // R-PLAY-003 / L-51: a component-schema shape / x-ctx-storage change is RESTART-CLASS — discard
        // the runtime state and re-instantiate from the new derivation (a loud session event). In-place
        // migration of live archetype storage under running systems is explicitly not attempted in v1.
        session_.reset();
        session_.emplace(config_, /*run_setup=*/true);
        out.reload_class = HotReloadClass::restart_class;
        out.state_preserved = false;
    }
    else
    {
        // L-22: a data-value edit is a LIVE-PRESERVING hot reload — the running session keeps its state;
        // the authored change re-derives into the world through the L-22 watch->hash->re-derive pipeline
        // (the CEF host wires that seam). Headless, the session is retained as-is and re-observed.
        out.reload_class = HotReloadClass::live_preserving;
        out.state_preserved = true;
    }

    out.ok = true;
    out.frame = extract_frame();
    return out;
}

} // namespace context::editor::gui::playbar
