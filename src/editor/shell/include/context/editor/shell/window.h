// The Shell's native-window seam (design 03 §1) — one interface, real Windows, Linux and macOS
// backends, and a portable headless backend.
//
// e04 shipped the WINDOWS backend; e12a added the LINUX one (X11/XWayland — native Wayland is
// post-M9 per D21); e12b adds the macOS one (NSWindow/NSView, CEF-free — the .app bundle + helper
// processes that let CEF run there are e12c's). All three platforms now have a real backend, so
// make_window_backend's remaining diagnostics are about a FAILED creation (no display, no GUI
// session, a refused window) rather than about a platform nobody implemented yet — the honest
// report e03's make_present_blitter established. A shell that quietly opened no window would look
// identical to one that opened an invisible one.
//
// THE PLATFORM BLIND SPOT, AND WHAT IS DONE ABOUT IT. The local dev gate defines _WIN32, so a POSIX
// branch gets no compile signal at all there, and CI's Windows leg is the only thing that ever runs
// a WndProc. macOS is worse still: no local gate compiles an `__APPLE__` branch AT ALL, and until
// M9 e12c-3 no CI job RAN a windowed macOS test (`editor-shell-cocoa-window` now does, on that ONE
// leg). So EACH native backend is split in two, on the same seam:
//
//   * `translate_win32_message` / `translate_x11_event` / `translate_ns_event` — the EVENT DECODING,
//     as pure functions over plain integers. They include no <windows.h>, no <X11/Xlib.h> and no
//     <AppKit/AppKit.h>, name no HWND, no Display* and no NSEvent*, and are compiled and executed by
//     the ctest on all three OSes. This is where the bit-twiddling that actually goes wrong lives
//     (which half of LPARAM is x, that WM_MOUSEWHEEL's coordinates are SCREEN-relative while every
//     other mouse message's are client-relative, that X11 encodes the wheel as buttons 4-7 whose
//     RELEASE must not be counted a second time, that Cocoa's y axis points UP and its virtual key
//     codes are POSITIONAL — 0x00 is `A` and 0x01 is `S`).
//   * `win32_window.cpp` / `x11_window.cpp` / `cocoa_window.mm` — the OS calls: RegisterClassExW /
//     XCreateWindow / [NSWindow initWithContentRect:], the pump, per-monitor DPI. Each is honestly
//     untested off its own platform, exactly as e03 left the GDI blit body.
//
// The WM_*, X11 and NSEvent values below are declared locally so the decoders need no platform
// header. They are static_assert'ed against the real ones inside win32_window.cpp / x11_window.cpp /
// cocoa_window.mm, so a wrong constant is a COMPILE error on the platform that has the header rather
// than a runtime mystery.

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
    // moved: the new top-left, in screen coordinates — EXCEPT on macOS, where it is the Cocoa frame
    // origin (BOTTOM-left, in POINTS), the same space WindowPlacement keeps there and for the same
    // reason (docs/shell.md). Consumers treat it as an opaque "the window moved" signal and re-read
    // placement() rather than interpreting it, which is why the exception is survivable.
    PointI position;
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
    // every native backend so that loop can land without touching the interface. Nothing calls it
    // yet.
    virtual void request_redraw() = 0;

    virtual void set_title(std::string_view title) = 0;

    // Best-effort: bring this window to the foreground — the D15/C-F23 single-instance FOCUS a second
    // opener (`context edit .`) requests when it finds this editor already on the project (M9 e14b).
    // Pure like every other seam on this interface (mirrors request_redraw): each backend states its own
    // answer rather than inheriting a default. The Win32 override does the real OS raise; the headless
    // backend explicitly no-ops (no OS window — the honest behaviour on a box with no interactive
    // desktop, Session 0 / CI). Keeping it pure means every windowed backend is forced to implement
    // the raise instead of silently no-op-ing the single-instance focus: the X11 override (e12a) does
    // the real EWMH activation and the Cocoa one (e12b) the real deminiaturize +
    // makeKeyAndOrderFront. Interactive
    // verification rides the deferred interactive-Windows pass (docs/shell.md); the arbitration handshake
    // itself is proven headlessly in the T2 drill.
    virtual void request_activation() = 0;

    // The two chrome verbs the window-control surface dispatches (editor-window-chrome a1, target
    // design 02 §5). Pure like request_activation / request_redraw — every backend states its own
    // answer — because a silent default here would make a web-drawn minimize button a no-op on
    // exactly the platform nobody compiled. `set_maximized` PROMOTES the shape the X11 backend
    // already carried privately (EWMH _NET_WM_STATE): a bool target rather than a toggle, so the
    // caller can express "restore" without first asking the OS what state it is in — the toggle
    // (`window.toggle-maximize`) is composed one level up, in the bridge handler, from
    // `placement().maximized`. Both are best-effort asks, exactly like request_activation: a WM /
    // OS is entitled to refuse, and the observable state remains `placement()`.
    virtual void minimize() = 0;
    virtual void set_maximized(bool maximized) = 0;

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
    // Honest STATE-ONLY chrome (a1): there is no OS window to iconify or zoom, so the headless
    // backend records what was asked — `minimized()` for tests, and the placement's maximized bit,
    // which is exactly the lever the placement-poll -> `editor.ui` fact test flips.
    void minimize() override { minimized_ = true; }
    void set_maximized(bool maximized) override { placement_.maximized = maximized; }
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
    // What `minimize()` recorded — the headless observable for the a1 window-control surface.
    [[nodiscard]] bool minimized() const { return minimized_; }

