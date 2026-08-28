// The Linux X11/XWayland window backend (design 03 §1, D21) — XCreateWindow / the event pump /
// EWMH placement + activation, and the DPI the desktop actually publishes.
//
// This file is the OS-call half of the Linux seam, the exact counterpart of win32_window.cpp. All of
// the event DECODING lives in window.cpp as a pure function compiled and tested on every OS — see
// window.h for why. What remains here is genuinely Xlib-only and honestly untested off Linux.
//
// XWayland, NOT native Wayland (D21). A Wayland session runs XWayland by default, so this backend
// works on both; a native `wl_surface` backend is post-M9 and would be a THIRD implementation of the
// same 12-virtual interface, not a rewrite of this one.
//
// THREE X11 SHAPES THAT DIFFER FROM WIN32 AND ARE EASY TO GET WRONG:
//
//   1. ONE CONNECTION, N WINDOWS, ONE QUEUE. Xlib has a single event queue per Display, so — exactly
//      like the Win32 pump's NULL hwnd filter — whichever backend pumps drains events belonging to
//      its siblings too. They are dispatched by `xany.window` through a process-wide registry, and
//      the Display is reference-counted so the last window closing is what disconnects.
//   2. X ERRORS ARE ASYNCHRONOUS AND FATAL BY DEFAULT. Xlib's default error handler calls exit().
//      A window manager racing a property change is enough to hit one, so a handler is installed
//      that RECORDS instead — an editor must not die because a WM withdrew a window early.
//   3. THE KEYSYM FOR A VK CODE IS THE UNSHIFTED ONE. `Xutf8LookupString` reports the SHIFTED
//      keysym (Shift+1 is `exclam`), which has no virtual-key equivalent — so the VK code comes from
//      `XLookupKeysym(&event, 0)` (group 0, shift level 0) and the lookup call is used only for the
//      TEXT. Deriving both from one call silently kills every shifted-key shortcut.

#include "context/editor/shell/window.h"

#if defined(__linux__) && defined(CONTEXT_SHELL_HAS_X11)

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>

#include <clocale>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// X11/X.h macro-defines these one-word names, which collide with ordinary C++ identifiers in the
// headers included ABOVE it (render::NativeWindowKind::None, render::CompareFunction::Always,
// render::LoadOp / StoreOp members). Those declarations are already parsed, so undefining the macros
// here costs nothing and keeps any later use of them compiling. The numeric values are spelled
// directly at the few Xlib call sites that need them, each with a comment naming the constant.
#undef None
#undef Always
#undef Success

namespace context::editor::shell
{
namespace
{

// The locally-declared X constants the pure decoder uses, asserted against the REAL ones here, where
// <X11/Xlib.h> is available. A wrong constant is therefore a Linux COMPILE error rather than an
// event that silently decodes as something else at runtime on the one platform that runs it.
static_assert(kX11KeyPress == KeyPress, "KeyPress drifted");
static_assert(kX11KeyRelease == KeyRelease, "KeyRelease drifted");
static_assert(kX11ButtonPress == ButtonPress, "ButtonPress drifted");
static_assert(kX11ButtonRelease == ButtonRelease, "ButtonRelease drifted");
static_assert(kX11MotionNotify == MotionNotify, "MotionNotify drifted");
static_assert(kX11EnterNotify == EnterNotify, "EnterNotify drifted");
static_assert(kX11LeaveNotify == LeaveNotify, "LeaveNotify drifted");
static_assert(kX11FocusIn == FocusIn, "FocusIn drifted");
static_assert(kX11FocusOut == FocusOut, "FocusOut drifted");
static_assert(kX11Expose == Expose, "Expose drifted");
static_assert(kX11ConfigureNotify == ConfigureNotify, "ConfigureNotify drifted");
static_assert(kX11ClientMessage == ClientMessage, "ClientMessage drifted");
static_assert(kX11NotifyNormal == NotifyNormal, "NotifyNormal drifted");
static_assert(kX11NotifyGrab == NotifyGrab, "NotifyGrab drifted");
static_assert(kX11NotifyUngrab == NotifyUngrab, "NotifyUngrab drifted");
static_assert(kX11ShiftMask == ShiftMask, "ShiftMask drifted");
static_assert(kX11LockMask == LockMask, "LockMask drifted");
static_assert(kX11ControlMask == ControlMask, "ControlMask drifted");
static_assert(kX11Mod1Mask == Mod1Mask, "Mod1Mask drifted");
static_assert(kX11Mod4Mask == Mod4Mask, "Mod4Mask drifted");
static_assert(kX11Button1Mask == Button1Mask, "Button1Mask drifted");
static_assert(kX11Button2Mask == Button2Mask, "Button2Mask drifted");
static_assert(kX11Button3Mask == Button3Mask, "Button3Mask drifted");
static_assert(kX11ButtonLeft == Button1, "Button1 drifted");
static_assert(kX11ButtonMiddle == Button2, "Button2 drifted");
static_assert(kX11ButtonRight == Button3, "Button3 drifted");
static_assert(kX11ButtonWheelUp == Button4, "Button4 drifted");
static_assert(kX11ButtonWheelDown == Button5, "Button5 drifted");

class X11WindowBackend;

// X error handling. The default Xlib handler calls exit(), and an editor must never die because a
// window manager withdrew a window a microsecond before a property was set on it.
int x11_error_handler(Display* /*display*/, XErrorEvent* /*error*/)
{
    return 0;
}

// ------------------------------------------------------------------ the shared Display connection

struct X11Connection
{
    Display* display = nullptr;
    int screen = 0;
    Window root = 0;
    XIM input_method = nullptr;

