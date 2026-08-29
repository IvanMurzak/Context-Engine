// The Windows window backend (design 03 §1) — RegisterClassExW / CreateWindowExW / WndProc, the
// integrated pump, and REAL per-monitor-v2 DPI (the spike pinned DPI to 1.0; this replaces that).
//
// This file is the OS-call half of the seam. All of the message DECODING lives in window.cpp as a
// pure function compiled and tested on every OS — see window.h for why. What remains here is
// genuinely Windows-only and honestly untested off-Windows, exactly as e03 left the GDI blit body.
//
// THE DPI APIS ARE RESOLVED DYNAMICALLY, not linked. `GetDpiForWindow`,
// `SetProcessDpiAwarenessContext` and `AdjustWindowRectExForDpi` are Windows 10 1607/1703 additions,
// and whether they are DECLARED depends on the SDK: the CI Windows leg builds with MSVC while the
// local dev gate builds the same file with Strawberry GCC's MinGW headers, which do not reliably
// declare them. A GetProcAddress lookup compiles identically under both toolchains and degrades to
// the 96-dpi behaviour on an older Windows instead of failing to build on one of them.

#include "context/editor/shell/window.h"

#if defined(_WIN32)

// NOMINMAX: <windows.h> otherwise macro-defines min/max and mangles std::min/std::max at every later
// include. WIN32_LEAN_AND_MEAN drops the winsock/OLE headers this file has no use for.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// The ONE Dwm call this design makes (editor-window-chrome b1, 02 §3): the dark-mode edge
// tint/shadow attribute. dwmapi ships with the Windows SDK on both toolchains and joins the link
// list in CMakeLists.txt beside user32.
#include <dwmapi.h>

#include <string>
#include <vector>

