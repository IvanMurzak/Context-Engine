// The Shell-mediated CROSS-WINDOW DRAG SESSION (M9 e10c, design 04 §2, 03 §1 / §7).
//
// WHAT THIS IS, AND WHY IT HAS TO EXIST. e10a made a second window a runtime primitive and e10b made
// a panel MOVE between windows over the D6 relay (window_bridge.h). Neither made a panel DRAGGABLE
// between windows: within one window Dockview's own DnD handles the gesture, but once the pointer
// LEAVES the window's bounds no single window owns the space between windows, so the SHELL must take
// the drag over. That takeover is this file: the state machine of one Shell-mediated drag, the
// cross-origin relay it queries the target window's editor-core through, and — the highest-blast-radius
// requirement in the whole task — the RAII guard that guarantees the global OS cursor capture is
// released on EVERY exit path.
//
// ─────────────────────────────────────────────────────────────────────────────────────────────────
// THE SAFETY-CRITICAL INVARIANT. A global OS cursor capture (SetCapture on Windows) routes EVERY
// pointer event in the whole desktop to one window until it is released. If a drag ends — dropped,
// Escaped, dropped on no zone, its target window closed mid-drag, its SOURCE window closed mid-drag —
// without releasing it, the entire desktop becomes unusable: no other window can be clicked, ever,
// until the process dies. So the capture is NOT a bool the session flips; it is a `ScopedCursorCapture`
// held by `std::optional` inside the session, released by its DESTRUCTOR. Every terminal path routes
// through ONE private `end()` that resets that optional, so no early return, no thrown exception, and
// no forgotten branch can leak it — the compiler runs the release for us. `capture_released()` is the
// assertion each test and the live smoke checks on every one of those paths.
// ─────────────────────────────────────────────────────────────────────────────────────────────────
//
// THE CROSS-ORIGIN ROUND TRIP (the novel seam, 04 §2). The Shell does not KNOW the target window's
// drop zones — the target window's editor-core does (it owns its own Dockview arrangement). So the
// Shell PUBLISHES the drag's hover (which panel, where, in the TARGET window's client pixels) into the
// `CrossWindowDragStore`, the TARGET window's editor-core reads it on its poll (`drag.probe`,
// window_bridge.h), hit-tests its own layout, and REPORTS its zone back (`drag.report-zone`). That is a
// genuine round trip to a DIFFERENT window's editor-core, cross-origin by construction (e08a: origin is
// per wire connection), at cursor frame rate. If the target window closes mid-drag its poll simply
// stops arriving and `on_window_closed` ends the session — the query never deadlocks and never
// dereferences a dead window.
//
// CEF-FREE and D10 BOUNDARY-CLEAN, exactly like window_bridge.h / ipc_bridge.h: the session runs
// nowhere the local dev gate cannot reach (the OS cursor capture is behind the `IGlobalCursorCapture`
// seam, the window resolution behind injected callbacks), so tests/test_cross_window_drag.cpp drives
// the SAME state machine the real Shell runs, on all three default `build` legs. Nothing here touches a
// kernel-internal module (the panel seed's state blob is an OPAQUE `contract::Json`, never interpreted).

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/shell/dpi.h"
#include "context/editor/shell/window_bridge.h" // PanelSeed
#include "context/editor/shell/window_registry.h" // WindowId

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace context::editor::shell
{

// ------------------------------------------------------------------ the global OS cursor-capture seam
//
// The ONE OS call the drag session makes (SetCapture / ReleaseCapture on Windows). Behind an interface
// for the reason browser.h / window.h state: the real capture is Windows-only and un-runnable on the
// Session-0 CI runner, so anything written inside it would be exercised by nothing local. `release`
// is IDEMPOTENT — releasing an unheld capture is a no-op, never an error — because the safety guard
// above may release more than once (an explicit early release plus the destructor) and every one of
// those must be harmless.
class IGlobalCursorCapture
{
public:
    virtual ~IGlobalCursorCapture() = default;

    // Take the global cursor capture. Returns false when it could not be taken (no OS window on a
    // headless box, an OS refusal) — the session then never begins rather than running captureless.
    [[nodiscard]] virtual bool capture() = 0;

    // Release the capture. Idempotent: a no-op when nothing is held.
    virtual void release() = 0;

    [[nodiscard]] virtual bool captured() const = 0;
};

// A cursor capture with no OS behind it: it records capture/release and reports its state, nothing
// more. This is what makes the Session-0-safe smoke and the unit tests drive the REAL session — the
// same way HeadlessWindowBackend is the honest offscreen window, not a pejorative "mock". A test
// asserts against `captures()` / `releases()` that the session took and RELEASED the capture on each
// path; the smoke uses it because a real global capture cannot be driven on a Session-0 runner at all.
class HeadlessCursorCapture final : public IGlobalCursorCapture
{
public:
    [[nodiscard]] bool capture() override
    {
        ++captures_;
        held_ = true;
        return true;
    }
    void release() override
    {
        if (held_)
        {
            ++releases_;
            held_ = false;
        }
    }
    [[nodiscard]] bool captured() const override { return held_; }

    [[nodiscard]] std::uint64_t captures() const { return captures_; }
    [[nodiscard]] std::uint64_t releases() const { return releases_; }

private:
    std::uint64_t captures_ = 0;
    std::uint64_t releases_ = 0;
    bool held_ = false;
};

// A cursor capture that CANNOT be taken — `capture()` always fails. The honest report of a box with no
// interactive desktop asked to begin a global drag; the session must refuse to begin rather than run a
// captureless drag that never routes the desktop's pointer events. Exists so the `no_capture` path is
// testable without an OS.
class UnavailableCursorCapture final : public IGlobalCursorCapture
{
public:
    [[nodiscard]] bool capture() override { return false; }
    void release() override {}
    [[nodiscard]] bool captured() const override { return false; }
};

// The RAII scope-guard. Captures on construction (via a live `IGlobalCursorCapture&`) and releases in
// its destructor — THE structural guarantee that no early-return or exception can leak the capture.
// Move-only (a capture has one owner at a time); a moved-from guard holds nothing and releases nothing.
// Non-owning: it references the app's capture object, which outlives every drag.
class ScopedCursorCapture
{
public:
    ScopedCursorCapture() = default; // holds nothing
    // Takes the capture immediately. `holds()` is false afterwards iff the capture could not be taken —
    // the caller checks and refuses to begin the drag.
    explicit ScopedCursorCapture(IGlobalCursorCapture& capture);
    ~ScopedCursorCapture();

    ScopedCursorCapture(ScopedCursorCapture&& other) noexcept;
    ScopedCursorCapture& operator=(ScopedCursorCapture&& other) noexcept;
    ScopedCursorCapture(const ScopedCursorCapture&) = delete;
    ScopedCursorCapture& operator=(const ScopedCursorCapture&) = delete;

    // Release the capture NOW (idempotent). The destructor also releases, so an early release is safe.
    void release();

    // True when this guard took (and has not yet released) a capture.
    [[nodiscard]] bool holds() const { return capture_ != nullptr && took_; }

private:
    IGlobalCursorCapture* capture_ = nullptr;
    bool took_ = false;
};

// ------------------------------------------------------------------------- the cross-window relay
//
// The Shell's hover for the window under the cursor: which panel is being dragged, where the cursor is
// in that window's CLIENT pixels, and a monotone generation the target echoes back so a stale zone
// answer (from a previous target) is ignored. `active` is false when the drag is over no live window.
struct DragHover
{
    bool active = false;
    WindowId target = kInvalidWindowId;
    std::string panel_id;
    PointI local; // cursor in the TARGET window's client pixels
    std::uint64_t generation = 0;
};

// The target window's editor-core's answer: which drop zone the cursor is over, if any. `zone_id` is
// cosmetic — the highlight label — and `generation` is the hover it answers, so the Shell can drop a
// stale report from a target the cursor has already left.
struct DragZone
{
    bool valid = false;
    std::string zone_id;
    std::uint64_t generation = 0;
};

// The shared cross-window drag relay. ONE per app (referenced by every window's `WindowBridge`),
// because the drag's SOURCE and its TARGET are two DIFFERENT windows and the answer has to cross
// between them. Pure data movement, CEF-free, fully unit-testable: no browser, no window, no bridge.
//
// The two directions:
//   * Shell -> target : `publish_hover` sets the current hover; the target window's editor-core reads
//     it with `hover_for(self_id)`, which answers `active` ONLY to the window the hover targets — every
//     other window sees `active:false` and does nothing (so a drag over window 1 never makes window 0's
//     editor-core report a zone).
//   * target -> Shell : `report_zone` records the target's answer, but ONLY when its generation matches
//     the CURRENT hover — a report for a hover the cursor has already moved past is dropped.
class CrossWindowDragStore
{
public:
    // Shell publishes the hover (a new generation each cursor frame). Clears any prior zone: a fresh
    // hover has not been answered yet, so the last target's zone must not leak into this frame.
    void publish_hover(const DragHover& hover);
    // The drag ended / found no live target: no window is being hovered.
    void clear_hover();

    // The hover THIS window should act on: the live hover iff it targets `reader`, else an inactive one.
    [[nodiscard]] DragHover hover_for(WindowId reader) const;
    [[nodiscard]] const DragHover& hover() const { return hover_; }

    // The target reports its zone. Ignored unless `zone.generation` equals the current hover's — a late
    // answer for a superseded hover is stale.
    void report_zone(const DragZone& zone);
    [[nodiscard]] const DragZone& zone() const { return zone_; }

    [[nodiscard]] std::uint64_t hovers_published() const { return hovers_published_; }
    [[nodiscard]] std::uint64_t zones_reported() const { return zones_reported_; }

private:
    DragHover hover_;
    DragZone zone_;
    std::uint64_t hovers_published_ = 0;
    std::uint64_t zones_reported_ = 0;
};

// --------------------------------------------------------------------------------- the drag session

// Why a drag ended. Every value except `none` is a TERMINAL state that has released the capture. The
// cancel-class values (`escaped`, `dropped_no_zone`, `target_closed`, `source_closed`) are the four the
// DoD names explicitly; `no_capture` is the drag that never began because the OS capture could not be
// taken.
enum class DragEndReason
{
    none,            // still active (or never begun)
    dropped,         // dropped on a VALID zone -> rehomed through e10b's move path
    dropped_no_zone, // dropped where there was no valid zone -> the panel stays home
    escaped,         // Escape (or an explicit cancel)
    target_closed,   // the window under the cursor died mid-drag
    source_closed,   // the drag's own source window died mid-drag
    no_capture,      // the OS cursor capture could not be taken -> the session never began
};

[[nodiscard]] const char* to_string(DragEndReason reason);

// The Shell-mediated drag of one panel between windows. Non-copyable and non-movable: it holds a
// reference to the shared store and a capture guard whose lifetime IS the release contract, and nothing
// may relocate either out from under a live drag.
class CrossWindowDragSession
{
public:
    // Resolve the WindowId whose bounds contain a SCREEN point (from e10a's registry). Returns
    // `kInvalidWindowId` when the cursor is over no editor window (the desktop, another app).
    using WindowAtPoint = std::function<WindowId(PointI screen)>;
    // Convert a SCREEN point into `target`'s CLIENT-local pixels. Injected so the session stays OS-free:
    // only the app knows each window's screen rect.
    using ToLocal = std::function<PointI(WindowId target, PointI screen)>;
    // Carry out the drop: rehome `seed` INTO `target` through e10b's EXISTING move path
    // (`window.move-to` -> `PanelHost.open` + `panel.state.set`). Returns whether the rehome was
    // accepted. Deliberately a callback, NOT a second recreate mechanism — D6's one-mechanism rule is
    // the whole reason this hands off rather than re-implementing recreate (a third path would be a
    // REPORTABLE finding, not something to add here).
    using DropHandler = std::function<bool(WindowId target, const PanelSeed& seed)>;

    explicit CrossWindowDragSession(CrossWindowDragStore& store);
    // Belt-and-braces: if a session is destroyed while somehow still active, the capture guard's own
    // destructor still releases — the invariant holds even past a forgotten `end()`.
    ~CrossWindowDragSession() = default;

    CrossWindowDragSession(const CrossWindowDragSession&) = delete;
    CrossWindowDragSession& operator=(const CrossWindowDragSession&) = delete;
    CrossWindowDragSession(CrossWindowDragSession&&) = delete;
    CrossWindowDragSession& operator=(CrossWindowDragSession&&) = delete;

    void bind_window_at_point(WindowAtPoint resolver);
    void bind_to_local(ToLocal to_local);
    void bind_drop(DropHandler handler);

    // BEGIN the drag: the pointer left `source`'s bounds carrying `seed`. Takes the OS cursor capture
    // via `capture` (which MUST outlive the session — it is the app's). Returns false and ends
    // IMMEDIATELY with `no_capture` when the capture could not be taken: there is never a half-begun,
    // captureless session. `screen` seeds the ghost at the pointer.
    [[nodiscard]] bool begin(WindowId source, PanelSeed seed, PointI screen,
                             IGlobalCursorCapture& capture);

    // UPDATE: the global cursor moved to `screen`. Moves the ghost, resolves the window under the
    // cursor, and — when that is a live window OTHER than none — publishes the hover so THAT window's
    // editor-core answers its zone over the bridge. Over no window (the desktop, or a window that just
    // died) it publishes an inactive hover: the drag stays live with no valid target until the cursor
    // finds a window or a terminal event fires. A no-op when the session is not active.
    void update_cursor(PointI screen);

    // Refresh the cached drop zone from the store (the target's editor-core reported it back over
    // `drag.report-zone`). A no-op when inactive. Called before `drop()` so the drop decision sees the
    // latest cross-window answer.
    void sync_zone();

    // DROP the panel. Rehomes through the drop handler when the CURRENT target answered a VALID zone for
    // the CURRENT hover; otherwise cancels as `dropped_no_zone` (the panel stays in its source window).
    // Releases the capture on BOTH branches. Returns the terminal reason.
    DragEndReason drop();

    // Cancel the drag (Escape, or an explicit abort). Releases the capture. A no-op when inactive.
    void cancel();

    // A window died mid-drag. Ends the session — releasing the capture — when it is the TARGET
    // (`target_closed`; the query never dereferences the dead window) or the SOURCE (`source_closed`).
    // An unrelated window closing is ignored. This is the DoD's "target/source window closes mid-drag"
    // path, and the CE #319-doubled lifetime hazard: the session drops its reference to the dead window
    // rather than reading through it.
    void on_window_closed(WindowId id);

    // --- state / assertion surface ---------------------------------------------------------------
    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] WindowId source() const { return source_; }
    // The window the cursor is currently over (`kInvalidWindowId` when over none). The cross-window
    // drop-zone query is directed here.
    [[nodiscard]] WindowId target() const { return target_; }
    // The Shell-owned drag ghost's screen position — it follows the global cursor (the actual
    // compositing is Shell-side; this is the position it is drawn at).
    [[nodiscard]] PointI ghost_position() const { return ghost_; }
    [[nodiscard]] const DragZone& zone() const { return zone_; }
    [[nodiscard]] DragEndReason end_reason() const { return end_reason_; }
    // THE SAFETY-CRITICAL assertion: the drag has ended AND the OS cursor capture is down. Checked on
    // EVERY terminal path by the unit tests and the live smoke — a leaked capture makes the whole
    // desktop unusable, so this is the one property that must hold no matter how the drag ended.
    [[nodiscard]] bool capture_released() const;

private:
    // THE ONE terminal path. Resets the capture guard (its destructor releases the OS capture), clears
    // the store's hover, drops the target reference, marks the session inactive, and records the reason.
    // EVERY end — drop, cancel, a closed window, a failed begin — routes through here, which is what
    // makes "released on every path" a single auditable line rather than a promise spread across
    // branches.
    void end(DragEndReason reason);

    CrossWindowDragStore& store_;
    WindowAtPoint window_at_point_;
    ToLocal to_local_;
    DropHandler drop_;

    // The RAII capture. `reset()` releases; it is also released by ~CrossWindowDragSession if a drag is
    // still live at destruction. The non-owning `capture_obj_` is kept only so `capture_released()` can
    // read the REAL OS-capture state after the guard let go.
    std::optional<ScopedCursorCapture> capture_guard_;
    IGlobalCursorCapture* capture_obj_ = nullptr;

    PanelSeed seed_;
    WindowId source_ = kInvalidWindowId;
    WindowId target_ = kInvalidWindowId;
    PointI ghost_;
    DragZone zone_;
    std::uint64_t hover_generation_ = 0;
    bool active_ = false;
    DragEndReason end_reason_ = DragEndReason::none;
};

} // namespace context::editor::shell
