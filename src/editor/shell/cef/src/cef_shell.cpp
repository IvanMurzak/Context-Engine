// The windowed-OSR CEF binding — see cef_shell.h for the model and the owner ruling on the
// accelerated path.
//
// This is the ONLY CEF-dependent translation unit in the Shell. The cross-process / headless-boot
// carve-outs (subprocess re-entry, the per-PID root_cache_path, the Session-0 hard exit) mirror
// src/editor/gui/host/src/editor_host.cpp, which boots green on all three OS legs today.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "context/editor/shell/cef/cef_shell.h"

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_render_handler.h"

#if defined(__APPLE__)
#include "include/wrapper/cef_library_loader.h"
#endif

#include <chrono>
#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h> // getpid()
#endif

namespace context::editor::shell::cef
{
namespace
{

namespace present = ::context::render::present;

bool g_initialized = false;

// --------------------------------------------------------------------------- modifier translation

std::uint32_t to_cef_modifiers(const Modifiers& modifiers)
{
    std::uint32_t flags = 0;
    if (modifiers.shift)
    {
        flags |= EVENTFLAG_SHIFT_DOWN;
    }
    if (modifiers.control)
    {
        flags |= EVENTFLAG_CONTROL_DOWN;
    }
    if (modifiers.alt)
    {
        flags |= EVENTFLAG_ALT_DOWN;
    }
    if (modifiers.meta)
    {
        flags |= EVENTFLAG_COMMAND_DOWN;
    }
    // The button flags matter for drag tracking: without them Chromium sees a move with no button
    // held and ends the drag it is in the middle of.
    if (modifiers.left_button_down)
    {
        flags |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    }
    if (modifiers.middle_button_down)
    {
        flags |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    }
    if (modifiers.right_button_down)
    {
        flags |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    }
    return flags;
}

cef_mouse_button_type_t to_cef_button(MouseButton button)
{
    switch (button)
    {
    case MouseButton::right:
        return MBT_RIGHT;
    case MouseButton::middle:
        return MBT_MIDDLE;
    case MouseButton::left:
    case MouseButton::none:
    default:
        return MBT_LEFT;
    }
}

cef_key_event_type_t to_cef_key_type(KeyAction action)
{
    switch (action)
    {
    case KeyAction::key_down:
        return KEYEVENT_KEYDOWN;
    case KeyAction::key_up:
        return KEYEVENT_KEYUP;
    case KeyAction::character:
        return KEYEVENT_CHAR;
    case KeyAction::raw_key_down:
    default:
        return KEYEVENT_RAWKEYDOWN;
    }
}

// ------------------------------------------------------------------------------- the CEF client

// The browser-side client: render handler (OSR frames + the popup), life-span (popup suppression),
// and load handler. It forwards frames into whatever sink the host is currently pumping with.
class ShellCefClient : public CefClient,
                       public CefRenderHandler,
                       public CefLifeSpanHandler,
                       public CefLoadHandler
{
public:
    ShellCefClient(render::Extent2D logical_size, DpiScale dpi)
        : logical_size_(logical_size), dpi_(dpi)
    {
    }

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

    // --- CefRenderHandler --------------------------------------------------------------------
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override
    {
        // VIEW coordinates are DIP. Reporting physical pixels here lays the document out at the
        // wrong size on every non-100% monitor — the bug the spike's DPI-1.0 pin hid.
        rect.Set(0, 0, static_cast<int>(logical_size_.width),
                 static_cast<int>(logical_size_.height));
    }

    bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& screen_info) override
    {
        // The other half of real DPI: the scale CEF multiplies the DIP view rect by to decide how
        // many PHYSICAL pixels to paint. Without it a 2x monitor gets a 1x-resolution UI.
        screen_info.device_scale_factor = dpi_.factor();
        // CefScreenInfo::rect is a RAW cef_rect_t (unlike CefRect it carries no Set()). The screen
        // rect is in screen DEVICE pixels on Windows/Linux and DIP on macOS — the same split CEF
        // documents for GetScreenPoint — so it is derived from the view size accordingly.
#if defined(__APPLE__)
        const render::Extent2D screen = logical_size_;
#else
        const render::Extent2D screen = to_physical(logical_size_, dpi_);
#endif
        screen_info.rect.x = 0;
        screen_info.rect.y = 0;
        screen_info.rect.width = static_cast<int>(screen.width);
        screen_info.rect.height = static_cast<int>(screen.height);
        screen_info.available_rect = screen_info.rect;
        return true;
    }