private:
    std::vector<ShellEvent> queued_;
    render::NativeWindowDesc native_;
    render::Extent2D size_;
    DpiScale dpi_;
    WindowPlacement placement_;
    std::string title_;
    int redraw_requests_ = 0;
    bool minimized_ = false;
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

// Up to three ShellEvents from ONE native event. Several native events legitimately carry more than
// one fact: an X11 ConfigureNotify alters the size AND the position at once (a maximize does exactly
// that), an X11 KeyPress / Cocoa NSKeyDown carries both the raw key and the character it produced
// (where Windows splits the latter into its own WM_CHAR), and a Cocoa window dragged onto a Retina
// display changes size, position AND backing scale in one step — which is the three. A fixed array
// rather than a vector so decoding stays allocation-free on the pump's hot path.
struct ShellEventBatch
{
    static constexpr std::size_t kCapacity = 3;

    ShellEvent events[kCapacity];
    std::size_t count = 0;
    // A push past kCapacity is RECORDED, not merely ignored. The bound is now exactly SATURATED —
    // translate_ns_window_geometry emits all three of resize + moved + dpi_changed for one Retina
    // drag — so the next decoder fact added on any platform silently loses an event, and it would do
    // so most easily on macOS, which produces the most facts per native event and has no windowed CI
    // coverage at all. The decoders are pure and run on every leg, so their tests assert this stays
    // false and the invariant is PROVEN rather than assumed. Dropping remains the behaviour (a torn
    // batch is worse than a missing tail event on the pump's hot path); what changes is that it can
    // no longer happen unobserved.
    bool overflowed = false;

    void push(const ShellEvent& event)
    {
        if (count < kCapacity)
        {
            events[count++] = event;
            return;
        }
        overflowed = true;
    }
};

// The name the X11 decoder was introduced under (e12a). Kept as an alias rather than renamed at
// every call site: e12b generalised the type for the Cocoa decoder, and a rename would have churned
// the Linux backend for no behavioural reason.
using X11EventBatch = ShellEventBatch;

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

// ------------------------------------------------------------------- Cocoa/NSEvent decoding (pure)

