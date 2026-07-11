// The play-in-editor PLAYBAR model (M5-F5, issue #166; R-PLAY-001/002/003/004, R-EDIT-001, L-51, L-22,
// R-HUX-011): the headless, CEF-free state machine that drives session-control over src/runtime/session/
// (start / pause / stop / step) through the SessionControl seam, tracks the L-51 edit/play provenance
// state, and holds the last rendered PlayFrame the F1 viewport observes. Observer-grade play only (the
// M5 T1 loop) — no timeline / debugger / profiler surface (later milestones).
//
// L-51 (edit/play split): `edit` is authored truth (no live session); `playing`/`paused` run a live
// runtime session over a session VIEW whose mutations are discarded on stop and never written to files.
// The panel renders a LOUD play-mode indicator (playbar_panel.h). L-22 (hot reload): hot_reload()
// reflects a live authored edit into the running session — live-preserving for a data-value edit,
// restart-class for a shape/layout change. R-HUX-011: every control transition fires a PlayControlEvent
// the CEF host times input->paint latency around (the same seam pattern as the viewport's view-update
// listener); the headless model ships the seam, the host captures the real timestamp.

#pragma once

#include "context/editor/gui/playbar/session_control.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace context::editor::gui::playbar
{

// The reserved `play.*` error-domain block (M5-F5 mints it — this leg's single code-minter). Owned
// HERE as string constants (the promote-a-local-string pattern of viewport::kViewport*Code /
// bridge::kScopeDeniedCode / ts::kTs*Code) so this GUI lib does NOT link the editor/contract catalog;
// src/editor/contract/src/error_catalog.cpp registers the SAME strings (append-only tail). USED by the
// model + the SessionControl seam and asserted by the playbar tests + the contract catalog test.
inline constexpr const char* kPlayNotRunningCode = "play.not_running";     // control issued in edit state
inline constexpr const char* kPlaySessionFailedCode = "play.session_failed"; // start refused (fail-closed)
inline constexpr const char* kPlayStepFailedCode = "play.step_failed";       // step refused (fail-closed)
inline constexpr const char* kPlayHotReloadFailedCode = "play.hot_reload_failed"; // reload refused

// The L-51 edit/play provenance state. edit = authored truth, no live session; playing = a live
// runtime session over a session view; paused = the same live session, not advancing.
enum class PlayState
{
    edit,
    playing,
    paused,
};

// The grep-stable token for a play state (mirrors the CEF host + the a11y status text).
[[nodiscard]] const char* state_token(PlayState state);

// The commands the playbar exposes — every control has a keyboard/CLI path (R-CLI-001 CLI-completeness
// as a structural accessibility property, R-A11Y-001). Bound to the focusable buttons in the panel.
inline constexpr const char* kPlayCommand = "play.play";   // start / resume
inline constexpr const char* kPauseCommand = "play.pause"; // playing -> paused
inline constexpr const char* kStopCommand = "play.stop";   // -> edit (discard runtime state, L-51)
inline constexpr const char* kStepCommand = "play.step";   // advance one fixed tick

// The R-HUX-011 play-control loop event — the human-interaction loop the playbar owns (alongside the
// viewport's gesture->update loop and the inspector's commit loop). Fired on every control transition.
// The real input->paint latency timestamp is captured at the CEF host around this seam (R-EDIT-001 /
// R-HUX-011 "instrumented timestamps in the real event path"); the headless model ships the loop seam,
// exactly like ViewportPanel's ViewportUpdate.
struct PlayControlEvent
{
    std::uint64_t control_generation = 0; // increments on each control transition
    PlayState state = PlayState::edit;    // the state AFTER the transition
    std::uint64_t sim_tick = 0;           // the running session's simTick (0 in edit)
    std::string transition;               // "play" | "resume" | "pause" | "stop" | "step" | "hot-reload"
};

// The outcome of a playbar control action: ok + the resulting state, or a reserved play.* code.
struct PlayAction
{
    bool ok = false;
    std::string error_code; // empty on ok; else a reserved play.* code
    PlayState state = PlayState::edit;
};

// The headless playbar state machine. Drives an optional SessionControl seam (nullptr => a pure-state
// model, e.g. the default the a11y harness scans). Total; every action reports success/failure by value.
class PlaybarModel
{
public:
    // The R-EDIT-001 contribution id this built-in panel registers under.
    static constexpr const char* kContributionId = "builtin.playbar";

    explicit PlaybarModel(SessionControl* control = nullptr) noexcept : control_(control) {}

    [[nodiscard]] PlayState state() const noexcept { return state_; }
    [[nodiscard]] bool is_running() const noexcept
    {
        return state_ == PlayState::playing || state_ == PlayState::paused;
    }
    [[nodiscard]] std::uint64_t sim_tick() const noexcept { return last_frame_.sim_tick; }
    // The last rendered frame — the snapshot the F1 viewport observes (ViewportPanel::set_snapshot).
    [[nodiscard]] const PlayFrame& last_frame() const noexcept { return last_frame_; }
    [[nodiscard]] std::uint64_t control_generation() const noexcept { return control_generation_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

    // Start (edit -> playing) or resume (paused -> playing). On the edit->playing transition the seam
    // begins a play session over the edit-state view (L-51; no authored-file writes). Already playing
    // is a no-op-with-error (play.not_running is not it — an in-flight play is not "not running"; the
    // action simply reports ok=false with no state change and no error code churn — see impl).
    PlayAction play();

    // playing -> paused. In edit state: play.not_running (nothing to pause).
    PlayAction pause();

    // Stop: playing/paused -> edit, discarding the runtime session state (L-51). Idempotent — stop in
    // edit state is a benign no-op (ok, stays edit), like a stopped transport bar.
    PlayAction stop();

    // Advance the running session by `ticks` fixed ticks (R-SIM-002). In edit state: play.not_running.
    // Propagates play.step_failed from the seam on a fail-closed refusal.
    PlayAction step(std::uint64_t ticks = 1);

    // L-22: reflect a live authored edit (an F3 inspector override write) into the running session.
    // In edit state: play.not_running (no session to reload into). Propagates play.hot_reload_failed.
    HotReloadOutcome hot_reload(const LiveEdit& edit);

    // Register a listener the CEF host / other panels use to react to a control transition — the seam
    // the host times input->paint latency around (R-HUX-011).
    using ControlListener = std::function<void(const PlayControlEvent&)>;
    void add_control_listener(ControlListener listener);

private:
    void notify(const std::string& transition);

    SessionControl* control_ = nullptr;
    PlayState state_ = PlayState::edit;
    PlayFrame last_frame_;
    std::uint64_t control_generation_ = 0;
    std::string last_error_;
    std::vector<ControlListener> listeners_;
};

} // namespace context::editor::gui::playbar