    void OnPopupShow(CefRefPtr<CefBrowser>, bool show) override
    {
        popup_visible_ = show;
        if (!show)
        {
            popup_rect_ = render::Rect2D{};
        }
        deliver_popup_state();
    }

    void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) override
    {
        // CEF sends the rect and the visibility as separate callbacks with no guaranteed order, so
        // both are held here and the sink is told the COMBINED state — the sink keeps no partial
        // state of its own (see browser.h).
        popup_rect_ = to_rect(rect);
        if (popup_visible_)
        {
            deliver_popup_state();
        }
    }

    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList& dirty_rects,
                 const void* buffer, int width, int height) override
    {
        if (sink_ == nullptr || buffer == nullptr || width <= 0 || height <= 0)
        {
            return;
        }
        BrowserFrame frame;
        frame.layer = type == PET_POPUP ? BrowserLayer::popup : BrowserLayer::view;
        const auto w = static_cast<std::uint32_t>(width);
        const auto h = static_cast<std::uint32_t>(height);
        frame.frame.pixels = buffer;
        frame.frame.bytes_per_row = w * 4u;
        frame.frame.byte_size = static_cast<std::size_t>(frame.frame.bytes_per_row) * h;
        // CEF's OnPaint buffer IS the whole image, so the allocation and the visible area coincide.
        frame.frame.coded_size = render::Extent2D{w, h};
        frame.frame.visible_rect = render::Rect2D{render::Origin2D{}, render::Extent2D{w, h}};
        frame.frame.dirty.reserve(dirty_rects.size());
        for (const CefRect& rect : dirty_rects)
        {
            frame.frame.dirty.push_back(to_rect(rect));
        }
        // Delivered SYNCHRONOUSLY: OnPaint runs inside CefDoMessageLoopWork(), which runs inside
        // pump(), so the sink is live and the buffer is valid — no copy needed. CEF explicitly
        // documents the buffer as valid only for the duration of this call.
        sink_->on_browser_frame(frame);
        ++paint_count_;
    }

    // OnAcceleratedPaint is deliberately NOT overridden: the accelerated path is unreachable by
    // policy (owner ruling 2026-07-19 — see cef_shell.h) and shared_texture_enabled is left off, so
    // CEF never calls it. Overriding it to do nothing would advertise a path that does not exist.

    // --- CefLifeSpanHandler ------------------------------------------------------------------
    bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int /*popup_id*/,
                       const CefString&, const CefString&, WindowOpenDisposition, bool,
                       const CefPopupFeatures&, CefWindowInfo&, CefRefPtr<CefClient>&,
                       CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*) override
    {
        // SUPPRESS every stray window.open (03 §1). Tear-out does NOT ride window.open — it is a
        // PanelHost/Shell mechanism (04 §2) — so a popup reaching here is an accident, and letting
        // CEF create a default popup window would put an un-composited native window on screen.
        ++suppressed_popups_;
        return true;
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override { browser_ = browser; }

    void OnBeforeClose(CefRefPtr<CefBrowser>) override
    {
        browser_ = nullptr;
        closed_ = true;
    }

    // --- CefLoadHandler ----------------------------------------------------------------------
    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override
    {
        if (frame->IsMain())
        {
            load_ended_ = true;
        }
    }

    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, ErrorCode, const CefString&,
                     const CefString&) override
    {
        if (frame->IsMain())
        {
            // A failed load still ENDS the load: a smoke waiting on load_ended would otherwise hang
            // until its timeout on a bad URL rather than reporting it.
            load_ended_ = true;
            load_failed_ = true;
        }
    }

    // --- driving it ---------------------------------------------------------------------------
    void set_sink(IBrowserFrameSink* sink) { sink_ = sink; }
    void set_view(render::Extent2D logical_size, DpiScale dpi)
    {
        logical_size_ = logical_size;
        dpi_ = dpi;
    }

    [[nodiscard]] CefRefPtr<CefBrowser> browser() const { return browser_; }
    [[nodiscard]] bool closed() const { return closed_; }
    [[nodiscard]] bool load_ended() const { return load_ended_; }
    [[nodiscard]] bool load_failed() const { return load_failed_; }
    [[nodiscard]] int paint_count() const { return paint_count_; }
    [[nodiscard]] int suppressed_popups() const { return suppressed_popups_; }

