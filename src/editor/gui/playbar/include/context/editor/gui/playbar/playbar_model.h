// The play-in-editor PLAYBAR model (M5-F5, issue #166; R-PLAY-001/002/004, R-EDIT-001, L-51,
// R-HUX-011): the headless, CEF-free transport that RENDERS the L-51 edit/play provenance state and
// drives it. Observer-grade play only (the M5 T1 loop) — no timeline / debugger / profiler surface.
//
// M9 e08b — THE PLAY STATE IS THE DAEMON'S, NOT THIS MODEL'S. Until e08a the model drove an
// IN-PROCESS `SessionControl*` and owned the resulting state in a private member; the daemon now owns
// it (design 05 §4 / D7 tier 1, docs/editor-session-state.md), so this model became a WRITER and a
// SUBSCRIBER and the in-process path is GONE, not kept as a parallel truth:
//
//   * `play/pause/stop/step` are RPC write requests through the PlayControlGateway seam below
//     (`editor.play|pause|stop|step`). The daemon answers with the resulting state, which is what
//     this model then renders — it never computes a transition of its own.
//   * `apply_play_state()` adopts a `play-state` fact published by ANOTHER client, so a CLI or agent
//     driving play is visible on the L-51 indicator with no local write at all. e08a's `origin` echo
//     suppression happens one layer up, so each real change lands exactly once.
//
// e08a deliberately mirrored THIS state machine token-for-token, so the rewire is a semantic no-op.
// The one refinement is at the reply boundary: an R-CLI-008 envelope cannot express "ok=false with no
// error code" (a failure must carry a catalog code), so a benign daemon no-op answers
// `ok=true, changed=false` and this model's `PlayAction::ok` is fed from `changed` — losslessly, since
// `ok` here always meant "something actually happened".
//
// WHAT LEFT WITH THE IN-PROCESS PATH. `PlayFrame` observation and L-22 `hot_reload()` were both
// SessionControl operations, not play-state transitions, and the daemon has no verb for either yet.
// They stay on the SessionControl seam itself (session_control.h — still built, still tested,
// runtime-side), rather than being kept alive here behind a `SessionControl*` that would reintroduce
// exactly the second truth this task removes. Re-homing them onto the wire is later work (e09+).
//
// R-HUX-011: every observed transition still fires a PlayControlEvent the CEF host times input->paint
// latency around (the same seam pattern as the viewport's view-update listener); the headless model
// ships the seam, the host captures the real timestamp.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace context::editor::gui::playbar
{

// The reserved `play.*` error-domain block (M5-F5 mints it — this leg's single code-minter). Owned
// HERE as string constants (the promote-a-local-string pattern of viewport::kViewport*Code /
// bridge::kScopeDeniedCode / ts::kTs*Code) so this GUI lib does NOT link the editor/contract catalog;
// src/editor/contract/src/error_catalog.cpp registers the SAME strings (append-only tail). Since e08b
// the DAEMON is what emits them on a refused `editor.play|pause|stop|step` (its EditorSessionState
// reuses this exact block, docs/editor-session-state.md); this model propagates whatever code it is
// handed, and the SessionControl seam still mints them runtime-side. Asserted by the playbar tests,
// the contract catalog test, and the e08a/e08b T2 drills.
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
// `ok` is fed from the daemon's `changed` (see the header note): true iff something actually moved.
struct PlayAction
{
    bool ok = false;
    std::string error_code; // empty on ok; else a reserved play.* code
    PlayState state = PlayState::edit;
};

// One `editor.play|pause|stop|step` reply, member-for-member the daemon's answer (kernel_server.cpp:
// `{state, simTick, changed}` on success, a reserved play.* catalog code on a refusal).
struct PlayCommandResult
{
    bool ok = false;          // the daemon accepted the command (false => error_code is set)
    bool changed = false;     // false + ok => a benign, idempotent no-op; nothing was published
    std::string error_code;   // empty when ok; else a reserved play.* code
    PlayState state = PlayState::edit; // the state AFTER the command, as the DAEMON reports it
    std::uint64_t sim_tick = 0;
};

// The seam the playbar WRITES play control through (M9 e08b) — the sibling of the scene tree's
// SelectionGateway, and declared here for the same reason: this library stays boundary-clean (no
// client SDK, no RPC), so the transport is CI-assertable with a recording double and the real
// implementation is a WIRE gateway shell-side. Total; every method reports by value.
class PlayControlGateway
{
public:
    virtual ~PlayControlGateway() = default;

    [[nodiscard]] virtual PlayCommandResult play() = 0;
    [[nodiscard]] virtual PlayCommandResult pause() = 0;
    [[nodiscard]] virtual PlayCommandResult stop() = 0;
    [[nodiscard]] virtual PlayCommandResult step(std::uint64_t ticks) = 0;
};

// The headless playbar. Renders the daemon's L-51 play state and drives it through an optional
// PlayControlGateway (nullptr => a render-only model, e.g. the default the a11y harness scans).
// Total; every action reports success/failure by value.
class PlaybarModel
{
public:
    // The R-EDIT-001 contribution id this built-in panel registers under.
    static constexpr const char* kContributionId = "builtin.playbar";

    // Non-owning; the gateway must outlive the model.
    explicit PlaybarModel(PlayControlGateway* gateway = nullptr) noexcept : gateway_(gateway) {}

    // The RENDERED play state — the daemon's answer, not a locally computed transition.
    [[nodiscard]] PlayState state() const noexcept { return state_; }
    [[nodiscard]] bool is_running() const noexcept
    {
        return state_ == PlayState::playing || state_ == PlayState::paused;
    }
    // The running session's simTick, as the daemon reports it.
    [[nodiscard]] std::uint64_t sim_tick() const noexcept { return sim_tick_; }
    [[nodiscard]] std::uint64_t control_generation() const noexcept { return control_generation_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

    // --- the four transport WRITES (`editor.play|pause|stop|step`) -------------------------------
    //
    // Each issues the RPC and then renders what the DAEMON answered. With no gateway bound nothing is
    // issued and nothing changes (ok=false, no code) — a transport bar with no daemon has nothing to
    // drive, and inventing a local transition would be exactly the parallel truth e08b removes.
    //
    // Start (edit|paused -> playing). Already playing is a benign no-op (ok=false, no code).
    PlayAction play();
    // playing -> paused. In edit state the daemon refuses play.not_running; already paused is a no-op.
    PlayAction pause();
    // playing|paused -> edit, discarding the runtime session state (L-51). Idempotent in edit.
    PlayAction stop();
    // Advance `ticks` fixed ticks (R-SIM-002). In edit state: play.not_running. Stepping leaves
    // playing/paused alone (you may step from either).
    PlayAction step(std::uint64_t ticks = 1);

    // --- the daemon's fact (the SUBSCRIBER half) -------------------------------------------------
    //
    // Adopt a `play-state` fact published by another client (a CLI, an agent, a second window). This
    // is what makes the L-51 indicator daemon-fed rather than self-fed. Notifies the R-HUX-011
    // listeners and returns true only when the rendered state actually changed.
    bool apply_play_state(PlayState state, std::uint64_t sim_tick);

    // Register a listener the CEF host / other panels use to react to a control transition — the seam
    // the host times input->paint latency around (R-HUX-011).
    using ControlListener = std::function<void(const PlayControlEvent&)>;
    void add_control_listener(ControlListener listener);

private:
    void notify(const std::string& transition);
    // The ONE place a command reply becomes rendered state (all four transports share it).
    PlayAction adopt(const PlayCommandResult& result, const char* transition);

    PlayControlGateway* gateway_ = nullptr;
    // The RENDERED state: what the daemon last said, never what this model decided.
    PlayState state_ = PlayState::edit;
    std::uint64_t sim_tick_ = 0;
    std::uint64_t control_generation_ = 0;
    std::string last_error_;
    std::vector<ControlListener> listeners_;
};

} // namespace context::editor::gui::playbar
