// The Shell's native-window seam (design 03 §1) — one interface, real Windows and Linux backends,
// and a portable headless backend.
//
// e04 shipped the WINDOWS backend; e12a adds the LINUX one (X11/XWayland — native Wayland is
// post-M9 per D21). macOS (NSWindow/NSView) is e12b's. That remaining gap is REPORTED, not silent:
// make_window_backend returns a selection carrying a diagnostic that names e12b, mirroring how e03's
// make_present_blitter reports its own missing platforms. A shell that quietly opened no window
// would look identical to one that opened an invisible one.
//
// THE PLATFORM BLIND SPOT, AND WHAT IS DONE ABOUT IT. The local dev gate defines _WIN32, so a POSIX
// branch gets no compile signal at all there, and CI's Windows leg is the only thing that ever runs
// a WndProc. So EACH native backend is split in two, on the same seam:
//
//   * `translate_win32_message` / `translate_x11_event` — the EVENT DECODING, as pure functions over
//     plain integers. They include no <windows.h> and no <X11/Xlib.h>, name no HWND and no Display*,
//     and are compiled and executed by the ctest on all three OSes. This is where the bit-twiddling
//     that actually goes wrong lives (which half of LPARAM is x, that WM_MOUSEWHEEL's coordinates
//     are SCREEN-relative while every other mouse message's are client-relative, that X11 encodes
//     the wheel as buttons 4-7 whose RELEASE must not be counted a second time).
//   * `win32_window.cpp` / `x11_window.cpp` — the OS calls: RegisterClassExW / XCreateWindow, the
//     pump, per-monitor DPI. Each is honestly untested off its own platform, exactly as e03 left the
//     GDI blit body.
//
// The WM_* and X11 values below are declared locally so the decoders need no platform header. They
// are static_assert'ed against the real ones inside win32_window.cpp / x11_window.cpp, so a wrong
// constant is a COMPILE error on the platform that has the header rather than a runtime mystery.

#pragma once

#include "context/editor/shell/dpi.h"
#include "context/editor/shell/editor_state.h"
#include "context/editor/shell/input.h"
#include "context/render/rhi.h"

#include <cstddef>
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
    // Pure like every other seam on this interface (mirrors request_redraw): each backend states its own
    // answer rather than inheriting a default. The Win32 override does the real OS raise; the headless
    // backend explicitly no-ops (no OS window — the honest behaviour on a box with no interactive
    // desktop, Session 0 / CI). Keeping it pure means a future windowed backend (macOS/X11, e12) is
    // forced to implement the raise instead of silently no-op-ing the single-instance focus. Interactive
    // verification rides the deferred interactive-Windows pass (docs/shell.md); the arbitration handshake
    // itself is proven headlessly in the T2 drill.
    virtual void request_activation() = 0;

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
    void request_activation() override {} // no OS window to raise — the honest headless no-op
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

// --------------------------------------------------------------------- X11 event decoding (pure)

// The subset of X core event types the Shell decodes (X11/X.h). Declared here so the decoder is
// <X11/Xlib.h>-free and therefore compiled + tested on every OS; asserted against the real values
// in x11_window.cpp.
inline constexpr std::int32_t kX11KeyPress = 2;
inline constexpr std::int32_t kX11KeyRelease = 3;
inline constexpr std::int32_t kX11ButtonPress = 4;
inline constexpr std::int32_t kX11ButtonRelease = 5;
inline constexpr std::int32_t kX11MotionNotify = 6;
inline constexpr std::int32_t kX11EnterNotify = 7;
inline constexpr std::int32_t kX11LeaveNotify = 8;
inline constexpr std::int32_t kX11FocusIn = 9;
inline constexpr std::int32_t kX11FocusOut = 10;
inline constexpr std::int32_t kX11Expose = 12;
inline constexpr std::int32_t kX11ConfigureNotify = 22;
inline constexpr std::int32_t kX11ClientMessage = 33;

// Crossing/focus `mode` values. Only NotifyNormal is a real user-visible transition: X also
// synthesizes LeaveNotify/FocusOut around every pointer or keyboard GRAB, and forwarding those tells
// the browser the pointer left (dropping a hover) or that focus was lost, in the middle of a drag
// that is still very much happening.
inline constexpr std::int32_t kX11NotifyNormal = 0;
inline constexpr std::int32_t kX11NotifyGrab = 1;
inline constexpr std::int32_t kX11NotifyUngrab = 2;