// The locally-declared WM_* constants the pure decoder uses are asserted against the REAL ones here,
// where <windows.h> is available. A wrong constant is therefore a Windows COMPILE error rather than
// a message that silently decodes as something else at runtime on the one platform that runs it.
namespace
{
using context::editor::shell::kSizeMinimized;
using context::editor::shell::kWheelDelta;

static_assert(context::editor::shell::kWmDestroy == WM_DESTROY, "WM_DESTROY drifted");
static_assert(context::editor::shell::kWmSize == WM_SIZE, "WM_SIZE drifted");
static_assert(context::editor::shell::kWmSetFocus == WM_SETFOCUS, "WM_SETFOCUS drifted");
static_assert(context::editor::shell::kWmKillFocus == WM_KILLFOCUS, "WM_KILLFOCUS drifted");
static_assert(context::editor::shell::kWmPaint == WM_PAINT, "WM_PAINT drifted");
static_assert(context::editor::shell::kWmClose == WM_CLOSE, "WM_CLOSE drifted");
static_assert(context::editor::shell::kWmMove == WM_MOVE, "WM_MOVE drifted");
static_assert(context::editor::shell::kWmKeyDown == WM_KEYDOWN, "WM_KEYDOWN drifted");
static_assert(context::editor::shell::kWmKeyUp == WM_KEYUP, "WM_KEYUP drifted");
static_assert(context::editor::shell::kWmChar == WM_CHAR, "WM_CHAR drifted");
static_assert(context::editor::shell::kWmSysKeyDown == WM_SYSKEYDOWN, "WM_SYSKEYDOWN drifted");
static_assert(context::editor::shell::kWmSysKeyUp == WM_SYSKEYUP, "WM_SYSKEYUP drifted");
static_assert(context::editor::shell::kWmSysChar == WM_SYSCHAR, "WM_SYSCHAR drifted");
static_assert(context::editor::shell::kWmMouseMove == WM_MOUSEMOVE, "WM_MOUSEMOVE drifted");
static_assert(context::editor::shell::kWmLButtonDown == WM_LBUTTONDOWN, "WM_LBUTTONDOWN drifted");
static_assert(context::editor::shell::kWmLButtonUp == WM_LBUTTONUP, "WM_LBUTTONUP drifted");
static_assert(context::editor::shell::kWmRButtonDown == WM_RBUTTONDOWN, "WM_RBUTTONDOWN drifted");
static_assert(context::editor::shell::kWmRButtonUp == WM_RBUTTONUP, "WM_RBUTTONUP drifted");
static_assert(context::editor::shell::kWmMButtonDown == WM_MBUTTONDOWN, "WM_MBUTTONDOWN drifted");
static_assert(context::editor::shell::kWmMButtonUp == WM_MBUTTONUP, "WM_MBUTTONUP drifted");
static_assert(context::editor::shell::kWmLButtonDblClk == WM_LBUTTONDBLCLK,
              "WM_LBUTTONDBLCLK drifted");
static_assert(context::editor::shell::kWmRButtonDblClk == WM_RBUTTONDBLCLK,
              "WM_RBUTTONDBLCLK drifted");
static_assert(context::editor::shell::kWmMButtonDblClk == WM_MBUTTONDBLCLK,
              "WM_MBUTTONDBLCLK drifted");
static_assert(context::editor::shell::kWmMouseWheel == WM_MOUSEWHEEL, "WM_MOUSEWHEEL drifted");
static_assert(context::editor::shell::kWmMouseHWheel == WM_MOUSEHWHEEL, "WM_MOUSEHWHEEL drifted");
static_assert(context::editor::shell::kWmMouseLeave == WM_MOUSELEAVE, "WM_MOUSELEAVE drifted");
// Guarded because WM_DPICHANGED only exists in an SDK new enough to declare it; comparing against
// the literal instead (as this line first did) is a tautology that pins nothing — it would pass no
// matter what the SDK defines, which is exactly the drift every sibling assert here exists to catch.
#if defined(WM_DPICHANGED)
static_assert(context::editor::shell::kWmDpiChanged == WM_DPICHANGED, "WM_DPICHANGED drifted");
#else
static_assert(context::editor::shell::kWmDpiChanged == 0x02E0, "WM_DPICHANGED drifted");
#endif
static_assert(kSizeMinimized == SIZE_MINIMIZED, "SIZE_MINIMIZED drifted");
static_assert(kWheelDelta == WHEEL_DELTA, "WHEEL_DELTA drifted");
// b1 — the frameless-frame messages and the HT* answers hit_test_frame returns.
static_assert(context::editor::shell::kWmGetMinMaxInfo == WM_GETMINMAXINFO,
              "WM_GETMINMAXINFO drifted");
static_assert(context::editor::shell::kWmNcCalcSize == WM_NCCALCSIZE, "WM_NCCALCSIZE drifted");
static_assert(context::editor::shell::kWmNcHitTest == WM_NCHITTEST, "WM_NCHITTEST drifted");
static_assert(context::editor::shell::kWmNcMouseMove == WM_NCMOUSEMOVE, "WM_NCMOUSEMOVE drifted");
static_assert(context::editor::shell::kWmNcLButtonDown == WM_NCLBUTTONDOWN,
              "WM_NCLBUTTONDOWN drifted");
static_assert(context::editor::shell::kWmNcLButtonUp == WM_NCLBUTTONUP, "WM_NCLBUTTONUP drifted");
static_assert(context::editor::shell::kWmNcLButtonDblClk == WM_NCLBUTTONDBLCLK,
              "WM_NCLBUTTONDBLCLK drifted");
static_assert(context::editor::shell::kWmNcMouseLeave == WM_NCMOUSELEAVE,
              "WM_NCMOUSELEAVE drifted");
static_assert(context::editor::shell::kHtNowhere == HTNOWHERE, "HTNOWHERE drifted");
static_assert(context::editor::shell::kHtClient == HTCLIENT, "HTCLIENT drifted");
static_assert(context::editor::shell::kHtCaption == HTCAPTION, "HTCAPTION drifted");
static_assert(context::editor::shell::kHtMinButton == HTMINBUTTON, "HTMINBUTTON drifted");
static_assert(context::editor::shell::kHtMaxButton == HTMAXBUTTON, "HTMAXBUTTON drifted");
static_assert(context::editor::shell::kHtLeft == HTLEFT, "HTLEFT drifted");
static_assert(context::editor::shell::kHtRight == HTRIGHT, "HTRIGHT drifted");
static_assert(context::editor::shell::kHtTop == HTTOP, "HTTOP drifted");
static_assert(context::editor::shell::kHtTopLeft == HTTOPLEFT, "HTTOPLEFT drifted");
static_assert(context::editor::shell::kHtTopRight == HTTOPRIGHT, "HTTOPRIGHT drifted");
static_assert(context::editor::shell::kHtBottom == HTBOTTOM, "HTBOTTOM drifted");
static_assert(context::editor::shell::kHtBottomLeft == HTBOTTOMLEFT, "HTBOTTOMLEFT drifted");
static_assert(context::editor::shell::kHtBottomRight == HTBOTTOMRIGHT, "HTBOTTOMRIGHT drifted");
static_assert(context::editor::shell::kHtClose == HTCLOSE, "HTCLOSE drifted");
// kDwmwaUseImmersiveDarkMode has no assert on purpose: DWMWA_USE_IMMERSIVE_DARK_MODE is an ENUM
// member of DWMWINDOWATTRIBUTE, not a macro, so `#if defined(...)` (the WM_DPICHANGED precedent for
// an SDK that may lack it) cannot probe for it — and older MinGW dwmapi.h headers genuinely lack
// it. The value 20 is the documented, ABI-frozen one (19 was only ever the pre-release 1809 slot).
} // namespace

#endif // _WIN32