    Atom wm_protocols = 0;
    Atom wm_delete_window = 0;
    Atom net_wm_ping = 0;
    Atom net_wm_state = 0;
    Atom net_wm_state_maximized_vert = 0;
    Atom net_wm_state_maximized_horz = 0;
    Atom net_active_window = 0;
    Atom net_wm_name = 0;
    Atom utf8_string = 0;

    int refs = 0;
    std::vector<X11WindowBackend*> windows;

    [[nodiscard]] X11WindowBackend* find(Window window) const;
};

X11Connection& x11_connection()
{
    static X11Connection connection;
    return connection;
}

// Open (or re-use) the process's X connection. `error` is filled and false returned when there is no
// reachable display — which is the ordinary state of a headless CI leg, not a crash.
bool x11_acquire(std::string& error)
{
    X11Connection& conn = x11_connection();
    if (conn.display != nullptr)
    {
        ++conn.refs;
        return true;
    }

    // Installed BEFORE the first request, so even a connection-time error is recorded rather than
    // exiting the process.
    ::XSetErrorHandler(&x11_error_handler);

    conn.display = ::XOpenDisplay(nullptr);
    if (conn.display == nullptr)
    {
        // `std::getenv` is MSVC C4996 under /W4 /WX, and the pre-push audit's check 3 flags it
        // syntactically. It is a FALSE POSITIVE here for a stronger reason than the documented
        // `#if defined(_MSC_VER)` split in keybindings_bridge.cpp: this ENTIRE translation unit body
        // is inside `#if defined(__linux__)`, a macro MSVC never defines, so no MSVC compiler ever
        // reaches this line. The variable is worth naming in the diagnostic — "DISPLAY=<unset>" is
        // the difference between "the backend is broken" and "you are on a headless box".
        const char* env = std::getenv("DISPLAY");
        error = std::string("XOpenDisplay failed (DISPLAY=") + (env != nullptr ? env : "<unset>") +
                "): there is no reachable X server";
        return false;
    }
    conn.screen = DefaultScreen(conn.display);
    conn.root = RootWindow(conn.display, conn.screen);

    // False (0) = create the atom if it does not exist. Every one of these is a name the desktop
    // already uses, so creation is the normal path on a bare server.
    conn.wm_protocols = ::XInternAtom(conn.display, "WM_PROTOCOLS", 0);
    conn.wm_delete_window = ::XInternAtom(conn.display, "WM_DELETE_WINDOW", 0);
    conn.net_wm_ping = ::XInternAtom(conn.display, "_NET_WM_PING", 0);
    conn.net_wm_state = ::XInternAtom(conn.display, "_NET_WM_STATE", 0);
    conn.net_wm_state_maximized_vert =
        ::XInternAtom(conn.display, "_NET_WM_STATE_MAXIMIZED_VERT", 0);
    conn.net_wm_state_maximized_horz =
        ::XInternAtom(conn.display, "_NET_WM_STATE_MAXIMIZED_HORZ", 0);
    conn.net_active_window = ::XInternAtom(conn.display, "_NET_ACTIVE_WINDOW", 0);
    conn.net_wm_name = ::XInternAtom(conn.display, "_NET_WM_NAME", 0);
    conn.utf8_string = ::XInternAtom(conn.display, "UTF8_STRING", 0);

    // The input method is BEST EFFORT. A minimal CI container (or a bare Xvfb) commonly has no
    // XIM server at all, and an editor that refused to open a window there would be useless — so a
    // null IM degrades to XLookupString's Latin-1 text rather than failing window creation.
    //
    // ⚠ LC_CTYPE ONLY — never LC_ALL. Xlib reports XSupportsLocale() for the CURRENT locale, which
    // without this call is "C": XOpenIM would then bind the built-in IM with the C locale's empty
    // compose table, dead keys and Compose sequences would produce nothing, and the multi-byte
    // branches of lookup_key() below could never fire — the whole XIM path would be dead code that
    // still looks wired. But widening this to LC_ALL (or LC_NUMERIC) would set the process's
    // numeric locale from the environment, and a locale whose decimal separator is a comma changes
    // how the C numeric routines format doubles — which would corrupt the canonical-JSON byte form
    // the authored-data contract (R-FILE-001) depends on, from a window backend, invisibly.
    if (::setlocale(LC_CTYPE, "") != nullptr && ::XSupportsLocale() != 0)
    {
        (void)::XSetLocaleModifiers("");
        conn.input_method = ::XOpenIM(conn.display, nullptr, nullptr, nullptr);
    }

    conn.refs = 1;
    return true;
}

void x11_release()
{
    X11Connection& conn = x11_connection();
    if (conn.display == nullptr || conn.refs <= 0)
    {
        return;
    }
    if (--conn.refs > 0)
    {
        return;
    }
    if (conn.input_method != nullptr)
    {
        ::XCloseIM(conn.input_method);
        conn.input_method = nullptr;
    }
    ::XCloseDisplay(conn.display);
    conn.display = nullptr;
    conn.windows.clear();
}

// ------------------------------------------------------------------------------------ DPI lookup

// The desktop's own answer first (`Xft.dpi` in the X resource database — what GTK/Qt read and what a
// user's scaling setting actually writes), then the screen derivation, then 96. Both fallbacks are
// PURE functions in window.cpp so their rules are asserted on all three legs.
DpiScale x11_read_dpi(Display* display, int screen)
{
    const char* resources = ::XResourceManagerString(display);
    if (resources != nullptr)
    {
        XrmDatabase database = ::XrmGetStringDatabase(resources);
        if (database != nullptr)
        {
            char* type = nullptr;
            XrmValue value{};
            if (::XrmGetResource(database, "Xft.dpi", "Xft.Dpi", &type, &value) != 0 &&
                value.addr != nullptr)
            {
                const std::optional<std::uint32_t> parsed = x11_parse_xft_dpi(
                    std::string_view(value.addr, std::strlen(value.addr)));
                ::XrmDestroyDatabase(database);
                if (parsed.has_value())
                {
                    return make_dpi_scale(*parsed);
                }
                return x11_screen_dpi(DisplayWidth(display, screen),
                                      DisplayWidthMM(display, screen));
            }
            ::XrmDestroyDatabase(database);
        }
    }
    return x11_screen_dpi(DisplayWidth(display, screen), DisplayWidthMM(display, screen));
}

// ------------------------------------------------------------------------------------ the backend

class X11WindowBackend final : public IWindowBackend
{
public:
    X11WindowBackend() = default;