// The subset of NSEventType the Shell decodes (AppKit/NSEvent.h). Declared here so the decoder is
// <AppKit/AppKit.h>-free and therefore compiled + tested on every OS; asserted against the real
// values in cocoa_window.mm.
inline constexpr std::int32_t kNsLeftMouseDown = 1;
inline constexpr std::int32_t kNsLeftMouseUp = 2;
inline constexpr std::int32_t kNsRightMouseDown = 3;
inline constexpr std::int32_t kNsRightMouseUp = 4;
inline constexpr std::int32_t kNsMouseMoved = 5;
inline constexpr std::int32_t kNsLeftMouseDragged = 6;
inline constexpr std::int32_t kNsRightMouseDragged = 7;
inline constexpr std::int32_t kNsMouseEntered = 8;
inline constexpr std::int32_t kNsMouseExited = 9;
inline constexpr std::int32_t kNsKeyDown = 10;
inline constexpr std::int32_t kNsKeyUp = 11;
inline constexpr std::int32_t kNsFlagsChanged = 12;
inline constexpr std::int32_t kNsScrollWheel = 22;
inline constexpr std::int32_t kNsOtherMouseDown = 25;
inline constexpr std::int32_t kNsOtherMouseUp = 26;
inline constexpr std::int32_t kNsOtherMouseDragged = 27;

// NSEventModifierFlags. Note Option is the Alt key and Command is the Meta key — Cocoa names them
// by their keycaps, and mapping Command onto `control` (the shape a Windows-first port reaches for)
// makes every Cmd shortcut in the editor fire as a Ctrl shortcut.
//
// The WHOLE mask is pinned here, deliberately, so do not read "no decoder reads this one" as dead
// code: cocoa_window.mm static_asserts every entry against the real AppKit value, which is what makes
// this <AppKit/AppKit.h>-free header safe to compile and test on Linux and Windows. Two entries have
// no decoder reader on purpose — kNsModifierNumericPad, and kNsModifierFunction, which fn
// deliberately does not toggle (see ns_modifier_flag_for_key_code's default arm for why).
inline constexpr std::uint64_t kNsModifierCapsLock = 1ull << 16;
inline constexpr std::uint64_t kNsModifierShift = 1ull << 17;
inline constexpr std::uint64_t kNsModifierControl = 1ull << 18;
inline constexpr std::uint64_t kNsModifierOption = 1ull << 19;  // Alt
inline constexpr std::uint64_t kNsModifierCommand = 1ull << 20; // Meta
inline constexpr std::uint64_t kNsModifierNumericPad = 1ull << 21;
inline constexpr std::uint64_t kNsModifierFunction = 1ull << 23;

// +[NSEvent pressedMouseButtons] bits. ⚠ Bit 1 is the RIGHT button and bit 2 the MIDDLE one — the
// inverse of X11's 1/2/3 = left/middle/right, so a port of either backend that carries its
// neighbour's ordering across swaps the two on every three-button mouse.
inline constexpr std::uint32_t kNsPressedButtonLeft = 1u << 0;
inline constexpr std::uint32_t kNsPressedButtonRight = 1u << 1;
inline constexpr std::uint32_t kNsPressedButtonMiddle = 1u << 2;

// -[NSEvent buttonNumber] for the OtherMouse* family. Left and right have their OWN event types, so
// the only value the Shell routes here is the middle button's.
inline constexpr std::int32_t kNsOtherButtonMiddle = 2;

