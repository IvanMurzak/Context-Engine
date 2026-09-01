// The browser seam (design 03 §1, §4) — what the Shell needs from an OSR browser, with no CEF in it.
//
// CEF is a CI-only dependency path (the MSVC/Clang-ABI prebuilt cannot link under the local
// Strawberry-GCC dev gate), so a compositor written against `CefBrowserHost` directly would be
// exercised by nothing that runs locally and by one CI job remotely. Everything above this interface
// — the layer stack, the damage tracking, the resize protocol, the PET_POPUP layer, the input
// dispatch — is therefore CEF-free and unit-tested on all three OS legs of the default `build`
// matrix. `shell_cef_host.cpp` implements this interface over the real browser behind
// CONTEXT_BUILD_GUI_CEF.
//
// The frame vocabulary is e03's `OsrFrame` unchanged: it already carries exactly what CEF's OnPaint
// delivers (premultiplied BGRA8 pixels, coded_size, visible_rect, dirty rects) and what the import
// driver consumes. A second, shell-local frame struct would be a translation layer whose only job is
// to be kept in sync.

#pragma once

#include "context/editor/shell/dpi.h"
#include "context/editor/shell/input.h"
#include "context/editor/shell/osr_drag.h"
#include "context/render/present/osr_import.h"
#include "context/render/rhi.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::shell
{

// Which OSR layer a frame belongs to. CEF reports these as PET_VIEW and PET_POPUP on the same
// OnPaint callback, and they are genuinely different layers: the popup is composited SECOND, over
// the view, confined to the popup rect (03 §4).
enum class BrowserLayer
{
    view,
    popup,
};

struct BrowserFrame
{
    BrowserLayer layer = BrowserLayer::view;
    render::present::OsrFrame frame;
};

// What the compositor implements so a browser host can push into it.
class IBrowserFrameSink
{
public:
    virtual ~IBrowserFrameSink() = default;

    // A new OSR frame for one layer.
    virtual void on_browser_frame(const BrowserFrame& frame) = 0;

    // The popup opened/closed, and where it is. CEF sends the RECT (OnPopupSize) and the
    // VISIBILITY (OnPopupShow) as separate callbacks and does not guarantee an order, so the sink
    // gets both halves as one call and keeps no partial state of its own.
    //
    // `rect` IS IN DIP — view coordinates, exactly as `OnPopupSize` reports it, and the ONE rect on
    // this seam that is not already physical pixels (a2). The SINK converts, because the conversion
    // needs a scale and the compositor is the one holder of that scale (`WindowCompositor::dpi_`);
    // converting in the CEF binding instead would put the arithmetic in a TU no local gate compiles
    // and no headless CI job executes. The popup's own OnPaint TEXTURE is physical like every other
    // frame — that split between the rect and the texture is the whole of the bug a2 fixed.
    //
    // A hidden popup MUST drop its layer rather than merely stop drawing it: CEF reuses the popup
    // texture for the next dropdown at a different size, and a retained stale layer would composite
    // the previous menu's pixels for the frame between the hide and the next paint.
    virtual void on_popup_state(bool visible, const render::Rect2D& rect) = 0;
};

// What the SHELL implements so an OSR browser can report the two drag callbacks up (b1, D11 —
// osr_drag.h for the protocol, docs/shell.md § 16 for the audit rows this closes).
//
// SEPARATE FROM `IBrowserFrameSink` on purpose. The frame sink is the COMPOSITOR — it consumes
// pixels and popup geometry and knows nothing about input. A drag is an INPUT gesture the WINDOW
// owns: the window is what has the pointer stream, the view rect to hit-test against, and the
// `OsrDragSession` whose injections go back through `IBrowserHost::inject_drag`. Routing
// `StartDragging` through the compositor would put the one decision in the one object with no way
// to make it.
//
// Bound ONCE (`set_drag_observer`) rather than passed per `pump()` like the sink, because
// `StartDragging` fires from inside CEF's own pump, with no argument of ours to carry it in.
class IBrowserDragObserver
{
public:
    virtual ~IBrowserDragObserver() = default;

    // `CefRenderHandler::StartDragging` (cef_render_handler.h:208). `allowed` is the operations
    // mask the renderer will accept; `start_view_dip` is the header's SCREEN start point ALREADY
    // converted to view DIP by the binding (`osr_view_point`, dpi.h) — the conversion happens where
    // the platform's screen convention is known, exactly as `GetScreenPoint` does.
    //
    // RETURNS WHAT `StartDragging` RETURNS: true to drive the drag, false to ABORT it. The
    // unimplemented default returned false, and the header defines that as "abort the drag
    // operation" — which is why every HTML5 drag in the editor was dead rather than merely
    // unhandled.
    [[nodiscard]] virtual bool on_start_dragging(DragOperationMask allowed,
                                                 PointI start_view_dip) = 0;

    // `CefRenderHandler::UpdateDragCursor` (cef_render_handler.h:222): the operation the view would
    // perform at the current position — the drag feedback, and what `DragSourceEndedAt` reports as
    // the operation a drop actually performed.
    virtual void on_update_drag_cursor(DragOperation operation) = 0;
};

// One OSR browser bound to one window. Not thread-safe: driven from the single shell-owned pump
// (03 §1 — `multi_threaded_message_loop=false`, the single-threaded owner loop).
class IBrowserHost
{
public:
    virtual ~IBrowserHost() = default;

    [[nodiscard]] virtual const char* name() const = 0;

    // The browser's view size in LOGICAL (DIP) pixels plus the monitor scale. This is CEF's
    // `GetViewRect` + `GetScreenInfo::device_scale_factor` pair: reporting a physical size here
    // would lay the document out at the wrong size on any non-100% monitor.
    virtual void resize(render::Extent2D logical_size, DpiScale dpi) = 0;

    // WHERE that view sits on screen (a1): the window's CLIENT origin, in the platform's own screen
    // convention — device pixels on Windows/Linux, DIP on macOS (`IWindowBackend::client_origin`,
    // and the same predicate `osr_screen_point` takes in dpi.h).
    //
    // The second half of the OSR geometry contract, and separate from `resize()` on purpose: a
    // window that MOVES has not resized, and routing the origin through `resize()` would drive
    // CEF's `WasResized()` — a full re-layout and repaint — on every step of a window drag. An
    // off-screen browser cannot ask the OS where it is, so without this it answers CEF's
    // `GetScreenPoint` with view coordinates and every native menu opens at the wrong place
    // (docs/shell.md § 16).
    //
    // PURE, like `resize`: a default that quietly dropped the origin would be the bug itself.
    virtual void set_client_origin(PointI origin) = 0;

    virtual void send_pointer(const PointerDispatch& dispatch, const PointerEvent& event) = 0;
    virtual void send_key(const KeyEvent& event) = 0;
    virtual void set_focus(bool focused) = 0;

    // --- b1: the OSR drag protocol (D11; osr_drag.h) ---------------------------------------------

    // Bind (or unbind, with nullptr) the observer this browser reports `StartDragging` /
    // `UpdateDragCursor` to. The observer must outlive the binding; `EditorWindow` satisfies that by
    // construction — the observer is one of its own members and the host is its `browser_` member,
    // so nothing can pump the browser between the two destructions.
    virtual void set_drag_observer(IBrowserDragObserver* observer) = 0;

    // Apply ONE step of the protocol — the six windowless-only `CefBrowserHost` drag members
    // (`cef_browser.h:897-951`), as the value `OsrDragSession` emits. ONE method rather than six,
    // because the session has ALREADY decided which member and in what order: six entry points
    // would let a caller re-decide that, which is exactly the judgement this design keeps out of
    // the CEF translation unit.
    //
    // PURE, like `resize` / `set_client_origin` and for the identical reason: a defaulted no-op is
    // not a neutral fallback here, it IS the bug — a host that silently swallowed the injections
    // would leave `StartDragging` returning true and the renderer stuck mid-drag forever, which is
    // strictly worse than the honest refusal b1 replaces.
    virtual void inject_drag(const OsrDragInjection& injection) = 0;

    // Drive one slice of the browser's work and deliver whatever frames it produced into `sink`.
    // Returns false once the browser is gone. For the CEF host this is where CefDoMessageLoopWork
    // runs — the integrated pump (03 §1).
    //
    // ⚠ THE HOST MAY RETAIN `sink` BEYOND THIS CALL, so the caller must keep it alive until
    // `close()` (which unbinds it). The reference is NOT scoped to the call, and with N windows it
    // cannot be: the CEF host's pump drives a PROCESS-WIDE message loop that dispatches the pending
    // paints of EVERY browser in the process, so a sink bound only while its own host is pumping
    // misses every frame the loop happens to deliver during a SIBLING window's pump — which, with
    // the owner loop pumping window 0 first each tick, is very nearly all of them (see
    // `cef_shell.h` § `frames_dropped_without_sink`). `EditorWindow` satisfies the requirement by
    // construction: the sink is its own compositor member and the host is its browser member.
    virtual bool pump(IBrowserFrameSink& sink) = 0;

    // Run a fragment of JavaScript in the browser's MAIN FRAME. Added by e10a for one reason: the
    // `OnBeforePopup` suppression (03 §1) is a security containment boundary, and the only honest
    // proof of it is a REAL `window.open` issued by REAL renderer content — a unit-level stub would
    // assert that the handler we wrote returns true, which is not the same claim at all.
    //
    // PURE, like every other seam here: each host states its own answer. The CEF host runs
    // `CefFrame::ExecuteJavaScript`; the scripted host records the source (it has no JS engine, and
    // pretending otherwise would let a caller believe a script ran).
    virtual void execute_script(std::string_view source) = 0;

    virtual void close() = 0;

    // --- teardown, split into two phases so N browsers tear down SAFELY ---------------------------
    //
    // `close()` above does the whole thing at once: unbind the sink, ask CEF to close, AND drive the
    // process-wide message loop until this browser is done. That is correct for ONE browser (the app's
    // single window, the sibling single-window smokes, a host that simply goes out of scope). It is
    // NOT correct for N: `CefDoMessageLoopWork()` is process-wide, so one browser's close-drain
    // advances ANOTHER still-open browser's teardown, and on Windows that reaches a CEF ref-counted
    // object's final Release INSIDE its own destructor — the `!in_dtor_` abort (CE #319 generalised to
    // N windows tearing down at once). The WindowManager therefore drives the three phases below
    // itself: ask EVERY window to close first, then ONE shared drain, then release the clients — so no
    // window's teardown pump can re-enter another window's final destruction.
    //
    // The defaults route to `close()` / report "already closed" / no-op, so a host with no async
    // teardown (the scripted host, the unit fakes) satisfies the interface unchanged.

    // Phase 1: unbind the frame sink and ask CEF to close this browser, WITHOUT pumping the loop.
    // Idempotent. Default: the synchronous `close()`.
    virtual void request_close() { close(); }

    // Has the browser finished acknowledging the close (its `OnBeforeClose` has run), so its client
    // may be released? A host with no async teardown is closed the moment it is asked. Default: true.
    [[nodiscard]] virtual bool is_closed() const { return true; }

    // Phase 2: drive ONE slice of the shared teardown message loop, delivering no frames. Process-wide
    // for the CEF host (it drains EVERY closing browser at once); a no-op for a host with no message
    // loop. Called in a single drain loop after phase 1 has requested close on every window.
    virtual void pump_teardown() {}

    // Retire this browser MID-PROCESS: unbind its frame sink so it stops painting into a compositor
    // that is going away, but do NOT ask CEF to close it — its CEF teardown is DEFERRED to the shared,
    // all-closing `shutdown()` drain. This is the e10a `!in_dtor_` fix (CE #319 generalised): closing +
    // draining a SINGLE browser mid-process, while sibling browsers are still live in the same
    // process-wide message loop, drives `CefDoMessageLoopWork()` through the closing browser's teardown
    // interleaved with the live siblings' work, and on Windows that re-enters a libcef-internal
    // ref-counted object's own destructor (`Release()` with `in_dtor_` set). By keeping the browser
    // OPEN until every browser is closing together, the interleaving teardown is unreachable. The host
    // then outlives `shell::cef::shutdown()` in the registry's graveyard (window_registry.h § LIFETIME
    // RULE). Default: no-op — a host with no async teardown / no live sink has nothing to unbind.
    virtual void detach() {}
};

// ------------------------------------------------------------------- the integrated pump schedule

// When to run a pump slice (03 §1) — the portable half of the integrated message pump.
//
// With external_message_pump on, CEF does not own a loop: it calls OnScheduleMessagePumpWork("pump
// me in delay_ms") and expects the embedder's own thread to comply. Two things make that policy
// belong HERE rather than inside the CEF binding:
//
//  - It is a pure function of (scheduled, due, now). The binding is the one translation unit the
//    local gate cannot build, so a scheduler living there is exercised by nothing that runs locally
//    — yet this is precisely the mechanism the design cites when it rejects the spike's
//    multi-threaded+mutex model, so it is the last thing that should go unverified.
//  - CEF documents OnScheduleMessagePumpWork as callable from ANY thread, while the owner thread
//    concurrently asks whether to pump. That is a real cross-thread handoff of these two scalars,
//    so they are atomic — plain members would be a data race (UB), and a torn `due` could park the
//    browser until the floor below happened to fire.
class PumpSchedule
{
public:
    // CEF asked to be pumped `delay_ms` from `now_ms`. A negative delay means "as soon as possible".
    // Callable from any thread.
    void schedule(std::int64_t delay_ms, std::int64_t now_ms);

    // Should the owner thread run a pump slice at `now_ms`? True when scheduled work has come due,
    // and — the unconditional FLOOR — also when nothing is scheduled at all, so a schedule callback
    // that never arrives (or one dropped by the benign race below) cannot stall the browser. A due
    // schedule is consumed. Call only from the owner thread.
    //
    // The race is deliberately left benign rather than locked: a schedule() landing between the due
    // check and the consume can be dropped, after which `scheduled` reads false and the very next
    // call pumps via the floor. Pumping early or extra is always safe — CefDoMessageLoopWork with
    // nothing pending returns immediately — so the floor is what makes lock-free acceptable here.
    [[nodiscard]] bool should_pump(std::int64_t now_ms);

    [[nodiscard]] bool has_scheduled_work() const;

    // The absolute deadline of the pending schedule; meaningless when none is pending.
    [[nodiscard]] std::int64_t due_ms() const;

private:
    std::atomic<std::int64_t> due_ms_{0};
    std::atomic<bool> scheduled_{false};
};

// ------------------------------------------------------------------- the portable scripted host

// A browser host with no browser: scripted OSR frames in, recorded input out. It is what lets the
// Session-0-safe smoke drive the REAL compositor over REAL software-OSR pixels on a CI runner with
// no interactive desktop and no CEF, and what lets the layer/damage/popup logic be asserted on all
// three OSes.
//
// The frames it emits are premultiplied BGRA8, and the CALLER chooses the shape: the coded size,
// the visible rect inside it, and the row stride are all scripted, so a caller can drive the honest
// wide shape — an allocation larger than the visible rect at a padded stride — which is what
// catches the UV/stride bugs e03 documents. The Session-0 smoke does exactly that.
class ScriptedBrowserHost final : public IBrowserHost
{
public:
    [[nodiscard]] const char* name() const override { return "scripted"; }

    void resize(render::Extent2D logical_size, DpiScale dpi) override;
    void set_client_origin(PointI origin) override;
    void send_pointer(const PointerDispatch& dispatch, const PointerEvent& event) override;
    void send_key(const KeyEvent& event) override;
    void set_focus(bool focused) override;
    void set_drag_observer(IBrowserDragObserver* observer) override;
    void inject_drag(const OsrDragInjection& injection) override;
    bool pump(IBrowserFrameSink& sink) override;
    void execute_script(std::string_view source) override;
    void close() override { alive_ = false; }

    // --- scripting -------------------------------------------------------------------------------
    // Queue a frame the next pump() delivers. The pixel storage is COPIED and owned here, so a
    // caller cannot hand over a buffer that dies before the frame is consumed.
    //
    // `bytes_per_row` of 0 means a tight stride (coded_size.width * 4). A LARGER value is the real
    // OSR shape — a padded row stride — and is expressible here on purpose: hardcoding a tight
    // stride would make the padded producer shape unrepresentable through this host, so every
    // stride bug would be invisible to the smoke and the tests that drive the compositor through it.
    void queue_frame(BrowserLayer layer, render::Extent2D coded_size,
                     const render::Rect2D& visible_rect, std::vector<std::uint8_t> pixels,
                     std::uint32_t bytes_per_row = 0, std::vector<render::Rect2D> dirty = {});
    // Queue a solid premultiplied-BGRA frame — the common case for a smoke that cares about the
    // composite arithmetic rather than the picture.
    void queue_solid_frame(BrowserLayer layer, render::Extent2D coded_size,
                           const render::Rect2D& visible_rect, std::uint8_t b, std::uint8_t g,
                           std::uint8_t r, std::uint8_t a, std::uint32_t bytes_per_row = 0);
    // Queue a popup visibility change (delivered in order with the frames).
    void queue_popup_state(bool visible, const render::Rect2D& rect);

    // --- b1: raising the two drag callbacks a real browser raises ---------------------------------
    //
    // THE SCRIPTED HOST HAS NO RENDERER, so it can never start a drag of its own — and a window
    // whose drag wiring is exercised only through CEF is a window whose drag wiring is exercised by
    // ONE CI job, in the one translation unit the local gate cannot build. These two are the drag
    // half's equivalent of `queue_frame`: they raise exactly the callbacks `ShellCefClient` raises,
    // at a moment the caller chooses, so the REAL `EditorWindow` routing — pointer sample ->
    // `OsrDragSession` -> `inject_drag` — runs on all three default `build` legs.
    //
    // Delivered SYNCHRONOUSLY rather than queued for the next `pump()`, unlike the frames above,
    // and that difference is the point: a frame is data the compositor consumes, while
    // `StartDragging`'s return value is an ANSWER the caller acts on — the binding returns it to
    // CEF, so a test that could not see it would be asserting a different thing.
    [[nodiscard]] bool script_start_dragging(DragOperationMask allowed, PointI start_view_dip);
    void script_update_drag_cursor(DragOperation operation);

    // --- what it recorded ------------------------------------------------------------------------
    [[nodiscard]] const std::vector<PointerEvent>& pointers() const { return pointers_; }
    [[nodiscard]] const std::vector<KeyEvent>& keys() const { return keys_; }
    // The scripts a caller asked to run. Recorded, never executed — see execute_script above.
    [[nodiscard]] const std::vector<std::string>& scripts() const { return scripts_; }
    [[nodiscard]] render::Extent2D last_logical_size() const { return last_logical_size_; }
    [[nodiscard]] DpiScale last_dpi() const { return last_dpi_; }
    [[nodiscard]] int resize_count() const { return resize_count_; }
    // The a1 screen-mapping observable. The COUNT is what keeps the assertion honest in the other
    // direction: a window that moved must push, and an idle pump must not — an origin re-pushed
    // every iteration would drive a CEF callback storm nothing in the value alone would reveal.
    [[nodiscard]] PointI last_client_origin() const { return last_client_origin_; }
    [[nodiscard]] int client_origin_pushes() const { return client_origin_pushes_; }
    [[nodiscard]] bool focused() const { return focused_; }
    // Every drag step the window applied, IN ORDER — the b1 observable. The order is the assertion
    // that matters: CEF's two ordering rules (an `over` only after an `enter`; all `DragTarget*`
    // before all `DragSource*`) are claims about a SEQUENCE, which a count could never express.
    [[nodiscard]] const std::vector<OsrDragInjection>& drag_injections() const
    {
        return drag_injections_;
    }
    [[nodiscard]] bool has_drag_observer() const { return drag_observer_ != nullptr; }
    [[nodiscard]] bool alive() const { return alive_; }

private:
    // One scripted step: either a frame or a popup-state change, so ordering between the two is
    // expressible (a popup rect arriving before its first paint is the real CEF sequence).
    struct Step
    {
        bool is_popup_state = false;
        bool popup_visible = false;
        render::Rect2D popup_rect;
        BrowserLayer layer = BrowserLayer::view;
        render::Extent2D coded_size;
        render::Rect2D visible_rect;
        std::uint32_t bytes_per_row = 0; // 0 = tight
        std::vector<std::uint8_t> pixels;
        std::vector<render::Rect2D> dirty;
    };

    std::vector<Step> steps_;
    std::vector<PointerEvent> pointers_;
    std::vector<KeyEvent> keys_;
    std::vector<std::string> scripts_;
    std::vector<OsrDragInjection> drag_injections_;
    IBrowserDragObserver* drag_observer_ = nullptr;
    render::Extent2D last_logical_size_{};
    DpiScale last_dpi_;
    PointI last_client_origin_{};
    int resize_count_ = 0;
    int client_origin_pushes_ = 0;
    bool focused_ = false;
    bool alive_ = true;
};

// Fill a premultiplied-BGRA8 buffer of `coded_size` at `bytes_per_row` with a solid colour. Exposed
// because both the smoke and the tests need the same honest producer shape (a padded stride, an
// allocation larger than the visible rect).
[[nodiscard]] std::vector<std::uint8_t> make_premultiplied_bgra(render::Extent2D coded_size,
                                                                std::uint32_t bytes_per_row,
                                                                std::uint8_t b, std::uint8_t g,
                                                                std::uint8_t r, std::uint8_t a);

} // namespace context::editor::shell