    X11WindowBackend(const X11WindowBackend&) = delete;
    X11WindowBackend& operator=(const X11WindowBackend&) = delete;

    ~X11WindowBackend() override { destroy(); }

    [[nodiscard]] bool create(const WindowDesc& desc, std::string& error);

    [[nodiscard]] const char* name() const override { return "x11"; }

    [[nodiscard]] render::NativeWindowDesc native_window() const override
    {
        render::NativeWindowDesc native;
        if (window_ == 0)
        {
            return native; // kind stays None — the honest "nothing presentable here"
        }
        native.kind = render::NativeWindowKind::XlibWindow;
        // rhi.h's XlibWindow contract: the handle is the XID widened to a pointer, NOT a pointer to
        // it. Taking the address of the member would hand every consumer a dangling stack pointer
        // the moment this getter returned.
        native.handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(window_));
        native.display = x11_connection().display;
        return native;
    }

    // Derived, never cached separately: geometry_ is the ONE record of what the server last told us
    // this window is, and a second copy would only add a sync invariant for the handle() switch to
    // break silently.
    [[nodiscard]] render::Extent2D client_size() const override
    {
        return render::Extent2D{geometry_.width, geometry_.height};
    }
    [[nodiscard]] DpiScale dpi() const override { return dpi_; }
    [[nodiscard]] bool alive() const override { return window_ != 0 && alive_; }

    bool pump(std::vector<ShellEvent>& out) override;

    void request_redraw() override
    {
        if (window_ == 0)
        {
            return;
        }
        // A zero width/height means "to the window's edges"; the trailing 1 (True) is what makes
        // this GENERATE an Expose rather than merely clearing pixels the compositor owns anyway.
        ::XClearArea(x11_connection().display, window_, 0, 0, 0, 0, 1);
        ::XFlush(x11_connection().display);
    }

    void request_activation() override;
    void set_title(std::string_view title) override;