// The macOS virtual key codes the decoder needs to name (Carbon HIToolbox Events.h). Only the ones
// whose identity is load-bearing are named; the rest go through the table in window.cpp.
//
// ⚠ THESE ARE POSITIONAL, NOT ALPHABETICAL. kVK_ANSI_A is 0x00 and kVK_ANSI_S is 0x01 — they follow
// the physical ASDF row of the original Apple keyboard. `key_code + 'A'` is not merely imprecise, it
// is wrong for every letter but `A`.
inline constexpr std::uint32_t kNsVkAnsiA = 0x00;
inline constexpr std::uint32_t kNsVkAnsiS = 0x01;
inline constexpr std::uint32_t kNsVkAnsiZ = 0x06;
inline constexpr std::uint32_t kNsVkAnsi5 = 0x17;
inline constexpr std::uint32_t kNsVkAnsi6 = 0x16;
inline constexpr std::uint32_t kNsVkReturn = 0x24;
inline constexpr std::uint32_t kNsVkTab = 0x30;
inline constexpr std::uint32_t kNsVkSpace = 0x31;
// ⚠ NAMED FOR THE KEYCAP, NOT THE BEHAVIOUR. On an Apple keyboard the big key above Return is
// engraved "delete" and BACKSPACES; the one in the navigation cluster is kVK_ForwardDelete. Mapping
// kVK_Delete to VK_DELETE (the reading its name invites) makes Backspace delete forwards.
inline constexpr std::uint32_t kNsVkDelete = 0x33;        // Backspace
inline constexpr std::uint32_t kNsVkForwardDelete = 0x75; // Delete
inline constexpr std::uint32_t kNsVkEscape = 0x35;
inline constexpr std::uint32_t kNsVkCommand = 0x37;
inline constexpr std::uint32_t kNsVkRightCommand = 0x36;
inline constexpr std::uint32_t kNsVkShift = 0x38;
inline constexpr std::uint32_t kNsVkRightShift = 0x3C;
inline constexpr std::uint32_t kNsVkCapsLock = 0x39;
inline constexpr std::uint32_t kNsVkOption = 0x3A;
inline constexpr std::uint32_t kNsVkRightOption = 0x3D;
inline constexpr std::uint32_t kNsVkControl = 0x3B;
inline constexpr std::uint32_t kNsVkRightControl = 0x3E;
inline constexpr std::uint32_t kNsVkFunction = 0x3F;
inline constexpr std::uint32_t kNsVkKeypad0 = 0x52;
inline constexpr std::uint32_t kNsVkF1 = 0x7A;
inline constexpr std::uint32_t kNsVkF5 = 0x60;
inline constexpr std::uint32_t kNsVkLeftArrow = 0x7B;
inline constexpr std::uint32_t kNsVkRightArrow = 0x7C;
inline constexpr std::uint32_t kNsVkDownArrow = 0x7D;
inline constexpr std::uint32_t kNsVkUpArrow = 0x7E;

// Cocoa's line-based scroll unit. AppKit reports a wheel notch as `scrollingDeltaY == 1` (lines)
// rather than Windows' 120, so a notch is scaled by kWheelDelta to reach the ONE convention the
// browser is handed on every platform. A PRECISE (trackpad / high-resolution wheel) delta is
// already in POINTS and is forwarded as-is — scaling it by 120 would make a two-finger swipe scroll
// a hundred screens.
inline constexpr std::int32_t kNsLinesToWheelDelta = kWheelDelta;

// One NSEvent, flattened to plain numbers. The two fields the decoder cannot compute itself are
// resolved by the backend, which owns the NSView: `location_x/location_y` (converted into VIEW
// coordinates via -convertPoint:fromView:nil, still in Cocoa POINTS with a BOTTOM-LEFT origin) and
// `text` (the first UTF-16 CODE UNIT of -characters, i.e. the input method's result — not a code
// point: an astral character arrives as its leading surrogate, matching what the Win32 decoder gets
// from a WM_CHAR pair's first message).
struct NsEvent
{
    std::int32_t type = 0;
    // The modifier mask AS OF this event — unlike X11's `state`, Cocoa reports it AFTER the change.
    std::uint64_t modifier_flags = 0;
    // View-relative, Cocoa POINTS, origin BOTTOM-LEFT (see NsViewGeometry for the flip).
    double location_x = 0.0;
    double location_y = 0.0;
    // OtherMouse* only: see kNsOtherButtonMiddle.
    std::int32_t button_number = 0;
    // +[NSEvent pressedMouseButtons] — read from the OS by the backend once per event, exactly as
    // the Win32 backend reads the keyboard modifier state, so the decoder stays pure. It is a CLASS
    // property on NSEvent rather than a field of the event, so there is nothing to unpack from the
    // event itself the way WM_MOUSEMOVE's wParam or X11's `state` carry it.
    std::uint32_t pressed_mouse_buttons = 0;
    // Cocoa counts clicks for us — there is no separate double-click event type as on Windows.
    std::int32_t click_count = 1;
    // ScrollWheel: -scrollingDeltaX / -scrollingDeltaY. Positive Y is a scroll AWAY from the user
    // and positive X is a scroll to the RIGHT, matching the Win32 and X11 convention.
    double scrolling_delta_x = 0.0;
    double scrolling_delta_y = 0.0;
    // -hasPreciseScrollingDeltas: the deltas are in POINTS, not lines (see kNsLinesToWheelDelta).
    bool has_precise_scrolling_deltas = false;
    // KeyDown/KeyUp/FlagsChanged: the LAYOUT-INDEPENDENT hardware key code.
    std::uint32_t key_code = 0;
    // KeyDown: the character -characters produced, or 0 when the key produced no text.
    char32_t text = 0;
};

