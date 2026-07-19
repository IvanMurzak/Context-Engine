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
    // Resolved once. `GetModuleHandleW` rather than LoadLibrary: user32 is already loaded in any
    // process that has a window, and taking a second reference would leak it.
    static const Win32DpiApi api = [] {
        Win32DpiApi resolved;
        HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
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
    void set_title(std::string_view title) override
    {
        if (hwnd_ != nullptr)
        {
            ::SetWindowTextW(hwnd_, widen(title).c_str());
        }
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

    HWND hwnd_ = nullptr;
    std::vector<ShellEvent> pending_;
    render::Extent2D size_{};
    DpiScale dpi_;
    PointI last_client_pointer_{};
    bool tracking_mouse_leave_ = false;
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
    if (api.set_process_dpi_awareness_context != nullptr)
    {
        // Per-monitor-v2: the window is told about DPI changes and non-client areas scale too.
        // Failure is not fatal — an older Windows simply runs at system DPI awareness.
        (void)api.set_process_dpi_awareness_context(per_monitor_aware_v2_context());
    }

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

    if (api.get_dpi_for_window != nullptr)
    {
        initial_dpi = api.get_dpi_for_window(hwnd_);
    }
    dpi_ = make_dpi_scale(initial_dpi);

    RECT client{};
    ::GetClientRect(hwnd_, &client);
    size_ = render::Extent2D{static_cast<std::uint32_t>(client.right - client.left),
                             static_cast<std::uint32_t>(client.bottom - client.top)};

    if (desc.visible)
    {
        const bool maximized = desc.placement.has_value() && desc.placement->maximized;
        ::ShowWindow(hwnd_, maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
        ::UpdateWindow(hwnd_);
        ::GetClientRect(hwnd_, &client);
        size_ = render::Extent2D{static_cast<std::uint32_t>(client.right - client.left),
                                 static_cast<std::uint32_t>(client.bottom - client.top)};
    }
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

WindowBackendSelection make_window_backend(const WindowDesc& desc)
{
    WindowBackendSelection selection;
    auto backend = std::make_unique<Win32WindowBackend>();
    std::string error;
    if (!backend->create(desc, error))
    {
        selection.diagnostic = "the Windows window backend could not create a window: " + error;
        return selection;
    }
    selection.backend = std::move(backend);
    return selection;
}

#else // !_WIN32

WindowBackendSelection make_window_backend(const WindowDesc& /*desc*/)
{
    WindowBackendSelection selection;
#if defined(__APPLE__)
    selection.diagnostic =
        "no native window backend on macOS: the NSWindow/NSView backend lands with e12 "
        "(design 03 §1). The Shell runs headless here; use HeadlessWindowBackend explicitly to "
        "make that choice visible.";
#else
    selection.diagnostic =
        "no native window backend on this platform: the Linux X11/XWayland backend lands with e12 "
        "(design 03 §1, D21). The Shell runs headless here; use HeadlessWindowBackend explicitly "
        "to make that choice visible.";
#endif
    return selection;
}

#endif // _WIN32

} // namespace context::editor::shell