private:
    static render::Rect2D to_rect(const CefRect& rect)
    {
        render::Rect2D out;
        // CEF rects are signed; a negative origin cannot be represented and would wrap. Clamp
        // rather than wrap — the import driver clips against the allocation anyway.
        out.origin.x = rect.x > 0 ? static_cast<std::uint32_t>(rect.x) : 0u;
        out.origin.y = rect.y > 0 ? static_cast<std::uint32_t>(rect.y) : 0u;
        out.size.width = rect.width > 0 ? static_cast<std::uint32_t>(rect.width) : 0u;
        out.size.height = rect.height > 0 ? static_cast<std::uint32_t>(rect.height) : 0u;
        return out;
    }

    void deliver_popup_state()
    {
        if (sink_ != nullptr)
        {
            sink_->on_popup_state(popup_visible_, popup_rect_);
        }
    }

    IBrowserFrameSink* sink_ = nullptr;
    CefRefPtr<CefBrowser> browser_;
    render::Extent2D logical_size_;
    DpiScale dpi_;
    render::Rect2D popup_rect_{};
    bool popup_visible_ = false;
    bool closed_ = false;
    bool load_ended_ = false;
    bool load_failed_ = false;
    int paint_count_ = 0;
    int suppressed_popups_ = 0;

    IMPLEMENT_REFCOUNTING(ShellCefClient);
};

// ---------------------------------------------------------------------------------- the CEF app

// The browser-process app. Its one real job beyond command-line flags is the INTEGRATED PUMP hook:
// with external_message_pump on, CEF asks to be driven via OnScheduleMessagePumpWork instead of
// owning a loop, which is what lets the shell's single thread own frame pacing (03 §1).
class ShellCefApp : public CefApp, public CefBrowserProcessHandler
{
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

    void OnBeforeCommandLineProcessing(const CefString&,
                                       CefRefPtr<CefCommandLine> command_line) override
    {
        // Matches src/editor/gui/host: no sandbox (ContextCef.cmake:91 builds USE_SANDBOX OFF), and
        // the GPU disabled because the editor composites CEF's SOFTWARE OSR output itself — the
        // shipping Windows path per the owner ruling.
        command_line->AppendSwitch("no-sandbox");
        command_line->AppendSwitch("disable-gpu");
        command_line->AppendSwitch("disable-gpu-compositing");
    }

    void OnScheduleMessagePumpWork(int64_t delay_ms) override
    {
        // CEF is asking to be pumped in `delay_ms`. Recorded rather than acted on: the shell's own
        // thread owns the loop, and calling CefDoMessageLoopWork() from here would re-enter it from
        // whatever thread scheduled the work.
        const std::int64_t now = now_ms();
        const std::int64_t clamped = delay_ms < 0 ? 0 : delay_ms;
        due_ms_ = now + clamped;
        scheduled_ = true;
    }