namespace context::editor::shell
{

#if defined(_WIN32)

namespace
{

constexpr const wchar_t* kWindowClassName = L"ContextEditorShellWindow";

// Process-scoped window bookkeeping. Both exist because the pump uses a NULL hwnd filter, so every
// backend drains the WHOLE thread queue and therefore sees messages that belong to its siblings.
//
// `g_live_windows` gates PostQuitMessage: posting it from every WM_DESTROY means closing ONE window
// of a multi-window shell tears down all of them (WindowManager is explicitly built for N windows).
// `g_quit_requested` replaces the old "whoever peeks WM_QUIT declares ITSELF dead" handling, which
// killed an arbitrary window rather than the one that was actually closed — and, because PM_REMOVE
// consumed the message, left the remaining windows never learning about the quit at all.
int g_live_windows = 0;
bool g_quit_requested = false;

// Dynamically-resolved per-monitor-v2 entry points — see the header comment on why.
using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);

struct Win32DpiApi
{
    SetProcessDpiAwarenessContextFn set_process_dpi_awareness_context = nullptr;
    GetDpiForWindowFn get_dpi_for_window = nullptr;
    AdjustWindowRectExForDpiFn adjust_window_rect_ex_for_dpi = nullptr;
};

const Win32DpiApi& win32_dpi_api()
{
    // Resolved once. `LoadLibraryW`, deliberately NOT `GetModuleHandleW`: `context_editor` links
    // with CEF's standard `/DELAYLOAD:user32.dll`, and this resolver runs BEFORE the process has
    // created any window or called any user32 entry point — that early call is its whole purpose
    // (the awareness context must be set before the first window exists). Under delay-load, user32
    // is not resident yet at that moment, so GetModuleHandleW returned null and every entry point
    // below resolved to nullptr — which silently pinned the shell to 96 DPI (measured 2026-08-27: a
    // 150% desktop rendered the editor at 1.0x, with the uncovered band of the window left black).
    // The old "a second reference would leak" concern is moot: user32 never unloads from a GUI
    // process, so the reference this takes changes nothing observable. Pinned by
    // `editor-shell-test_win32_dpi`, which rebuilds the delay-load link condition.
    static const Win32DpiApi api = [] {
        Win32DpiApi resolved;
        HMODULE user32 = ::LoadLibraryW(L"user32.dll");
        if (user32 == nullptr)
        {
            return resolved;
        }
        // The double cast through void(*)() is what silences GCC's -Wcast-function-type on a
        // FARPROC->specific-signature conversion; MSVC accepts either form.
        resolved.set_process_dpi_awareness_context =
            reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                reinterpret_cast<void (*)()>(
                    ::GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        resolved.get_dpi_for_window = reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void (*)()>(::GetProcAddress(user32, "GetDpiForWindow")));
        resolved.adjust_window_rect_ex_for_dpi = reinterpret_cast<AdjustWindowRectExForDpiFn>(
            reinterpret_cast<void (*)()>(::GetProcAddress(user32, "AdjustWindowRectExForDpi")));
        return resolved;
    }();
    return api;
}

// DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is the pseudo-handle -4. Spelled numerically because
// the macro is not declared by every SDK this file compiles against (see the header note).
HANDLE per_monitor_aware_v2_context()
{
    return reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4));
}

// This window's CURRENT dpi through the delay-load-tolerant table above — ONE statement of the
// fallback policy (kReferenceDpi when GetDpiForWindow is unresolved) for every site that needs a
// live answer: create()'s post-create correction and the b1 frame messages, which re-query rather
// than read the tracked dpi_ because both WM_NCCALCSIZE and WM_GETMINMAXINFO are already sent
// INSIDE CreateWindowExW, before create() has computed dpi_ at all.
[[nodiscard]] UINT current_hwnd_dpi(HWND hwnd)
{
    const Win32DpiApi& api = win32_dpi_api();
    return api.get_dpi_for_window != nullptr ? api.get_dpi_for_window(hwnd) : kReferenceDpi;
}

// A SCREEN-relative mouse lParam (the NC family's convention) decoded into the client space every
// other pointer message — and the region map — is expressed in. The per-word sign extension is the
// multi-monitor subtlety (coordinates left/above the primary are negative), stated once for
// WM_NCHITTEST and the NC mouse family alike.
[[nodiscard]] PointI screen_lparam_to_client(HWND hwnd, LPARAM lparam)
{
    POINT point{static_cast<int>(static_cast<std::int16_t>(LOWORD(lparam))),
                static_cast<int>(static_cast<std::int16_t>(HIWORD(lparam)))};
    ::ScreenToClient(hwnd, &point);
    return PointI{point.x, point.y};
}

[[nodiscard]] std::wstring widen(std::string_view utf8)
{
    if (utf8.empty())
    {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                             static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0)
    {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                          needed);
    return wide;
}

