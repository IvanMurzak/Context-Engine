// The play-in-editor SESSION-CONTROL seam (M5-F5, issue #166; R-PLAY-001/002/003, R-EDIT-001, L-51,
// L-22): the abstraction the playbar drives to start / step / stop / hot-reload a running play session
// over src/runtime/session/. Kept as a pure seam — mirroring the inspector's OverrideWriteGateway — so
// the playbar's state machine is CI-assertable WITHOUT a live daemon and its failure paths are covered
// by runtime-session fault-injection (a SessionControl double that faults), exactly as issue #166's DoD
// requires.
//
// L-51 (edit/play split): start() begins a play session over the edit-state VIEW — it never writes the
// authored files; runtime mutations live in the session's memory and are discarded by discard() on
// stop. L-22 (hot reload): apply_hot_reload() reflects a live authored edit (an F3 inspector override
// write) into the RUNNING session — a data-value edit is live-preserving (state kept), a component
// schema shape / x-ctx-storage change is restart-class (state discarded + re-instantiated, a loud
// event; R-PLAY-003). The rendered play output the seam yields IS a render::RenderSnapshot — the SAME
// type the F1 viewport observes (ViewportPanel::set_snapshot), so play output flows through the F1
// viewport with NO second render path (the DoD constraint).

#pragma once

#include "context/render/render_world.h" // render::RenderSnapshot — the observed live play frame

#include "context/runtime/session/session.h" // runtime::session::Session / SessionConfig (the driven sim)

#include <cstdint>
#include <optional>
#include <string>

namespace context::editor::gui::playbar
{

// One rendered frame the running play session produced — the snapshot the F1 viewport observes. This
// is render::RenderSnapshot itself (no wrapper render path): the L-39 extract of the session World's
// render-relevant state at `sim_tick`, ready to hand straight to viewport::ViewportPanel::set_snapshot.
struct PlayFrame
{
    std::uint64_t sim_tick = 0;
    context::render::RenderSnapshot snapshot;
};

// The outcome of a start / step control op through the seam: ok + the produced frame, or a reserved
// play.* code on a fail-closed refusal.
struct ControlOutcome
{
    bool ok = false;
    std::string error_code; // empty when ok; else a reserved play.* code (playbar_model.h)
    PlayFrame frame;        // the frame produced (start / step); empty on failure
};

// L-22 / R-PLAY-003 classification of a live authored edit reflected into a running play session.
enum class HotReloadClass
{
    live_preserving, // data-value edit / asset swap / TS logic — runtime state PRESERVED (L-22)
    restart_class,   // component-schema shape / x-ctx-storage change — state discarded + re-instantiated
                     // from the new derivation, announced by a loud session event (R-PLAY-003 / L-51)
};

// A live authored edit to reflect into the running session — an F3 inspector override write (its
// entity-relative pointer, for provenance) plus whether it changes a component's SHAPE / storage
// layout (restart-class) versus a plain data value (live-preserving). The playbar does not itself
// re-derive; it hands the classified edit to the seam, which the CEF host wires to the L-22
// watch->hash->re-derive pipeline pushing fresh derivation output through RuntimeKernel's load seam.
struct LiveEdit
{
    std::string pointer;                 // the authored field pointer the override wrote (provenance)
    bool shape_or_layout_change = false; // true => restart-class (R-PLAY-003); false => data-value
};

// The outcome of a hot reload into the running session.
struct HotReloadOutcome
{
    bool ok = false;
    std::string error_code;                             // empty when ok; else play.hot_reload_failed
    HotReloadClass reload_class = HotReloadClass::live_preserving;
    bool state_preserved = true;                        // false on a restart-class reload
    PlayFrame frame;                                    // the frame after the reload (re-observed)
};

// The control seam the playbar drives. Total (never throws); every method reports success/failure by
// value so the headless model can propagate a reserved play.* code. Implemented for real by
// RuntimeSessionControl (below) and, by the CEF host, over the live embedded RuntimeKernel; headless
// tests inject a fault-injecting double to exercise the failure codes.
class SessionControl
{
public:
    virtual ~SessionControl() = default;

    // Begin a play session over the current edit state (L-51: a session VIEW; never writes authored
    // files). Returns the first rendered frame, or play.session_failed on a fail-closed refusal.
    [[nodiscard]] virtual ControlOutcome start() = 0;

    // Advance the running session by `ticks` fixed ticks (R-SIM-002) and yield the rendered frame, or
    // play.step_failed on a refusal.
    [[nodiscard]] virtual ControlOutcome step(std::uint64_t ticks) = 0;

    // Stop: throw the runtime session state away (L-51 — discarded on stop, never persisted to files).
    virtual void discard() = 0;

    // L-22: reflect a live authored edit into the running session. A live-preserving edit keeps state;
    // a restart-class edit discards + re-instantiates (loud event). play.hot_reload_failed on refusal.
    [[nodiscard]] virtual HotReloadOutcome apply_hot_reload(const LiveEdit& edit) = 0;
};

// The real adapter: drives a context::runtime::session::Session and extracts the render frame the F1
// viewport observes (render::extract_render_world — the L-39 one-way sim->render extract, R-REND-003).
// The demo scenario always instantiates + steps cleanly, so this adapter's happy path never emits a
// failure code; the fail-closed play.* codes are the seam's contract for a real host (and are covered
// by the fault-injecting double in the tests, per the DoD's "runtime-session fault-injection").
class RuntimeSessionControl : public SessionControl
{
public:
    explicit RuntimeSessionControl(context::runtime::session::SessionConfig config);

    [[nodiscard]] ControlOutcome start() override;
    [[nodiscard]] ControlOutcome step(std::uint64_t ticks) override;
    void discard() override;
    [[nodiscard]] HotReloadOutcome apply_hot_reload(const LiveEdit& edit) override;

    // A live play session is held (playing/paused) — false in edit state (before start / after stop).
    [[nodiscard]] bool running() const noexcept { return session_.has_value(); }

private:
    // Extract the current render frame from the running session's World (empty when not running).
    [[nodiscard]] PlayFrame extract_frame() const;

    context::runtime::session::SessionConfig config_;
    std::optional<context::runtime::session::Session> session_;
};

} // namespace context::editor::gui::playbar