    // True when CEF's scheduled work is due (or none is pending — see the pump-floor note in
    // CefBrowserHostImpl below).
    [[nodiscard]] bool work_is_due()
    {
        if (!scheduled_)
        {
            return false;
        }
        if (now_ms() >= due_ms_)
        {
            scheduled_ = false;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool has_scheduled_work() const { return scheduled_; }

private:
    static std::int64_t now_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    std::int64_t due_ms_ = 0;
    bool scheduled_ = false;

    IMPLEMENT_REFCOUNTING(ShellCefApp);
};

CefRefPtr<ShellCefApp> g_app;

// ------------------------------------------------------------------------------- the host

class CefBrowserHostImpl final : public IBrowserHost
{
public:
    CefBrowserHostImpl(CefRefPtr<ShellCefClient> client, CefRefPtr<ShellCefApp> app)
        : client_(client), app_(app)
    {
    }

    ~CefBrowserHostImpl() override { close(); }

    [[nodiscard]] const char* name() const override { return "cef-windowed-osr"; }

    void resize(render::Extent2D logical_size, DpiScale dpi) override
    {
        client_->set_view(logical_size, dpi);
        CefRefPtr<CefBrowser> browser = client_->browser();
        if (browser == nullptr)
        {
            return;
        }
        // The resize protocol (03 §4): WasResized makes CEF re-read GetViewRect + GetScreenInfo and
        // repaint. Reconfiguring the swapchain without this leaves the browser painting at the old
        // size and the composite sampling a UV sub-rect that no longer matches the window.
        browser->GetHost()->WasResized();
    }

    void send_pointer(const PointerDispatch& dispatch, const PointerEvent& event) override
    {
        CefRefPtr<CefBrowser> browser = client_->browser();
        if (browser == nullptr)
        {
            return;
        }
        CefMouseEvent mouse;
        // DIP, from the arbiter — CEF view coordinates are DIP, not physical pixels.
        mouse.x = dispatch.logical_position.x;
        mouse.y = dispatch.logical_position.y;
        mouse.modifiers = to_cef_modifiers(event.modifiers);

        CefRefPtr<CefBrowserHost> host = browser->GetHost();
        switch (event.action)
        {
        case PointerAction::move:
            host->SendMouseMoveEvent(mouse, /*mouseLeave*/ false);
            break;
        case PointerAction::leave:
            // The explicit leave is what stops a control staying hover-highlighted after the
            // pointer has left the window.
            host->SendMouseMoveEvent(mouse, /*mouseLeave*/ true);
            break;
        case PointerAction::down:
            host->SendMouseClickEvent(mouse, to_cef_button(event.button), /*mouseUp*/ false,
                                      event.click_count);
            break;
        case PointerAction::up:
            host->SendMouseClickEvent(mouse, to_cef_button(event.button), /*mouseUp*/ true,
                                      event.click_count);
            break;
        case PointerAction::wheel:
            host->SendMouseWheelEvent(mouse, event.wheel_delta_x, event.wheel_delta_y);
            break;
        default:
            break;
        }
    }

    void send_key(const KeyEvent& event) override
    {
        CefRefPtr<CefBrowser> browser = client_->browser();
        if (browser == nullptr)
        {
            return;
        }
        CefKeyEvent key;
        key.type = to_cef_key_type(event.action);
        key.modifiers = to_cef_modifiers(event.modifiers);
        key.windows_key_code = event.windows_key_code;
        key.native_key_code = event.native_key_code;
        key.is_system_key = event.is_system_key ? 1 : 0;
        key.character = static_cast<char16_t>(event.character);
        key.unmodified_character = key.character;
        browser->GetHost()->SendKeyEvent(key);
    }

    void set_focus(bool focused) override
    {
        CefRefPtr<CefBrowser> browser = client_->browser();
        if (browser != nullptr)
        {
            browser->GetHost()->SetFocus(focused);
        }
    }

    bool pump(IBrowserFrameSink& sink) override
    {
        // The sink is live only for the duration of this call, which is what lets OnPaint deliver
        // CEF's buffer straight through with no copy.
        client_->set_sink(&sink);
        // The integrated pump. CefDoMessageLoopWork runs when CEF's scheduled work is due; the
        // UNCONDITIONAL floor below runs it anyway when nothing is scheduled, which keeps the
        // browser live if a schedule is ever missed. Both are cheap: DoMessageLoopWork with no work
        // pending returns immediately.
        if (app_ == nullptr || app_->work_is_due() || !app_->has_scheduled_work())
        {
            CefDoMessageLoopWork();
        }
        client_->set_sink(nullptr);
        return !client_->closed();
    }

    void close() override
    {
        CefRefPtr<CefBrowser> browser = client_->browser();
        // Driving the message loop after CefShutdown is undefined behaviour, and a host destroyed
        // during static teardown would do exactly that. Guarding on g_initialized makes a late
        // close a no-op instead.
        if (browser == nullptr || !g_initialized)
        {
            return;
        }
        browser->GetHost()->CloseBrowser(/*force_close*/ true);
        // Pump the close through: OnBeforeClose is what releases the browser reference, and leaving
        // it pending would leak the browser past CefShutdown.
        for (int i = 0; i < 200 && !client_->closed(); ++i)
        {
            CefDoMessageLoopWork();
        }
    }

private:
    CefRefPtr<ShellCefClient> client_;
    CefRefPtr<ShellCefApp> app_;
};

} // namespace

int execute_subprocess(int argc, char** argv)
{
#if defined(__APPLE__)
    // On macOS the framework is LOADED at runtime, never linked, and the helper processes run from
    // their own bundles — so this entry point is not the subprocess path there.
    (void)argc;
    (void)argv;
    return -1;
#else
#if defined(_WIN32)
    CefMainArgs main_args(::GetModuleHandleW(nullptr));
    (void)argc;
    (void)argv;
#else
    CefMainArgs main_args(argc, argv);
#endif
    if (g_app == nullptr)
    {
        g_app = new ShellCefApp;
    }
    return CefExecuteProcess(main_args, g_app.get(), nullptr);
#endif
}

std::unique_ptr<IBrowserHost> make_cef_browser_host(const CefShellOptions& options,
                                                    std::string& error)
{
#if defined(__APPLE__)
    static CefScopedLibraryLoader library_loader;
    static bool library_loaded = library_loader.LoadInMain();
    if (!library_loaded)
    {
        error = "failed to load the CEF framework (LoadInMain)";
        return nullptr;
    }
#endif

    if (g_app == nullptr)
    {
        g_app = new ShellCefApp;
    }

    if (!g_initialized)
    {
#if defined(_WIN32)
        CefMainArgs main_args(::GetModuleHandleW(nullptr));
#else
        CefMainArgs main_args(0, nullptr);
#endif
        CefSettings settings;
        settings.no_sandbox = true;
        settings.windowless_rendering_enabled = true;
        // The single-threaded owner loop (03 §1): CEF does NOT own a thread, and asks to be driven
        // through OnScheduleMessagePumpWork. The design REJECTS the spike's multi-threaded+mutex
        // caveat in favour of this.
        settings.multi_threaded_message_loop = false;
        settings.external_message_pump = true;
        settings.log_severity = LOGSEVERITY_WARNING;
        if (options.devtools_enabled && options.remote_debugging_port > 0)
        {
            // Dev loop ONLY (review B-F11): a naive DevTools pass-through from an OSR browser does
            // not display, so the port is the working route — and an open debugging port in a
            // shipped editor is a security hole, which is why it is off unless asked for twice.
            settings.remote_debugging_port = options.remote_debugging_port;
        }

        // Chromium takes a process-singleton lock on the cache root, so two editors sharing one
        // would deadlock on boot. Per-PID by default (mirrors editor_host.cpp).
        std::error_code ec;
        std::filesystem::path cache = options.cache_root;
        if (cache.empty())
        {
#if defined(_WIN32)
            const long long pid = static_cast<long long>(::GetCurrentProcessId());
#else
            const long long pid = static_cast<long long>(::getpid());
#endif
            cache = std::filesystem::temp_directory_path(ec) /
                    ("context-editor-shell-" + std::to_string(pid));
        }
        std::filesystem::create_directories(cache, ec);
#if defined(_WIN32)
        CefString(&settings.root_cache_path).FromWString(cache.wstring());
#else
        CefString(&settings.root_cache_path).FromString(cache.string());
#endif

        if (!CefInitialize(main_args, settings, g_app.get(), nullptr))
        {
            error = "CefInitialize failed";
            return nullptr;
        }
        g_initialized = true;
    }

    CefRefPtr<ShellCefClient> client(new ShellCefClient(options.logical_size, options.dpi));

    CefWindowInfo window_info;
#if defined(_WIN32)
    // WINDOWED-OSR: the native window OWNS the device context while rendering stays off-screen.
    // A null handle degrades to a fully windowless browser, which is the honest headless config.
    window_info.SetAsWindowless(static_cast<HWND>(options.native_window));
#else
    // The X11/NSView handles are e12's; until then the non-Windows browser is windowless.
    (void)options.native_window;
    window_info.SetAsWindowless(0);
#endif
    // shared_texture_enabled is deliberately LEFT AT ITS DEFAULT (off): the accelerated OSR path is
    // unreachable by policy on Windows per the owner ruling of 2026-07-19, and asking CEF for a
    // shared texture the RHI cannot import would produce frames nothing can composite.
    (void)options.accelerated_osr;

    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate =
        options.windowless_frame_rate > 0 ? options.windowless_frame_rate : 60;

    CefRefPtr<CefBrowser> browser = CefBrowserHost::CreateBrowserSync(
        window_info, client, options.url, browser_settings, nullptr, nullptr);
    if (browser == nullptr)
    {
        error = "CreateBrowserSync failed";
        return nullptr;
    }
    error.clear();
    return std::make_unique<CefBrowserHostImpl>(client, g_app);
}

void shutdown()
{
    if (!g_initialized)
    {
        return;
    }
    g_initialized = false;
    g_app = nullptr;
    CefShutdown();
}

} // namespace context::editor::shell::cef