// What the decoder needs to know about the view an event landed in.
struct NsViewGeometry
{
    // The view's height in Cocoa POINTS. THE FLIP: Cocoa's origin is the BOTTOM-left of the view
    // while the region map, CEF and every other backend speak TOP-left, so y_top = height - y.
    // It must be the VIEW's height, not the WINDOW's — they differ by the titlebar, and using the
    // window's puts every pointer ~28 points off, which reads as a mis-calibrated screen.
    double height_points = 0.0;
    // The backing scale as a DpiScale (backingScaleFactor 2.0 -> 192 dpi). Cocoa reports POINTS and
    // this seam's contract is PHYSICAL PIXELS, so every position is scaled through it. On a 1x
    // display the two are equal, which is exactly why a missing scale ships looking correct and
    // breaks on Retina only.
    DpiScale dpi;
    // The modifier mask BEFORE this event, for the FlagsChanged diff (see translate_ns_event).
    std::uint64_t previous_modifier_flags = 0;
};

// The Cocoa window geometry a change is compared against. AppKit delivers -windowDidResize: and
// -windowDidMove: separately, but it also delivers BOTH for one live-resize step and re-delivers an
// unchanged geometry on a backing-property change — so the "report only what actually changed" rule
// is the same one the X11 ConfigureNotify path needs, and lives in the same kind of pure function.
struct NsWindowGeometry
{
    // The content view's size in Cocoa POINTS.
    double width_points = 0.0;
    double height_points = 0.0;
    // The window frame's origin in Cocoa SCREEN coordinates. ⚠ Origin BOTTOM-LEFT of the primary
    // display, and deliberately NOT flipped: `placement()` and `apply_placement()` are the only
    // consumers, they are the same platform's, and they round-trip exactly. Flipping here would
    // need the screen height — an OS call — inside a function whose whole point is having none.
    std::int32_t x = 0;
    std::int32_t y = 0;
    // The window's backingScaleFactor as a DpiScale.
    DpiScale dpi;
};

// backingScaleFactor -> DpiScale. A non-finite or non-positive factor falls back to 1x rather than
// scaling the whole editor by a garbage number (the refusal x11_screen_dpi makes too); make_dpi_scale
// then clamps whatever survives.
[[nodiscard]] DpiScale ns_dpi_from_backing_scale(double backing_scale);

// A Cocoa POINT extent -> physical pixels. Zero for anything not strictly positive, which also
// catches NaN (every comparison against a NaN is false, so the `!(x > 0)` spelling is deliberate —
// `x <= 0` would let a NaN through). Zero is a MEANINGFUL answer, not a failure: a miniaturized
// window legitimately measures 0 points, and translate_ns_window_geometry's empty-resize guard
// depends on getting 0 back rather than the never-collapse clamp dpi.h's to_physical applies.
[[nodiscard]] std::uint32_t ns_extent_to_physical(double points, DpiScale dpi);

// A view-relative Cocoa POINT -> the PHYSICAL client pixel the region map speaks: flip the y axis
// against the view height, then scale both axes by the backing factor. Round-to-nearest, and signed
// throughout — a captured drag legitimately leaves the view on any edge, and flooring a negative
// away from zero is how a drag one pixel above the view reads as being 4000 pixels below it.
[[nodiscard]] PointI ns_view_point_to_physical(double x_points, double y_points,
                                               const NsViewGeometry& view);

