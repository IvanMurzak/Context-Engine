// The Shell's native-window seam (design 03 §1) — one interface, a real Windows backend, and a
// portable headless backend.
//
// v1 ships the WINDOWS backend; macOS (NSWindow/NSView) and Linux (X11/XWayland) are e12's. That gap
// is REPORTED, not silent: make_window_backend returns a selection carrying a diagnostic that names
// e12, mirroring how e03's make_present_blitter reports its own missing platforms. A shell that
// quietly opened no window would look identical to one that opened an invisible one.
//
// THE PLATFORM BLIND SPOT, AND WHAT IS DONE ABOUT IT. The local dev gate defines _WIN32, so a POSIX
// branch gets no compile signal at all, and CI's Windows leg is the only thing that ever runs a
// WndProc. So the Win32 backend is split in two:
//
//   * `translate_win32_message` — the MESSAGE DECODING, as a pure function over plain integers. It
//     includes no <windows.h>, names no HWND, and is compiled and executed by the ctest on all three
//     OSes. This is where the bit-twiddling that actually goes wrong lives (which half of LPARAM is
//     x, that WM_MOUSEWHEEL's coordinates are SCREEN-relative while every other mouse message's are
//     client-relative, that a wheel delta is signed and packed in the high word).
//   * `win32_window.cpp` — the OS calls: RegisterClassExW, CreateWindowExW, the pump, per-monitor-v2
//     DPI. Windows-only and honestly untested off-Windows, exactly as e03 left the GDI blit body.
//
// The WM_* values below are declared locally so the decoder needs no <windows.h>. They are
// static_assert'ed against the real ones inside win32_window.cpp, so a wrong constant is a Windows
// COMPILE error rather than a runtime mystery.

#pragma once