    // a1 (editor-window-chrome): the two chrome verbs. `set_maximized` is the PROMOTION of this
    // backend's own private EWMH `_NET_WM_STATE` shape to the seam — the body is unchanged; the
    // interface adopted its signature. `minimize` is the EWMH-era XIconifyWindow request; like
    // request_activation, the WM is entitled to refuse either.
    void minimize() override;
    void set_maximized(bool maximized) override;

    [[nodiscard]] WindowPlacement placement() const override;
    void apply_placement(const WindowPlacement& placement) override;

    void close() override;

    // Called by whichever backend is pumping, for an event addressed to THIS window.
    void handle(const XEvent& event);
    [[nodiscard]] Window window_id() const { return window_; }

private:
    // This window's top-left in ROOT coordinates. The one rule the X11 backend repeats most: under
    // a reparenting window manager (nearly all of them) the window's own x/y are relative to the
    // WM's FRAME, so persisting or comparing those walks the window a titlebar up-and-left. Every
    // site that needs a position goes through here so the rule cannot drift between them. Leaves
    // its arguments untouched and returns false when the server refuses the translation.
    [[nodiscard]] bool root_origin(std::int32_t& x, std::int32_t& y) const;
    void destroy();
    [[nodiscard]] bool read_maximized() const;
    // The UTF-32 character (0 for none) and the UNSHIFTED keysym for a key event — see the file
    // header's third note on why those come from two different lookups.
    void lookup_key(const XKeyEvent& key, std::uint32_t& keysym, char32_t& text) const;