// Map a macOS hardware key code to the Windows virtual-key code CEF expects in `windows_key_code`
// on every platform (CefKeyEvent's field is named for Windows but is the cross-platform contract).
// Returns 0 for a key with no VK equivalent, which is the honest "this key is text-only".
//
// WHAT IS EASY TO GET WRONG HERE, and is therefore asserted by the tests:
//   * the codes are POSITIONAL — 0x00 is `A`, 0x01 is `S`, 0x06 is `Z`. Any arithmetic mapping is
//     wrong; this is a table;
//   * kVK_Delete (0x33) is BACKSPACE and kVK_ForwardDelete (0x75) is Delete;
//   * the digit row is not monotonic — 6 is 0x16 and 5 is 0x17, i.e. transposed;
//   * the function keys are scattered (F1 is 0x7A but F5 is 0x60), so no contiguous run exists;
//   * Command maps to VK_LWIN/VK_RWIN and Option to VK_MENU — not to VK_CONTROL.
[[nodiscard]] std::int32_t ns_key_code_to_windows_key_code(std::uint32_t key_code);

// Decode one NSEvent. An empty batch is the decoder's "this event is not one the Shell cares
// about", which is most of them.
//
// WHAT IS EASY TO GET WRONG HERE, and is therefore asserted by the tests:
//   * the y axis points UP (see NsViewGeometry::height_points);
//   * coordinates are POINTS, not pixels — correct-looking on a 1x display and half-scale on Retina;
//   * a modifier key produces NO KeyDown/KeyUp at all: it arrives as NSEventTypeFlagsChanged, which
//     says WHICH key but not whether it went down or up. That is recovered by diffing the mask
//     against `view.previous_modifier_flags`;
//   * NSEventTypeOtherMouse* numbers the MIDDLE button 2 (left and right have their own types), so
//     the X11 1/2/3 ordering does not transfer;
//   * a precise (trackpad) scroll delta is already in points and must not be multiplied by 120.
[[nodiscard]] ShellEventBatch translate_ns_event(const NsEvent& event, const NsViewGeometry& view);

// Decode a window geometry CHANGE into the resize / moved / dpi_changed facts it actually carries.
// See NsWindowGeometry for why this exists at all rather than being three delegate callbacks.
[[nodiscard]] ShellEventBatch translate_ns_window_geometry(const NsWindowGeometry& previous,
                                                            const NsWindowGeometry& current);

// ------------------------------------------------------------------------------ backend selection

struct WindowBackendSelection
{
    std::unique_ptr<IWindowBackend> backend;
    // Empty on success; otherwise why no native window could be created. Since e12b every platform
    // HAS a backend, so this now only ever reports a real creation failure — no display, no GUI
    // session, a refused window — never "nobody has written this one yet".
    std::string diagnostic;
};

// The per-platform factories make_window_backend chooses between. Each returns nullptr when this
// build is not for its platform — mirroring make_win32_gdi_blitter, so the off-platform refusal is
// assertable by the ctest on every leg — or when window creation failed, in which case `error` says
// why. `make_x11_window_backend` also returns nullptr (with an error) in a build configured without
// the X11 development headers, and on a host with no reachable X display; `make_cocoa_window_backend`
// does the same on a macOS box with no GUI (Aqua) session, which is the ordinary state of a build
// bot running under a plain ssh/launchd context.
[[nodiscard]] std::unique_ptr<IWindowBackend> make_win32_window_backend(const WindowDesc& desc,
                                                                        std::string& error);
[[nodiscard]] std::unique_ptr<IWindowBackend> make_x11_window_backend(const WindowDesc& desc,
                                                                      std::string& error);
[[nodiscard]] std::unique_ptr<IWindowBackend> make_cocoa_window_backend(const WindowDesc& desc,
                                                                        std::string& error);

// Create the native window backend for the host platform. Returns a null backend plus a diagnostic
// when window creation failed.
[[nodiscard]] WindowBackendSelection make_window_backend(const WindowDesc& desc);

} // namespace context::editor::shell
