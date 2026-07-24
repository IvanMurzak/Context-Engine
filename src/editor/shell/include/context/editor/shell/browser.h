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
    // A hidden popup MUST drop its layer rather than merely stop drawing it: CEF reuses the popup
    // texture for the next dropdown at a different size, and a retained stale layer would composite
    // the previous menu's pixels for the frame between the hide and the next paint.
    virtual void on_popup_state(bool visible, const render::Rect2D& rect) = 0;
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

    virtual void send_pointer(const PointerDispatch& dispatch, const PointerEvent& event) = 0;
    virtual void send_key(const KeyEvent& event) = 0;
    virtual void set_focus(bool focused) = 0;

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
    void send_pointer(const PointerDispatch& dispatch, const PointerEvent& event) override;
    void send_key(const KeyEvent& event) override;
    void set_focus(bool focused) override;
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

    // --- what it recorded ------------------------------------------------------------------------
    [[nodiscard]] const std::vector<PointerEvent>& pointers() const { return pointers_; }
    [[nodiscard]] const std::vector<KeyEvent>& keys() const { return keys_; }
    // The scripts a caller asked to run. Recorded, never executed — see execute_script above.
    [[nodiscard]] const std::vector<std::string>& scripts() const { return scripts_; }
    [[nodiscard]] render::Extent2D last_logical_size() const { return last_logical_size_; }
    [[nodiscard]] DpiScale last_dpi() const { return last_dpi_; }
    [[nodiscard]] int resize_count() const { return resize_count_; }
    [[nodiscard]] bool focused() const { return focused_; }
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
    render::Extent2D last_logical_size_{};
    DpiScale last_dpi_;
    int resize_count_ = 0;
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