    Window window_ = 0;
    XIC input_context_ = nullptr;
    std::vector<ShellEvent> pending_;
    DpiScale dpi_;
    X11WindowGeometry geometry_;
    bool alive_ = true;
};

X11WindowBackend* X11Connection::find(Window window) const
{
    for (X11WindowBackend* backend : windows)
    {
        if (backend != nullptr && backend->window_id() == window)
        {
            return backend;
        }
    }
    return nullptr;
}

bool X11WindowBackend::create(const WindowDesc& desc, std::string& error)
{
    if (!x11_acquire(error))
    {
        return false;
    }
    X11Connection& conn = x11_connection();

    dpi_ = x11_read_dpi(conn.display, conn.screen);

    render::Extent2D physical = to_physical(desc.logical_size, dpi_);
    if (desc.placement.has_value() && !render::is_empty(desc.placement->size()))
    {
        physical = desc.placement->size();
    }
    const int x = desc.placement.has_value() ? desc.placement->x : 0;
    const int y = desc.placement.has_value() ? desc.placement->y : 0;

    XSetWindowAttributes attributes{};
    attributes.background_pixmap = 0; // None — the compositor owns every pixel; see WM_ERASEBKGND
    attributes.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                            ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                            EnterWindowMask | LeaveWindowMask | FocusChangeMask;

    window_ = ::XCreateWindow(conn.display, conn.root, x, y, physical.width, physical.height, 0,
                              CopyFromParent, InputOutput, CopyFromParent,
                              CWBackPixmap | CWEventMask, &attributes);
    if (window_ == 0)
    {
        error = "XCreateWindow returned no window";
        x11_release();
        return false;
    }

    // WM_DELETE_WINDOW is what turns the titlebar's close button into an event instead of the
    // server killing the connection outright; _NET_WM_PING is what stops the desktop declaring the
    // editor "not responding" during a long frame.
    Atom protocols[2] = {conn.wm_delete_window, conn.net_wm_ping};
    (void)::XSetWMProtocols(conn.display, window_, protocols, 2);

    XClassHint class_hint{};
    // Xlib's fields are non-const char*, and it only reads them.
    char instance_name[] = "context-editor";
    char class_name[] = "ContextEditor";
    class_hint.res_name = instance_name;
    class_hint.res_class = class_name;
    (void)::XSetClassHint(conn.display, window_, &class_hint);

    if (desc.placement.has_value())
    {
        // Without USPosition/PPosition most window managers ignore the requested position entirely
        // and place the window by their own policy — losing the remembered placement silently.
        XSizeHints hints{};
        hints.flags = PPosition | USPosition;
        hints.x = x;
        hints.y = y;
        ::XSetWMNormalHints(conn.display, window_, &hints);
    }

    set_title(desc.title);

    if (conn.input_method != nullptr)
    {
        input_context_ = ::XCreateIC(conn.input_method, XNInputStyle,
                                     XIMPreeditNothing | XIMStatusNothing, XNClientWindow, window_,
                                     XNFocusWindow, window_, static_cast<void*>(nullptr));
    }

    conn.windows.push_back(this);

    if (desc.visible)
    {
        ::XMapWindow(conn.display, window_);
        if (desc.placement.has_value() && desc.placement->maximized)
        {
            set_maximized(true);
        }
    }
    // Round-trip so the server has processed the map before the client size is read below: an
    // unmapped window still reports its requested geometry, but a maximizing WM has not resized it
    // yet, and reading first would cache a size the very next ConfigureNotify contradicts.
    ::XSync(conn.display, 0);

    XWindowAttributes actual{};
    if (::XGetWindowAttributes(conn.display, window_, &actual) != 0)
    {
        geometry_.width = static_cast<std::uint32_t>(actual.width);
        geometry_.height = static_cast<std::uint32_t>(actual.height);
        // ROOT-relative, for the same reason placement() spells out below: under a reparenting WM
        // `actual.x/y` are relative to the WM's frame. Seeding the cache with those and then
        // comparing every later ConfigureNotify — which fills x/y from root_origin() too — compares
        // two different coordinate spaces, so the first configure after mapping always reports a
        // spurious `moved` and dirties the placement, costing an unnecessary editor-state write on
        // every single startup.
        geometry_.x = actual.x;
        geometry_.y = actual.y;
        (void)root_origin(geometry_.x, geometry_.y);
    }
    else
    {
        geometry_.width = physical.width;
        geometry_.height = physical.height;
        geometry_.x = x;
        geometry_.y = y;
    }

    error.clear();
    return true;
}

void X11WindowBackend::destroy()
{
    if (window_ == 0)
    {
        return;
    }
    X11Connection& conn = x11_connection();
    std::erase(conn.windows, this);
    if (input_context_ != nullptr)
    {
        ::XDestroyIC(input_context_);
        input_context_ = nullptr;
    }
    if (conn.display != nullptr)
    {
        ::XDestroyWindow(conn.display, window_);
        ::XFlush(conn.display);
    }
    window_ = 0;
    x11_release();
}

void X11WindowBackend::set_title(std::string_view title)
{
    if (window_ == 0)
    {
        return;
    }
    X11Connection& conn = x11_connection();
    const std::string owned(title);
    // BOTH properties: WM_NAME is Latin-1 legacy and _NET_WM_NAME is the UTF-8 one every modern
    // desktop actually displays. Setting only WM_NAME mangles any non-ASCII project name in the
    // titlebar; setting only _NET_WM_NAME leaves older tools (and xdotool-style automation) blank.
    ::XStoreName(conn.display, window_, owned.c_str());
    ::XChangeProperty(conn.display, window_, conn.net_wm_name, conn.utf8_string, 8,
                      PropModeReplace, reinterpret_cast<const unsigned char*>(owned.c_str()),
                      static_cast<int>(owned.size()));
    ::XFlush(conn.display);
}

void X11WindowBackend::request_activation()
{
    if (window_ == 0)
    {
        return;
    }
    X11Connection& conn = x11_connection();
    // Best-effort single-instance FOCUS (M9 e14b, D15/C-F23), the X11 counterpart of the Win32
    // override. _NET_ACTIVE_WINDOW is the EWMH request a compliant WM honours (and may refuse under
    // its own focus-stealing policy, exactly like SetForegroundWindow); XRaiseWindow is the bare-X
    // fallback for a server with no EWMH window manager at all, which is what Xvfb is.
    ::XRaiseWindow(conn.display, window_);

    XEvent message{};
    message.type = ClientMessage;
    message.xclient.window = window_;
    message.xclient.message_type = conn.net_active_window;
    message.xclient.format = 32;
    message.xclient.data.l[0] = 1; // source indication: an application, not a pager
    message.xclient.data.l[1] = 0; // CurrentTime
    message.xclient.data.l[2] = 0; // no requestor window
    (void)::XSendEvent(conn.display, conn.root, 0,
                       SubstructureRedirectMask | SubstructureNotifyMask, &message);
    ::XFlush(conn.display);
}

void X11WindowBackend::minimize()
{
    if (window_ == 0)
    {
        return;
    }
    X11Connection& conn = x11_connection();
    // XIconifyWindow sends the ICCCM WM_CHANGE_STATE client message for us — the request every WM
    // (and no-WM Xvfb, where it is a no-op) understands. Best-effort, like request_activation.
    (void)::XIconifyWindow(conn.display, window_, conn.screen);
    ::XFlush(conn.display);
}

void X11WindowBackend::set_maximized(bool maximized)
{
    if (window_ == 0)
    {
        return;
    }
    X11Connection& conn = x11_connection();
    XEvent message{};
    message.type = ClientMessage;
    message.xclient.window = window_;
    message.xclient.message_type = conn.net_wm_state;
    message.xclient.format = 32;
    message.xclient.data.l[0] = maximized ? 1 : 0; // _NET_WM_STATE_ADD / _REMOVE
    message.xclient.data.l[1] = static_cast<long>(conn.net_wm_state_maximized_vert);
    message.xclient.data.l[2] = static_cast<long>(conn.net_wm_state_maximized_horz);
    message.xclient.data.l[3] = 1; // source indication: an application
    (void)::XSendEvent(conn.display, conn.root, 0,
                       SubstructureRedirectMask | SubstructureNotifyMask, &message);
    ::XFlush(conn.display);
}

bool X11WindowBackend::read_maximized() const
{
    X11Connection& conn = x11_connection();
    Atom actual_type = 0;
    int actual_format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    if (::XGetWindowProperty(conn.display, window_, conn.net_wm_state, 0, 32, 0, XA_ATOM,
                             &actual_type, &actual_format, &count, &remaining, &data) != 0 ||
        data == nullptr)
    {
        return false;
    }
    bool vertical = false;
    bool horizontal = false;
    const Atom* atoms = reinterpret_cast<const Atom*>(data);
    for (unsigned long i = 0; i < count; ++i)
    {
        vertical = vertical || atoms[i] == conn.net_wm_state_maximized_vert;
        horizontal = horizontal || atoms[i] == conn.net_wm_state_maximized_horz;
    }
    ::XFree(data);
    // BOTH axes. A window maximized on one axis only is a legitimate EWMH state that is not what a
    // user means by "maximized", and restoring it as one would resize their window on next launch.
    return vertical && horizontal;
}

bool X11WindowBackend::root_origin(std::int32_t& x, std::int32_t& y) const
{
    if (window_ == 0)
    {
        return false;
    }
    X11Connection& conn = x11_connection();
    int root_x = 0;
    int root_y = 0;
    Window child = 0;
    if (::XTranslateCoordinates(conn.display, window_, conn.root, 0, 0, &root_x, &root_y, &child) ==
        0)
    {
        return false;
    }
    x = root_x;
    y = root_y;
    return true;
}

WindowPlacement X11WindowBackend::placement() const
{
    WindowPlacement placement;
    if (window_ == 0)
    {
        return placement;
    }
    X11Connection& conn = x11_connection();

    XWindowAttributes attributes{};
    if (::XGetWindowAttributes(conn.display, window_, &attributes) != 0)
    {
        placement.width = static_cast<std::uint32_t>(attributes.width);
        placement.height = static_cast<std::uint32_t>(attributes.height);
    }

    // Root coordinates, NOT the attributes' x/y — see root_origin().
    (void)root_origin(placement.x, placement.y);

    placement.maximized = read_maximized();
    // X core has no per-monitor identity — that needs RandR, deliberately not a dependency here — so
    // this names the SCREEN honestly rather than inventing a monitor name that would not match.
    placement.monitor = std::string("x11:") + DisplayString(conn.display) + "." +
                        std::to_string(conn.screen);
    return placement;
}

void X11WindowBackend::apply_placement(const WindowPlacement& placement)
{
    if (window_ == 0 || render::is_empty(placement.size()))
    {
        return;
    }
    X11Connection& conn = x11_connection();
    // Un-maximize FIRST when restoring: a maximized window ignores a move/resize, so applying the
    // restore rect while still maximized silently does nothing and the window snaps back.
    if (!placement.maximized && read_maximized())
    {
        set_maximized(false);
    }
    ::XMoveResizeWindow(conn.display, window_, placement.x, placement.y, placement.width,
                        placement.height);
    if (placement.maximized)
    {
        set_maximized(true);
    }
    ::XFlush(conn.display);
}

void X11WindowBackend::close()
{
    if (window_ == 0)
    {
        return;
    }
    X11Connection& conn = x11_connection();
    // Post a WM_DELETE_WINDOW to OURSELVES rather than destroying the window here — the exact
    // counterpart of the Win32 backend's PostMessage(WM_CLOSE). The owner loop then observes a
    // close_requested through the ordinary event path and shuts down in its own order, instead of
    // having the window vanish underneath a compositor that is mid-frame.
    XEvent message{};
    message.type = ClientMessage;
    message.xclient.window = window_;
    message.xclient.message_type = conn.wm_protocols;
    message.xclient.format = 32;
    message.xclient.data.l[0] = static_cast<long>(conn.wm_delete_window);
    message.xclient.data.l[1] = 0; // CurrentTime
    (void)::XSendEvent(conn.display, window_, 0, 0, &message);
    ::XFlush(conn.display);
}

void X11WindowBackend::lookup_key(const XKeyEvent& key, std::uint32_t& keysym,
                                  char32_t& text) const
{
    // The VK code's keysym is the UNSHIFTED one — index 0 is group 0, shift level 0. See the file
    // header's third note: the lookup calls below report the SHIFTED keysym, which for Shift+1 is
    // `exclam` and has no virtual-key equivalent at all.
    keysym = static_cast<std::uint32_t>(::XLookupKeysym(const_cast<XKeyEvent*>(&key), 0));
    text = 0;
    if (key.type != KeyPress)
    {
        return;
    }

    char buffer[32] = {};
    int written = 0;
    if (input_context_ != nullptr)
    {
        Status status = 0;
        KeySym ignored = 0;
        written = ::Xutf8LookupString(input_context_, const_cast<XKeyEvent*>(&key), buffer,
                                      static_cast<int>(sizeof(buffer) - 1), &ignored, &status);
        // XLookupChars / XLookupBoth are the two statuses that produced text; XLookupKeySym and
        // XLookupNone did not, and reading the buffer on those yields whatever was there before.
        if (status != XLookupChars && status != XLookupBoth)
        {
            written = 0;
        }
        if (written > 0)
        {
            // Decode the FIRST UTF-8 code point. A single key press produces one character in every
            // case the Shell forwards; a preedit commit that produced more is deliberately truncated
            // rather than silently concatenated into one bogus code point.
            const auto byte = [&](int index) {
                return static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[index]));
            };
            const std::uint32_t lead = byte(0);
            if (lead < 0x80u)
            {
                text = static_cast<char32_t>(lead);
            }
            else if ((lead & 0xE0u) == 0xC0u && written >= 2)
            {
                text = static_cast<char32_t>(((lead & 0x1Fu) << 6) | (byte(1) & 0x3Fu));
            }
            else if ((lead & 0xF0u) == 0xE0u && written >= 3)
            {
                text = static_cast<char32_t>(((lead & 0x0Fu) << 12) | ((byte(1) & 0x3Fu) << 6) |
                                             (byte(2) & 0x3Fu));
            }
            else if ((lead & 0xF8u) == 0xF0u && written >= 4)
            {
                text = static_cast<char32_t>(((lead & 0x07u) << 18) | ((byte(1) & 0x3Fu) << 12) |
                                             ((byte(2) & 0x3Fu) << 6) | (byte(3) & 0x3Fu));
            }
        }
        return;
    }

    // No input method (a bare Xvfb, a minimal container): XLookupString still yields Latin-1 text
    // for the ASCII keys, which is enough for the editor to be usable rather than mute.
    KeySym ignored = 0;
    written = ::XLookupString(const_cast<XKeyEvent*>(&key), buffer,
                              static_cast<int>(sizeof(buffer) - 1), &ignored, nullptr);
    if (written > 0)
    {
        text = static_cast<char32_t>(static_cast<unsigned char>(buffer[0]));
    }
}

