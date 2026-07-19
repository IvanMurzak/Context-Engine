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

#include <cstdint>
#include <string>
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
    virtual bool pump(IBrowserFrameSink& sink) = 0;

    virtual void close() = 0;
};

// ------------------------------------------------------------------- the portable scripted host

// A browser host with no browser: scripted OSR frames in, recorded input out. It is what lets the
// Session-0-safe smoke drive the REAL compositor over REAL software-OSR pixels on a CI runner with
// no interactive desktop and no CEF, and what lets the layer/damage/popup logic be asserted on all
// three OSes.
//
// The frames it emits are honest software-OSR frames — premultiplied BGRA8 with a coded_size larger
// than the visible rect, which is the shape that catches the UV bug e03 documents.
class ScriptedBrowserHost final : public IBrowserHost
{
public:
    [[nodiscard]] const char* name() const override { return "scripted"; }

    void resize(render::Extent2D logical_size, DpiScale dpi) override;
    void send_pointer(const PointerDispatch& dispatch, const PointerEvent& event) override;
    void send_key(const KeyEvent& event) override;
    void set_focus(bool focused) override;
    bool pump(IBrowserFrameSink& sink) override;
    void close() override { alive_ = false; }

    // --- scripting -------------------------------------------------------------------------------
    // Queue a frame the next pump() delivers. The pixel storage is COPIED and owned here, so a
    // caller cannot hand over a buffer that dies before the frame is consumed.
    void queue_frame(BrowserLayer layer, render::Extent2D coded_size,
                     const render::Rect2D& visible_rect, std::vector<std::uint8_t> pixels,
                     std::vector<render::Rect2D> dirty = {});
    // Queue a solid premultiplied-BGRA frame — the common case for a smoke that cares about the
    // composite arithmetic rather than the picture.
    void queue_solid_frame(BrowserLayer layer, render::Extent2D coded_size,
                           const render::Rect2D& visible_rect, std::uint8_t b, std::uint8_t g,
                           std::uint8_t r, std::uint8_t a);
    // Queue a popup visibility change (delivered in order with the frames).
    void queue_popup_state(bool visible, const render::Rect2D& rect);

    // --- what it recorded ------------------------------------------------------------------------
    [[nodiscard]] const std::vector<PointerEvent>& pointers() const { return pointers_; }
    [[nodiscard]] const std::vector<KeyEvent>& keys() const { return keys_; }
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
        std::vector<std::uint8_t> pixels;
        std::vector<render::Rect2D> dirty;
    };

    std::vector<Step> steps_;
    std::vector<PointerEvent> pointers_;
    std::vector<KeyEvent> keys_;
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