#include "context/editor/shell/dpi.h"
#include "context/editor/shell/editor_state.h"
#include "context/editor/shell/input.h"
#include "context/render/rhi.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::shell
{

// What the pump produced. `none` never reaches a caller — it is the decoder's "this message is not
// one the Shell cares about", which is most of them.
enum class ShellEventKind
{
    none,
    resize,      // the client area changed size (physical pixels)
    dpi_changed, // the window moved to a monitor with a different scale, or scaling changed
    moved,       // the window moved (placement persistence)
    pointer,
    key,
    focus_gained,
    focus_lost,
    paint_requested, // the OS asked for a repaint (WM_PAINT) — damage, not a frame budget
    close_requested,
};

struct ShellEvent
{
    ShellEventKind kind = ShellEventKind::none;
    render::Extent2D size;  // resize: the new PHYSICAL client size
    DpiScale dpi;           // dpi_changed: the new scale
    PointerEvent pointer;   // pointer
    KeyEvent key;           // key
    PointI position;        // moved: the new top-left, in screen coordinates
};

struct WindowDesc
{
    std::string title = "Context Editor";
    // The window's LOGICAL (DIP) size. Physical pixels are derived from the monitor's DPI at
    // creation, which is what makes a "1280x800 window" the same apparent size on every monitor.
    render::Extent2D logical_size{1280, 800};
    bool visible = true;
    // Optional remembered placement (editor-state.json). Applied at creation when present.
    std::optional<WindowPlacement> placement;
};

// One native window. Not thread-safe: it is owned and pumped by the single shell main thread.
class IWindowBackend
{
public:
    virtual ~IWindowBackend() = default;

    [[nodiscard]] virtual const char* name() const = 0;

    // The descriptor the RHI wraps as a surface. `kind == None` is the honest report of "there is no
    // presentable native window here" (the headless backend), which routes the compositor to the CPU
    // present fallback rather than failing.
    [[nodiscard]] virtual render::NativeWindowDesc native_window() const = 0;

    [[nodiscard]] virtual render::Extent2D client_size() const = 0; // PHYSICAL pixels
    [[nodiscard]] virtual DpiScale dpi() const = 0;
    [[nodiscard]] virtual bool alive() const = 0;

    // Drain pending OS events into `out` (appending). Returns false once the window is gone —
    // the owner loop's termination condition.
    virtual bool pump(std::vector<ShellEvent>& out) = 0;

    // Ask the OS to repaint. NOT currently on the redraw path: the owner loop calls
    // WindowCompositor::render_frame() every iteration and that is damage-gated internally, so a
    // browser paint already gets its frame without one. This is the seam an event-driven loop needs
    // (wait on the OS queue instead of polling — see docs/shell.md §10), and it is implemented by
    // both backends so that loop can land without touching the interface. Nothing calls it yet.
    virtual void request_redraw() = 0;

    virtual void set_title(std::string_view title) = 0;

    // Best-effort: bring this window to the foreground — the D15/C-F23 single-instance FOCUS a second
    // opener (`context edit .`) requests when it finds this editor already on the project (M9 e14b).
    // NON-pure: a backend with no OS window (the headless backend) simply does nothing, which is the
    // honest behaviour on a box with no interactive desktop (Session 0 / CI). The real OS raise is the
    // Win32 override; its interactive verification rides the deferred interactive-Windows pass
    // (docs/shell.md) — the arbitration handshake itself is proven headlessly in the T2 drill.
    virtual void request_activation() {}

    [[nodiscard]] virtual WindowPlacement placement() const = 0;
    virtual void apply_placement(const WindowPlacement& placement) = 0;

    virtual void close() = 0;
};

// ------------------------------------------------------------- the portable headless backend

// A window with no OS behind it: scripted events in, recorded calls out. This is what makes the
// Session-0-safe smoke possible — the self-hosted Windows CI runner has no interactive desktop, so
// the blocking smoke drives the REAL shell loop through this backend rather than a real HWND.
//
// It is not a test double in the pejorative sense: it is the honest offscreen shell, the same way
// e03's MemoryBlitter is an honest present target.
class HeadlessWindowBackend final : public IWindowBackend
{
public:
    explicit HeadlessWindowBackend(const WindowDesc& desc);

    [[nodiscard]] const char* name() const override { return "headless"; }
    [[nodiscard]] render::NativeWindowDesc native_window() const override { return native_; }
    [[nodiscard]] render::Extent2D client_size() const override { return size_; }
    [[nodiscard]] DpiScale dpi() const override { return dpi_; }
    [[nodiscard]] bool alive() const override { return alive_; }

    bool pump(std::vector<ShellEvent>& out) override;
    void request_redraw() override;
    void set_title(std::string_view title) override { title_ = std::string(title); }
    [[nodiscard]] WindowPlacement placement() const override { return placement_; }
    void apply_placement(const WindowPlacement& placement) override;
    void close() override { alive_ = false; }

    // --- driving it ------------------------------------------------------------------------------
    // Queue an event the next pump() will deliver.
    void post(const ShellEvent& event);
    // Present a native handle to the RHI. Off by default (kind None => the CPU present path); a
    // caller that wants to exercise the GPU composite against a fake backend sets one.
    void set_native_window(render::NativeWindowDesc native) { native_ = native; }
    void set_dpi(DpiScale scale) { dpi_ = scale; }

    [[nodiscard]] const std::string& title() const { return title_; }
    [[nodiscard]] int redraw_requests() const { return redraw_requests_; }

private:
    std::vector<ShellEvent> queued_;
    render::NativeWindowDesc native_;
    render::Extent2D size_;
    DpiScale dpi_;
    WindowPlacement placement_;
    std::string title_;
    int redraw_requests_ = 0;
    bool alive_ = true;
};

// ------------------------------------------------------------------ Win32 message decoding (pure)

// The subset of WM_* the Shell decodes. Declared here so the decoder is <windows.h>-free and
// therefore compiled + tested on every OS; asserted against the real values in win32_window.cpp.
inline constexpr std::uint32_t kWmDestroy = 0x0002;
inline constexpr std::uint32_t kWmSize = 0x0005;
inline constexpr std::uint32_t kWmSetFocus = 0x0007;
inline constexpr std::uint32_t kWmKillFocus = 0x0008;
inline constexpr std::uint32_t kWmPaint = 0x000F;
inline constexpr std::uint32_t kWmClose = 0x0010;
inline constexpr std::uint32_t kWmMove = 0x0003;
inline constexpr std::uint32_t kWmKeyDown = 0x0100;
inline constexpr std::uint32_t kWmKeyUp = 0x0101;
inline constexpr std::uint32_t kWmChar = 0x0102;
inline constexpr std::uint32_t kWmSysKeyDown = 0x0104;
inline constexpr std::uint32_t kWmSysKeyUp = 0x0105;
inline constexpr std::uint32_t kWmSysChar = 0x0106;
inline constexpr std::uint32_t kWmMouseMove = 0x0200;
inline constexpr std::uint32_t kWmLButtonDown = 0x0201;
inline constexpr std::uint32_t kWmLButtonUp = 0x0202;
inline constexpr std::uint32_t kWmLButtonDblClk = 0x0203;
inline constexpr std::uint32_t kWmRButtonDown = 0x0204;
inline constexpr std::uint32_t kWmRButtonUp = 0x0205;
inline constexpr std::uint32_t kWmRButtonDblClk = 0x0206;
inline constexpr std::uint32_t kWmMButtonDown = 0x0207;
inline constexpr std::uint32_t kWmMButtonUp = 0x0208;
inline constexpr std::uint32_t kWmMButtonDblClk = 0x0209;
inline constexpr std::uint32_t kWmMouseWheel = 0x020A;
inline constexpr std::uint32_t kWmMouseHWheel = 0x020E;
inline constexpr std::uint32_t kWmMouseLeave = 0x02A3;
inline constexpr std::uint32_t kWmDpiChanged = 0x02E0;

// SIZE_MINIMIZED: WM_SIZE's wParam when the window was minimized. A minimized window reports a 0x0
// client size, and forwarding that as a resize would ask the swapchain to reconfigure to nothing
// every frame the window stays minimized.
inline constexpr std::uint64_t kSizeMinimized = 1;

// WHEEL_DELTA: one notch of a mouse wheel.
inline constexpr std::int32_t kWheelDelta = 120;

// The modifier-key state the Shell cannot read out of a message's parameters (WM_KEYDOWN carries no
// shift/control bits). The backend reads it from the OS once per message and passes it in, so the
// decoder stays pure.
struct Win32ModifierState
{
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool meta = false;
};

struct Win32Message
{
    std::uint32_t message = 0;
    std::uint64_t wparam = 0;
    std::int64_t lparam = 0;
};

// Decode one Win32 message. Returns nullopt for every message the Shell does not handle.
//
// It takes NO DPI: nothing here needs the window's current scale. Positions are decoded in the
// PHYSICAL client pixels the OS reports — the same space the region map is published in — and the
// DIP conversion happens later, at dispatch (input.h §3). WM_DPICHANGED carries its own DPI in
// wParam. A scale parameter here would be an input the function ignores, i.e. a lie about what the
// decoding depends on.
//
// WHAT IS EASY TO GET WRONG HERE, and is therefore asserted by the tests:
//   * the coordinate halves of LPARAM are SIGNED 16-bit — a pointer dragged left of the client area
//     reports 65500, not -36, unless the cast goes through int16_t;
//   * WM_MOUSEWHEEL's coordinates are SCREEN-relative, unlike every other mouse message. The
//     decoder therefore reports the wheel WITHOUT a position and the backend supplies the last known
//     client position — a wheel event routed by a screen coordinate would arbitrate against the
//     wrong region every time the window is not at the origin;
//   * WM_DPICHANGED's DPI is in the LOW word of wParam (X and Y are separate and always equal).
[[nodiscard]] std::optional<ShellEvent> translate_win32_message(const Win32Message& message,
                                                                const Win32ModifierState& modifiers);

// ------------------------------------------------------------------------------ backend selection

struct WindowBackendSelection
{
    std::unique_ptr<IWindowBackend> backend;
    // Empty on success; otherwise why no native window could be created — including the honest
    // "this platform's backend is e12's" for macOS/Linux.
    std::string diagnostic;
};

// Create the native window backend for the host platform. Returns a null backend plus a diagnostic
// on a platform whose backend does not exist yet, or when window creation failed.
[[nodiscard]] WindowBackendSelection make_window_backend(const WindowDesc& desc);

} // namespace context::editor::shell