void X11WindowBackend::handle(const XEvent& event)
{
    X11Connection& conn = x11_connection();

    // The two protocol messages the Shell answers rather than decodes.
    if (event.type == ClientMessage && event.xclient.message_type == conn.wm_protocols &&
        static_cast<Atom>(event.xclient.data.l[0]) == conn.net_wm_ping)
    {
        // Bounce _NET_WM_PING back to the root so the desktop knows the editor is alive. Not
        // answering it is what makes a window manager grey the window out and offer to kill it.
        XEvent reply = event;
        reply.xclient.window = conn.root;
        (void)::XSendEvent(conn.display, conn.root, 0,
                           SubstructureRedirectMask | SubstructureNotifyMask, &reply);
        return;
    }
    if (event.type == DestroyNotify)
    {
        alive_ = false;
        return;
    }

    // An XIC only receives input while it is FOCUSED. XCreateIC's XNFocusWindow names which window
    // the context belongs to; it does not focus it. Without this pair an external XIM (ibus/fcitx
    // via XMODIFIERS) never sees the keystrokes, so preedit and every CJK/dead-key composition are
    // silently unavailable — and XFilterEvent can swallow keys that then produce no text at all.
    // Real focus transitions only: X synthesizes FocusIn/FocusOut around every grab (menus, drags),
    // and toggling the IC on those churns the IM mid-gesture. Same NotifyNormal rule the decoder
    // applies before reporting a focus change to the Shell.
    if ((event.type == FocusIn || event.type == FocusOut) && input_context_ != nullptr &&
        event.xfocus.mode == NotifyNormal)
    {
        if (event.type == FocusIn)
        {
            ::XSetICFocus(input_context_);
        }
        else
        {
            ::XUnsetICFocus(input_context_);
        }
    }

    X11Event flat;
    flat.type = event.type;
    switch (event.type)
    {
    case ConfigureNotify:
        flat.width = event.xconfigure.width;
        flat.height = event.xconfigure.height;
        // Absolute, not the parent-relative x/y a reparenting WM reports — see root_origin().
        flat.x = event.xconfigure.x;
        flat.y = event.xconfigure.y;
        (void)root_origin(flat.x, flat.y);
        break;
    case Expose:
        flat.count = event.xexpose.count;
        break;
    case FocusIn:
    case FocusOut:
        flat.mode = event.xfocus.mode;
        break;
    case EnterNotify:
    case LeaveNotify:
        flat.mode = event.xcrossing.mode;
        flat.state = event.xcrossing.state;
        flat.x = event.xcrossing.x;
        flat.y = event.xcrossing.y;
        break;
    case MotionNotify:
        flat.state = event.xmotion.state;
        flat.x = event.xmotion.x;
        flat.y = event.xmotion.y;
        break;
    case ButtonPress:
    case ButtonRelease:
        flat.detail = event.xbutton.button;
        flat.state = event.xbutton.state;
        flat.x = event.xbutton.x;
        flat.y = event.xbutton.y;
        break;
    case KeyPress:
    case KeyRelease:
        flat.detail = event.xkey.keycode;
        flat.state = event.xkey.state;
        flat.x = event.xkey.x;
        flat.y = event.xkey.y;
        lookup_key(event.xkey, flat.keysym, flat.text);
        break;
    case ClientMessage:
        flat.is_delete_window = event.xclient.message_type == conn.wm_protocols &&
                                static_cast<Atom>(event.xclient.data.l[0]) == conn.wm_delete_window;
        break;
    default:
        break;
    }

    const X11EventBatch batch = translate_x11_event(flat, geometry_);
    for (std::size_t i = 0; i < batch.count; ++i)
    {
        ShellEvent decoded = batch.events[i];
        // The backend applies the state-changing events to ITSELF before handing them on, exactly
        // as the Win32 backend does — by the time a resize is observed, client_size() is current.
        switch (decoded.kind)
        {
        case ShellEventKind::resize:
            geometry_.width = decoded.size.width;
            geometry_.height = decoded.size.height;
            break;
        case ShellEventKind::moved:
            geometry_.x = decoded.position.x;
            geometry_.y = decoded.position.y;
            break;
        case ShellEventKind::close_requested:
            alive_ = false;
            break;
        default:
            break;
        }
        pending_.push_back(decoded);
    }
}