// X11 modifier masks (X11/X.h). Note Mod1 is Alt and Mod4 is Super — X names them by slot, not by
// meaning, and every desktop maps them this way.
inline constexpr std::uint32_t kX11ShiftMask = 1u << 0;
inline constexpr std::uint32_t kX11LockMask = 1u << 1;
inline constexpr std::uint32_t kX11ControlMask = 1u << 2;
inline constexpr std::uint32_t kX11Mod1Mask = 1u << 3; // Alt
inline constexpr std::uint32_t kX11Mod4Mask = 1u << 6; // Super / Meta
inline constexpr std::uint32_t kX11Button1Mask = 1u << 8;
inline constexpr std::uint32_t kX11Button2Mask = 1u << 9;
inline constexpr std::uint32_t kX11Button3Mask = 1u << 10;

// X core button numbers. 1/2/3 are left/MIDDLE/right — NOT left/right/middle: X's middle button is
// 2, whereas Win32's MK_MBUTTON is the third bit, so a naive index-based port swaps middle and
// right on every three-button mouse. 4..7 are not buttons at all: the core protocol has no scroll
// axis, so a wheel notch arrives as a ButtonPress/ButtonRelease PAIR on 4 (up), 5 (down), 6 (left)
// or 7 (right). 8/9 are the browser back/forward thumb buttons, which the Shell does not route.
inline constexpr std::uint32_t kX11ButtonLeft = 1;
inline constexpr std::uint32_t kX11ButtonMiddle = 2;
inline constexpr std::uint32_t kX11ButtonRight = 3;
inline constexpr std::uint32_t kX11ButtonWheelUp = 4;
inline constexpr std::uint32_t kX11ButtonWheelDown = 5;
inline constexpr std::uint32_t kX11ButtonWheelLeft = 6;
inline constexpr std::uint32_t kX11ButtonWheelRight = 7;

// One X event, flattened to plain integers. The three fields the decoder cannot compute itself are
// resolved by the backend, which owns the Display connection: `keysym` (XkbKeycodeToKeysym),
// `text` (the input method's UTF-32 result) and `is_delete_window` (an interned atom comparison).
// Everything else is copied straight out of the XEvent union.
struct X11Event
{
    std::int32_t type = 0;
    // KeyPress/KeyRelease: the hardware keycode. ButtonPress/ButtonRelease: the button number.
    std::uint32_t detail = 0;
    // The modifier + button mask, as X reports it: the state BEFORE this event is applied.
    std::uint32_t state = 0;
    // Pointer + ConfigureNotify: client-area coordinates in physical pixels.
    std::int32_t x = 0;
    std::int32_t y = 0;
    // ConfigureNotify: the new client size.
    std::int32_t width = 0;
    std::int32_t height = 0;
    // Crossing (Enter/Leave) + focus events: NotifyNormal / NotifyGrab / NotifyUngrab.
    std::int32_t mode = kX11NotifyNormal;
    // Expose: how many more Expose events are already queued for this window.
    std::int32_t count = 0;
    // KeyPress/KeyRelease: the keysym the backend looked up for `detail` + `state`.
    std::uint32_t keysym = 0;
    // KeyPress: the character the input method produced, or 0 when the key produced no text.
    char32_t text = 0;
    // ClientMessage: true when it carries the WM_DELETE_WINDOW protocol atom.
    bool is_delete_window = false;
};

// The window geometry the decoder compares a ConfigureNotify against. X sends one for EVERY
// configure — including a pure move — so without the previous geometry a window dragged across the
// desktop would report a "resize" per motion step and reconfigure the swapchain each time.
struct X11WindowGeometry
{
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Up to two ShellEvents from ONE X event. Two X events legitimately carry two facts each:
// ConfigureNotify can change the size AND the position at once (a maximize does exactly that), and
// a KeyPress carries both the raw key and the character it produced — where Windows splits the
// latter into its own WM_CHAR. A fixed pair rather than a vector so decoding stays allocation-free
// on the pump's hot path.
struct X11EventBatch
{
    ShellEvent events[2];
    std::size_t count = 0;