[[nodiscard]] std::string narrow(const wchar_t* wide)
{
    if (wide == nullptr || wide[0] == L'\0')
    {
        return {};
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
    {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

[[nodiscard]] Win32ModifierState current_modifier_state()
{
    Win32ModifierState keys;
    // The HIGH bit of GetKeyState is "currently down"; the low bit is the toggle state, which for
    // Shift/Control/Alt is meaningless and for a naive `!= 0` test reads as permanently held.
    keys.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    keys.control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    keys.alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
    keys.meta = ((::GetKeyState(VK_LWIN) & 0x8000) != 0) || ((::GetKeyState(VK_RWIN) & 0x8000) != 0);
    return keys;
}

class Win32WindowBackend final : public IWindowBackend
{
public:
    Win32WindowBackend() = default;

    Win32WindowBackend(const Win32WindowBackend&) = delete;
    Win32WindowBackend& operator=(const Win32WindowBackend&) = delete;

    ~Win32WindowBackend() override
    {
        if (hwnd_ != nullptr)
        {
            ::DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    [[nodiscard]] bool create(const WindowDesc& desc, std::string& error);

    [[nodiscard]] const char* name() const override { return "win32"; }

    [[nodiscard]] render::NativeWindowDesc native_window() const override
    {
        render::NativeWindowDesc native;
        if (hwnd_ == nullptr)
        {
            return native;
        }
        native.kind = render::NativeWindowKind::Win32Hwnd;
        native.handle = hwnd_;
        native.display = ::GetModuleHandleW(nullptr); // HINSTANCE, per rhi.h's Win32Hwnd contract
        return native;
    }

    [[nodiscard]] render::Extent2D client_size() const override { return size_; }
    [[nodiscard]] DpiScale dpi() const override { return dpi_; }
    [[nodiscard]] bool alive() const override { return hwnd_ != nullptr && !g_quit_requested; }

    bool pump(std::vector<ShellEvent>& out) override;
    void request_redraw() override
    {
        if (hwnd_ != nullptr)
        {
            ::InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
    void request_activation() override
    {
        // Best-effort single-instance FOCUS (M9 e14b, D15/C-F23): un-minimize then raise to the
        // foreground. SetForegroundWindow is subject to the OS's focus-stealing rules, so this is
        // honestly best-effort — a no-op window is never worse than a duplicate one. Interactive
        // verification is the deferred interactive-Windows pass (docs/shell.md).
        if (hwnd_ != nullptr)
        {
            if (::IsIconic(hwnd_))
            {
                ::ShowWindow(hwnd_, SW_RESTORE);
            }
            ::SetForegroundWindow(hwnd_);
        }
    }
    void set_title(std::string_view title) override
    {
        if (hwnd_ != nullptr)
        {
            ::SetWindowTextW(hwnd_, widen(title).c_str());
        }
    }

    // a1 (editor-window-chrome): the two chrome verbs. Best-effort asks, like request_activation.
    void minimize() override
    {
        if (hwnd_ != nullptr)
        {
            ::ShowWindow(hwnd_, SW_MINIMIZE);
        }
    }
    void set_maximized(bool maximized) override
    {
        if (hwnd_ == nullptr)
        {
            return;
        }
        // Through the SAME placement machinery apply_placement uses (GetWindowPlacement +
        // SetWindowPlacement, flipping only showCmd): SetWindowPlacement restores AND positions in
        // one call, so a maximize keeps a correct restore rect and an un-maximize returns to it —
        // where a bare ShowWindow(SW_RESTORE) would also un-MINIMIZE, which is not what this verb
        // means.
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        if (::GetWindowPlacement(hwnd_, &wp) == FALSE)
        {
            return;
        }
        wp.showCmd = maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        ::SetWindowPlacement(hwnd_, &wp);
    }

    // b1: the pushed-down chrome facts (window.h § the two chrome FACTS). The regions feed
    // WM_NCHITTEST; both pushes arrive on the single shell thread that also runs the pump, and a
    // modal OS loop (a live caption drag) blocks that thread, so the WndProc can never observe a
    // half-replaced map.
    void set_chrome_regions(const std::vector<ShellRegion>& regions) override
    {
        chrome_regions_.publish(regions);
    }
    void set_appearance(bool dark) override
    {
        appearance_dark_ = dark;
        apply_dark_mode();
    }

    [[nodiscard]] WindowPlacement placement() const override;
    void apply_placement(const WindowPlacement& placement) override;
    void close() override
    {
        if (hwnd_ != nullptr)
        {
            ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
    }

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    // Apply the recorded appearance to the DWM edge tint / drop shadow — the ONE Dwm call this
    // design makes (02 §3). Failure is deliberately ignored: on a Windows too old to know the
    // attribute the frame simply keeps the system tint, which is the pre-b1 behaviour.
    void apply_dark_mode()
    {
        if (hwnd_ != nullptr && appearance_dark_.has_value())
        {
            const BOOL dark = *appearance_dark_ ? TRUE : FALSE;
            (void)::DwmSetWindowAttribute(hwnd_, kDwmwaUseImmersiveDarkMode, &dark, sizeof(dark));
        }
    }

    HWND hwnd_ = nullptr;
    std::vector<ShellEvent> pending_;
    render::Extent2D size_{};
    DpiScale dpi_;
    PointI last_client_pointer_{};
    // b1: the published chrome regions WM_NCHITTEST consults, and the NC forwarding state
    // translate_win32_nc_mouse threads (window.h § Win32NcMouseDecision).
    RegionMap chrome_regions_;
    std::optional<bool> appearance_dark_;
    bool nc_hover_ = false;
    bool nc_pressed_ = false;
    bool tracking_mouse_leave_ = false;
    // b1: whether a TrackMouseEvent(TME_LEAVE | TME_NONCLIENT) request is outstanding. Separate
    // from tracking_mouse_leave_ because the two requests are independent per-type trackers, and
    // WM_NCMOUSELEAVE is never posted without one (the client-only TME_LEAVE does not cover it).
    bool tracking_nc_mouse_leave_ = false;
};

LRESULT CALLBACK Win32WindowBackend::wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
    {
        // Stash `this` before any other message can arrive. WM_NCCREATE is the first message a
        // window receives, so a backend pointer installed here is available to every later one.
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        auto* self = static_cast<Win32WindowBackend*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }
    auto* self =
        reinterpret_cast<Win32WindowBackend*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr)
    {
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }
    return self->handle(hwnd, message, wparam, lparam);
}

LRESULT Win32WindowBackend::handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    const Win32Message raw{static_cast<std::uint32_t>(message), static_cast<std::uint64_t>(wparam),
                           static_cast<std::int64_t>(lparam)};
    std::optional<ShellEvent> decoded = translate_win32_message(raw, current_modifier_state());

    if (decoded.has_value())
    {
        ShellEvent& event = *decoded;
        switch (event.kind)
        {
        case ShellEventKind::resize:
            size_ = event.size;
            break;
        case ShellEventKind::dpi_changed:
            dpi_ = event.dpi;
            break;
        case ShellEventKind::pointer:
            // b1: a left release arriving as a CLIENT message also closes the NC forwarded-press
            // bookkeeping — the user pressed a caption control, dragged into the client area
            // (client messages resumed), and released there.
            if (event.pointer.action == PointerAction::up && event.pointer.button == MouseButton::left)
            {
                nc_pressed_ = false;
            }
            // ...and a client MOVE with the left button reported UP while a forwarded NC press is
            // still outstanding means the release landed OFF this window entirely (the consumed NC
            // press never took capture, so that release was delivered to nobody). Close the
            // browser's phantom press with the up it never received, ordered before the move.
            if (nc_pressed_ && event.pointer.action == PointerAction::move &&
                !event.pointer.modifiers.left_button_down)
            {
                ShellEvent up = event;
                up.pointer.action = PointerAction::up;
                up.pointer.button = MouseButton::left;
                up.pointer.click_count = 1;
                pending_.push_back(up);
                nc_pressed_ = false;
            }
            if (event.pointer.action == PointerAction::wheel)
            {
                // The decoder deliberately reports no position for a wheel (its lParam is SCREEN
                // relative); the last CLIENT position is the coordinate the region map speaks.
                event.pointer.position = last_client_pointer_;
            }
            else if (event.pointer.action != PointerAction::leave)
            {
                last_client_pointer_ = event.pointer.position;
                if (!tracking_mouse_leave_)
                {
                    // WM_MOUSELEAVE is only delivered after it is explicitly requested, and the
                    // request is consumed each time it fires — so re-arm on the next move. Without
                    // it CEF never learns the pointer left and keeps a control hover-highlighted.
                    TRACKMOUSEEVENT track{};
                    track.cbSize = sizeof(track);
                    track.dwFlags = TME_LEAVE;
                    track.hwndTrack = hwnd;
                    tracking_mouse_leave_ = ::TrackMouseEvent(&track) != FALSE;
                }
            }
            else
            {
                tracking_mouse_leave_ = false;
            }
            break;
        default:
            break;
        }
        pending_.push_back(event);
    }

    switch (message)
    {
    // ------------------------------------------------ the frameless frame (b1, 02 §3)
    case WM_NCCALCSIZE:
    {
        // The frame takeover: the (almost) whole window rect becomes client, through the SAME pure
        // inset function the hit-test bands derive from, so the two can never disagree. Both wParam
        // forms address the window rect first — TRUE hands NCCALCSIZE_PARAMS whose rgrc[0] is that
        // rect, FALSE hands the bare RECT, same address either way — so one reinterpret serves
        // both. `IsZoomed` is already true inside a maximize's frame recalcs, which is what routes
        // the all-sides maximized inset (ROADMAP risk 2: no 8px overhang).
        auto* rect = reinterpret_cast<RECT*>(lparam);
        if (rect == nullptr)
        {
            break;
        }
        const Win32FrameInsets insets = win32_frameless_client_insets(
            ::IsZoomed(hwnd) != FALSE, make_dpi_scale(current_hwnd_dpi(hwnd)));
        rect->left += insets.left;
        rect->top += insets.top;
        rect->right -= insets.right;
        rect->bottom -= insets.bottom;
        return 0;
    }
    case WM_GETMINMAXINFO:
    {
        // The maximize geometry that makes the maximized inset above land the client EXACTLY on
        // the work area — and that keeps a maximize onto a secondary monitor correct, since the OS
        // default derives from the primary monitor's size. Windows resolves MonitorFromWindow to
        // the monitor being maximized onto by the time this message is sent for a maximize.
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor_info{};
        monitor_info.cbSize = sizeof(monitor_info);
        if (info == nullptr || monitor == nullptr ||
            ::GetMonitorInfoW(monitor, &monitor_info) == FALSE)
        {
            break; // no monitor to ask: the OS defaults beat a guess
        }
        const Win32MaxGeometry geometry = win32_frameless_max_geometry(
            PointI{monitor_info.rcWork.left - monitor_info.rcMonitor.left,
                   monitor_info.rcWork.top - monitor_info.rcMonitor.top},
            render::Extent2D{
                static_cast<std::uint32_t>(monitor_info.rcWork.right - monitor_info.rcWork.left),
                static_cast<std::uint32_t>(monitor_info.rcWork.bottom - monitor_info.rcWork.top)},
            make_dpi_scale(current_hwnd_dpi(hwnd)));
        info->ptMaxPosition.x = geometry.position.x;
        info->ptMaxPosition.y = geometry.position.y;
        info->ptMaxSize.x = static_cast<LONG>(geometry.size.width);
        info->ptMaxSize.y = static_cast<LONG>(geometry.size.height);
        // ptMin/MaxTrackSize keep the values Windows pre-filled.
        return 0;
    }
    case WM_NCHITTEST:
    {
        // Decided by the pure hit_test_frame over the pushed-down chrome regions (02 §3). The
        // lParam is SCREEN-relative — the one conversion this OS-side arm owns.
        return hit_test_frame(screen_lparam_to_client(hwnd, lparam), size_, dpi_,
                              ::IsZoomed(hwnd) != FALSE, chrome_regions_);
    }
    case WM_NCMOUSEMOVE:
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONUP:
    case WM_NCLBUTTONDBLCLK:
    case WM_NCMOUSELEAVE:
    {
        // The NC mouse family over the web-drawn caption controls, classified by the pure
        // translate_win32_nc_mouse (window.h § Win32NcMouseDecision): forward what keeps the web
        // buttons live, consume what DefWindowProc must not double-act on, and leave the caption
        // and bands to the OS.
        PointI position{};
        if (message != WM_NCMOUSELEAVE)
        {
            position = screen_lparam_to_client(hwnd, lparam);
        }
        const std::int32_t hit =
            message == WM_NCMOUSELEAVE ? kHtNowhere : static_cast<std::int32_t>(wparam);
        if (message == WM_NCMOUSEMOVE && nc_pressed_ &&
            (::GetKeyState(VK_LBUTTON) & 0x8000) == 0)
        {
            // The forwarded press's RELEASE landed off this window: the consumed NC press never
            // took capture, so a release over another window (or the desktop) is delivered to
            // nobody, and the browser is left holding exactly the phantom pressed button the
            // decision table exists to prevent — every later forwarded move would re-assert
            // `left_button_down` from the stale flag. Close it through the same pure table a
            // delivered WM_NCLBUTTONUP takes ("release wherever it lands"), then route this move
            // with the reconciled state.
            const Win32NcMouseDecision closed =
                translate_win32_nc_mouse(kWmNcLButtonUp, hit, position, current_modifier_state(),
                                         nc_hover_, nc_pressed_);
            nc_hover_ = closed.hover;
            nc_pressed_ = closed.pressed;
            if (closed.event.has_value())
            {
                pending_.push_back(*closed.event);
            }
        }
        const Win32NcMouseDecision decision =
            translate_win32_nc_mouse(static_cast<std::uint32_t>(message), hit, position,
                                     current_modifier_state(), nc_hover_, nc_pressed_);
        nc_hover_ = decision.hover;
        nc_pressed_ = decision.pressed;
        if (message == WM_NCMOUSELEAVE)
        {
            tracking_nc_mouse_leave_ = false; // the request is consumed each time it fires
        }
        else if (nc_hover_ && !tracking_nc_mouse_leave_)
        {
            // WM_NCMOUSELEAVE — the message the stuck-hover synthesis keys on — is only ever
            // POSTED after an explicit TrackMouseEvent request carrying TME_NONCLIENT (the
            // kWmMouseLeave re-arm above tracks the CLIENT area only). Without this arm, a pointer
            // flying OFF the window from a hovered caption control never produces the leave, and
            // the web-drawn button stays hover-lit forever (ROADMAP risk 3).
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE | TME_NONCLIENT;
            track.hwndTrack = hwnd;
            tracking_nc_mouse_leave_ = ::TrackMouseEvent(&track) != FALSE;
        }
        if (decision.event.has_value())
        {
            if (decision.event->pointer.action != PointerAction::leave)
            {
                last_client_pointer_ = decision.event->pointer.position;
            }
            pending_.push_back(*decision.event);
        }
        if (decision.consume)
        {
            return 0;
        }
        break;
    }
    case WM_DPICHANGED:
    {
        // Windows supplies the rect the window should occupy on the new monitor. Honouring it is
        // what makes the window keep its APPARENT size when it is dragged across a scaling boundary
        // rather than physically growing or shrinking.
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested != nullptr)
        {
            ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left, suggested->bottom - suggested->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_PAINT:
    {
        // The compositor owns the pixels; validating the region here stops Windows re-posting
        // WM_PAINT forever. The decoded paint_requested event is what actually schedules a frame.
        PAINTSTRUCT ps;
        ::BeginPaint(hwnd, &ps);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        // Claim the erase so Windows does not flash the class background between frames.
        return 1;
    case WM_CLOSE:
        ::DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
        if (g_live_windows > 0)
        {
            --g_live_windows;
        }
        // Only the LAST window ends the loop; otherwise closing one window of a multi-window shell
        // quits the whole editor.
        if (g_live_windows == 0)
        {
            ::PostQuitMessage(0);
        }
        return 0;
    default:
        break;
    }
    // The message's OWN hwnd, never a re-derived one: WM_DESTROY nulls hwnd_, and the WM_NCDESTROY
    // that follows would otherwise be dispatched to whatever window happened to be active — or to
    // NULL in Session 0, where none is — so the window being torn down never gets its own default
    // WM_NCDESTROY handling.
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

bool Win32WindowBackend::create(const WindowDesc& desc, std::string& error)
{
    const Win32DpiApi& api = win32_dpi_api();
    // Per-monitor-v2: the window is told about DPI changes and non-client areas scale too. Failure
    // is not fatal — an older Windows runs at system DPI awareness, and a process whose awareness
    // was already fixed (CEF sets it during CefInitialize too) keeps the value it has.
    (void)win32_apply_per_monitor_dpi_awareness();

    HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    // CS_OWNDC: the CPU present fallback blits through the window's DC (e03's Win32GdiBlitter), and
    // a shared class DC would have its state reset between GetDC calls.
    // CS_DBLCLKS: without it Windows NEVER synthesizes WM_*BUTTONDBLCLK, so the decoder's
    // double-click cases would be unreachable and CEF would only ever see click_count == 1 —
    // silently disabling double-click-to-select-word in every text field of the editor UI.
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC | CS_DBLCLKS;
    wc.lpfnWndProc = &Win32WindowBackend::wnd_proc;
    wc.hInstance = instance;
    // IDC_ARROW is an integer ATOM, not a string — the ANSI/Unicode macro pair differ only in the
    // pointer type they cast it to. This repo does not define UNICODE, so the plain macro is a
    // char* and has to be re-cast for the W entry point.
    wc.hCursor = ::LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = kWindowClassName;
    // Re-registering the same class fails with ERROR_CLASS_ALREADY_EXISTS, which is the normal case
    // for the second window and not an error.
    if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        error = "RegisterClassExW failed (error " + std::to_string(::GetLastError()) + ")";
        return false;
    }

    // b1 (02 §3): the style STAYS the stock WS_OVERLAPPEDWINDOW — Snap, the maximize animation and
    // minimize-to-taskbar all key on it. The mockup-frameless look comes from the WM_NCCALCSIZE /
    // WM_NCHITTEST takeover in handle(), never from stripping style bits. The AdjustWindowRect*
    // below still sizes the OUTER rect as if the stock frame existed; the actual client our
    // NCCALCSIZE then carves is a caption-bar taller than the estimate, which is exactly the band
    // the web titlebar occupies — and the placement restore path applies exact window rects anyway.
    const DWORD style = WS_OVERLAPPEDWINDOW;
    // The window is created at the SYSTEM dpi and then corrected: the real per-monitor DPI is only
    // knowable once the window exists and Windows has decided which monitor it is on.
    UINT initial_dpi = kReferenceDpi;
    render::Extent2D physical = to_physical(desc.logical_size, make_dpi_scale(initial_dpi));
    if (desc.placement.has_value() && !render::is_empty(desc.placement->size()))
    {
        physical = desc.placement->size();
    }

    RECT rect{0, 0, static_cast<LONG>(physical.width), static_cast<LONG>(physical.height)};
    if (api.adjust_window_rect_ex_for_dpi != nullptr)
    {
        (void)api.adjust_window_rect_ex_for_dpi(&rect, style, FALSE, 0, initial_dpi);
    }
    else
    {
        (void)::AdjustWindowRect(&rect, style, FALSE);
    }

    const int x = desc.placement.has_value() ? desc.placement->x : CW_USEDEFAULT;
    const int y = desc.placement.has_value() ? desc.placement->y : CW_USEDEFAULT;

    HWND hwnd = ::CreateWindowExW(0, kWindowClassName, widen(desc.title).c_str(), style, x, y,
                                  rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                  instance, this);
    if (hwnd == nullptr)
    {
        error = "CreateWindowExW failed (error " + std::to_string(::GetLastError()) + ")";
        return false;
    }
    hwnd_ = hwnd;

    dpi_ = make_dpi_scale(current_hwnd_dpi(hwnd_));

    // b1: re-assert a dark-mode choice recorded before this window existed (today the appearance
    // report always arrives after boot, but the ordering must not be load-bearing).
    apply_dark_mode();

    if (desc.visible)
    {
        const bool maximized = desc.placement.has_value() && desc.placement->maximized;
        ::ShowWindow(hwnd_, maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
        ::UpdateWindow(hwnd_);
    }

    // Read the client rect ONCE, after any show: showing (especially maximized) changes it, and
    // nothing between window creation and here reads size_.
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    size_ = render::Extent2D{static_cast<std::uint32_t>(client.right - client.left),
                             static_cast<std::uint32_t>(client.bottom - client.top)};

    error.clear();
    ++g_live_windows;
    return true;
}

bool Win32WindowBackend::pump(std::vector<ShellEvent>& out)
{
    MSG msg;
    // PeekMessage, not GetMessage: the owner loop also drives CEF and the compositor, so it must
    // never block inside the OS queue (03 §1 — the single-threaded owner loop).
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE)
    {
        if (msg.message == WM_QUIT)
        {
            // Process-scoped, NOT "this window died": with a NULL hwnd filter the backend that
            // happens to peek WM_QUIT is arbitrary, so claiming it for this one killed a window
            // that was never closed and hid the quit from its siblings.
            g_quit_requested = true;
            break;
        }
        // TranslateMessage is what turns a WM_KEYDOWN into the WM_CHAR the browser needs for text.
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
    return hwnd_ != nullptr && !g_quit_requested;
}

WindowPlacement Win32WindowBackend::placement() const
{
    WindowPlacement placement;
    if (hwnd_ == nullptr)
    {
        return placement;
    }
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (::GetWindowPlacement(hwnd_, &wp) != FALSE)
    {
        // rcNormalPosition is the RESTORED rect, which is recorded even while maximized — see the
        // WindowPlacement header note on why a maximized window still needs one.
        placement.x = wp.rcNormalPosition.left;
        placement.y = wp.rcNormalPosition.top;
        placement.width =
            static_cast<std::uint32_t>(wp.rcNormalPosition.right - wp.rcNormalPosition.left);
        placement.height =
            static_cast<std::uint32_t>(wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
        placement.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    }
    HMONITOR monitor = ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    if (monitor != nullptr)
    {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (::GetMonitorInfoW(monitor, &info) != FALSE)
        {
            placement.monitor = narrow(info.szDevice);
        }
    }
    return placement;
}

void Win32WindowBackend::apply_placement(const WindowPlacement& placement)
{
    if (hwnd_ == nullptr || render::is_empty(placement.size()))
    {
        return;
    }
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (::GetWindowPlacement(hwnd_, &wp) == FALSE)
    {
        return;
    }
    wp.rcNormalPosition.left = placement.x;
    wp.rcNormalPosition.top = placement.y;
    wp.rcNormalPosition.right = placement.x + static_cast<LONG>(placement.width);
    wp.rcNormalPosition.bottom = placement.y + static_cast<LONG>(placement.height);
    // SetWindowPlacement restores AND positions in one call, so a maximized window keeps a correct
    // restore rect — the reason the placement is applied this way rather than through SetWindowPos.
    wp.showCmd = placement.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    ::SetWindowPlacement(hwnd_, &wp);
}

} // namespace

Win32DpiApiStatus win32_dpi_api_status()
{
    const Win32DpiApi& api = win32_dpi_api();
    Win32DpiApiStatus status;
    status.set_process_dpi_awareness_context = api.set_process_dpi_awareness_context != nullptr;
    status.get_dpi_for_window = api.get_dpi_for_window != nullptr;
    status.adjust_window_rect_ex_for_dpi = api.adjust_window_rect_ex_for_dpi != nullptr;
    return status;
}

bool win32_apply_per_monitor_dpi_awareness()
{
    const Win32DpiApi& api = win32_dpi_api();
    if (api.set_process_dpi_awareness_context == nullptr)
    {
        return false;
    }
    return api.set_process_dpi_awareness_context(per_monitor_aware_v2_context()) != FALSE;
}

std::unique_ptr<IWindowBackend> make_win32_window_backend(const WindowDesc& desc,
                                                          std::string& error)
{
    auto backend = std::make_unique<Win32WindowBackend>();
    if (!backend->create(desc, error))
    {
        return nullptr;
    }
    return backend;
}

#else // !_WIN32

std::unique_ptr<IWindowBackend> make_win32_window_backend(const WindowDesc& /*desc*/,
                                                          std::string& error)
{
    // Compiled everywhere, real only on Windows — the same shape make_win32_gdi_blitter takes, so
    // the off-platform refusal is a value the ctest asserts rather than a symbol that is absent.
    error = "the Win32 window backend is compiled only on Windows";
    return nullptr;
}

#endif // _WIN32

} // namespace context::editor::shell