bool X11WindowBackend::pump(std::vector<ShellEvent>& out)
{
    X11Connection& conn = x11_connection();
    if (conn.display != nullptr)
    {
        // XPending, not XNextEvent alone: the owner loop also drives CEF and the compositor, so it
        // must never block inside the X queue (03 §1 — the single-threaded owner loop). XPending
        // flushes the output buffer and reads whatever has arrived, without waiting.
        while (::XPending(conn.display) > 0)
        {
            XEvent event;
            ::XNextEvent(conn.display, &event);
            // The input method gets first refusal (dead keys, a CJK preedit). A filtered event has
            // been consumed and MUST NOT be decoded — doing so types the preedit twice.
            if (::XFilterEvent(&event, 0 /* None */) != 0)
            {
                continue;
            }
            // Dispatched by window id: one queue serves every window on this connection, exactly
            // like the Win32 pump's NULL hwnd filter.
            X11WindowBackend* target = conn.find(event.xany.window);
            if (target != nullptr)
            {
                target->handle(event);
            }
        }
    }
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
    return window_ != 0 && alive_;
}

} // namespace

std::unique_ptr<IWindowBackend> make_x11_window_backend(const WindowDesc& desc, std::string& error)
{
    auto backend = std::make_unique<X11WindowBackend>();
    if (!backend->create(desc, error))
    {
        return nullptr;
    }
    return backend;
}

} // namespace context::editor::shell

#else // !(__linux__ && CONTEXT_SHELL_HAS_X11)

#include <string>

namespace context::editor::shell
{

std::unique_ptr<IWindowBackend> make_x11_window_backend(const WindowDesc& /*desc*/,
                                                        std::string& error)
{
    // Compiled everywhere, real only on a Linux build that FOUND the X11 development headers — the
    // same shape make_win32_gdi_blitter takes, so the refusal is a value the ctest asserts on every
    // leg rather than a symbol that is absent. The two reasons are reported separately because they
    // call for different fixes: a wrong platform is expected, a missing package is installable.
#if defined(__linux__)
    error = "this build was configured without the X11 development headers "
            "(install libx11-dev / libX11-devel and re-run cmake)";
#else
    error = "the X11 window backend is compiled only on Linux";
#endif
    return nullptr;
}

} // namespace context::editor::shell

#endif // __linux__ && CONTEXT_SHELL_HAS_X11