    void push(const ShellEvent& event)
    {
        if (count < 2)
        {
            events[count++] = event;
        }
    }
};

// Map an X keysym to the Windows virtual-key code CEF expects in `windows_key_code`, on every
// platform (CefKeyEvent's field is named for Windows but is the cross-platform contract). Returns 0
// for a keysym with no VK equivalent, which is the honest "this key is text-only".
//
// WHAT IS EASY TO GET WRONG HERE, and is therefore asserted by the tests:
//   * a LOWERCASE latin keysym must map to the UPPERCASE VK code — `a` is keysym 0x61 but VK 0x41,
//     and passing 0x61 through makes every unshifted letter an unrecognised key in Chromium;
//   * the keypad digits are VK_NUMPAD0..9, not the row digits, or a numeric-keypad shortcut fires
//     the wrong command;
//   * punctuation goes to the VK_OEM_* codes, which are NOT the ASCII values.
[[nodiscard]] std::int32_t x11_keysym_to_windows_key_code(std::uint32_t keysym);

// Decode one X event against the window's PREVIOUS geometry. An empty batch is the decoder's "this
// event is not one the Shell cares about", which is most of them.
//
// WHAT IS EASY TO GET WRONG HERE, and is therefore asserted by the tests:
//   * a wheel notch is a ButtonPress/ButtonRelease PAIR on button 4-7, so decoding the RELEASE too
//     scrolls twice as far as the user asked;
//   * X's button-number order is left/MIDDLE/right (see kX11Button*);
//   * `state` is the mask BEFORE the event, so a press must add its own button and a release must
//     clear it, or the modifiers handed to the browser lag one event behind;
//   * LeaveNotify/FocusOut with mode != NotifyNormal is a GRAB artefact, not the pointer leaving;
//   * an Expose with count > 0 is one of a run, and repainting per rectangle is a wasted frame each.
[[nodiscard]] X11EventBatch translate_x11_event(const X11Event& event,
                                                const X11WindowGeometry& previous);

// Parse an `Xft.dpi` X-resource value ("144", "144.0", " 96 "). Returns nullopt for anything that is
// not a positive number, so a malformed resource falls through to the screen derivation below
// instead of scaling the whole editor by a garbage factor.
[[nodiscard]] std::optional<std::uint32_t> x11_parse_xft_dpi(std::string_view value);

// Derive a DPI from a screen's pixel width and its physical width in millimetres — X's only
// built-in answer, and a famously unreliable one. It is REFUSED (falling back to 96) outside a
// plausible band, because two common shapes produce nonsense rather than a wrong-but-close number:
// a server that reports a hardcoded 1-metre-wide screen (~33 dpi, which would shrink the UI to
// unreadable), and a multi-head X screen whose millimetres are the SUM across monitors while the
// pixels are too — plausible per-axis, wrong per-monitor. Real per-monitor DPI needs RandR, which
// is deliberately not a dependency of this task.
[[nodiscard]] DpiScale x11_screen_dpi(std::int32_t pixels, std::int32_t millimetres);

// ------------------------------------------------------------------------------ backend selection

struct WindowBackendSelection
{
    std::unique_ptr<IWindowBackend> backend;
    // Empty on success; otherwise why no native window could be created — including the honest
    // "this platform's backend is e12b's" for macOS.
    std::string diagnostic;
};

// The per-platform factories make_window_backend chooses between. Each returns nullptr when this
// build is not for its platform — mirroring make_win32_gdi_blitter, so the off-platform refusal is
// assertable by the ctest on every leg — or when window creation failed, in which case `error` says
// why. `make_x11_window_backend` also returns nullptr (with an error) in a build configured without
// the X11 development headers, and on a host with no reachable X display.
[[nodiscard]] std::unique_ptr<IWindowBackend> make_win32_window_backend(const WindowDesc& desc,
                                                                        std::string& error);
[[nodiscard]] std::unique_ptr<IWindowBackend> make_x11_window_backend(const WindowDesc& desc,
                                                                      std::string& error);

// Create the native window backend for the host platform. Returns a null backend plus a diagnostic
// on a platform whose backend does not exist yet, or when window creation failed.
[[nodiscard]] WindowBackendSelection make_window_backend(const WindowDesc& desc);

} // namespace context::editor::shell
