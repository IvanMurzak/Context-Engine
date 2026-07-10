// src/editor/cef/src/cef_boot_smoke.cpp — M5-F0a CEF build-substrate boot smoke (issue #150).
//
// The MINIMAL proof that the pinned, SHA-verified CEF prebuilt (tools/fetch_cef.py) fetches, links,
// and BOOTS: initialize CEF headless (windowless / off-screen rendering, no GPU, no sandbox), create
// a real browser (which spawns the CEF render subprocess), load about:blank, observe the load end,
// then shut down cleanly and exit 0. It renders nothing and asserts no pixels — this is a
// build-substrate smoke, NOT the editor host (no panels, no RPC, no R-EDIT-001; those are M5-F0b).
//
// Cross-process model: CEF re-invokes THIS executable for its renderer/GPU/utility subprocesses.
// CefExecuteProcess() returns >= 0 in a subprocess (we return that immediately); the browser process
// falls through to CefInitialize(). On macOS the Chromium Embedded Framework is loaded dynamically
// via CefScopedLibraryLoader (the framework is not linked at build time), and the subprocesses use a
// dedicated helper bundle (cef_boot_smoke_helper_mac.cpp) — so this file's mac path is the browser
// process only.
//
// Headless CI: the windowless (OSR) path needs no OS window. On Linux the `cef-substrate` CI job runs
// this under xvfb (CEF's default ozone/X11 backend still wants a display connection to initialize);
// GPU is disabled so no adapter is required. On the Session-0 self-hosted Windows runner we _Exit()
// after a successful boot to skip CEF's occasionally-flaky process teardown (the same Session-0
// carve-out src/render/ documents for wgpu-native).

#if defined(_WIN32)
// windows.h first (before the CEF headers) for HINSTANCE / ::GetModuleHandle, matching the spike's
// include order. CEF's cef_variables define WIN32_LEAN_AND_MEAN + NOMINMAX for us.
#include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_render_handler.h"
#include "include/wrapper/cef_helpers.h"

#if defined(__APPLE__)
#include "include/wrapper/cef_library_loader.h"
#endif

namespace {

// Boot outcome shared between the CEF callbacks and main(). Single-threaded message pump
// (multi_threaded_message_loop = false), so no synchronization is needed.
struct BootState {
    bool created = false;    // OnAfterCreated fired — the browser + its subprocess booted
    bool load_ended = false; // OnLoadEnd fired on the main frame — the boot round-trip completed
    bool closed = false;     // OnBeforeClose fired — teardown acknowledged
};

BootState g_state;

// Windowless client: the smallest CefClient that drives an off-screen browser to a loaded state.
class BootClient : public CefClient,
                   public CefRenderHandler,
                   public CefLifeSpanHandler,
                   public CefLoadHandler {
public:
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

    // OSR requires a view rect; a small fixed size is fine (nothing is rasterized/asserted).
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect.Set(0, 0, 640, 480); }

    // OSR delivers painted frames here — a no-op for the boot smoke (we never read pixels).
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int,
                 int) override {}

    void OnAfterCreated(CefRefPtr<CefBrowser>) override { g_state.created = true; }

    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override {
        if (frame->IsMain()) {
            g_state.load_ended = true;
        }
    }

    // about:blank does not error, but if it ever did we still treat the browser as booted.
    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, ErrorCode, const CefString&,
                     const CefString&) override {
        if (frame->IsMain()) {
            g_state.load_ended = true;
        }
    }

    void OnBeforeClose(CefRefPtr<CefBrowser>) override { g_state.closed = true; }

private:
    IMPLEMENT_REFCOUNTING(BootClient);
};

// App: force GPU/hardware off so the smoke boots on any headless CI box with no adapter.
class BootApp : public CefApp, public CefBrowserProcessHandler {
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

    void OnBeforeCommandLineProcessing(const CefString&,
                                       CefRefPtr<CefCommandLine> command_line) override {
        command_line->AppendSwitch("disable-gpu");
        command_line->AppendSwitch("disable-gpu-compositing");
        command_line->AppendSwitch("disable-software-rasterizer");
        command_line->AppendSwitch("no-sandbox");
    }

private:
    IMPLEMENT_REFCOUNTING(BootApp);
};

// Pump the single-threaded CEF loop until `done` or a wall-clock deadline. Returns true if `done`
// became true before the deadline.
template <typename Pred>
bool pump_until(Pred done, std::chrono::seconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        CefDoMessageLoopWork();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
#if defined(__APPLE__)
    // Load the Chromium Embedded Framework from Contents/Frameworks (browser process).
    CefScopedLibraryLoader library_loader;
    if (!library_loader.LoadInMain()) {
        std::fprintf(stderr, "[cef-boot] failed to load the CEF framework (LoadInMain)\n");
        return 1;
    }
#endif

#if defined(_WIN32)
    CefMainArgs main_args(::GetModuleHandle(nullptr));
#else
    CefMainArgs main_args(argc, argv);
#endif

    CefRefPtr<BootApp> app(new BootApp);

    // Subprocess re-entry: renderer/GPU/utility processes reuse this exe (non-mac) and return here.
    const int exit_code = CefExecuteProcess(main_args, app.get(), nullptr);
    if (exit_code >= 0) {
        return exit_code;
    }

    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;
    settings.multi_threaded_message_loop = false;
    settings.log_severity = LOGSEVERITY_WARNING;

    if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
        std::fprintf(stderr, "[cef-boot] CefInitialize failed\n");
        return 1;
    }

    CefWindowInfo window_info;
    window_info.SetAsWindowless(0); // off-screen: no OS window, headless-friendly
    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = 1;

    CefRefPtr<BootClient> client(new BootClient);
    CefRefPtr<CefBrowser> browser = CefBrowserHost::CreateBrowserSync(
        window_info, client, "about:blank", browser_settings, nullptr, nullptr);
    if (!browser) {
        std::fprintf(stderr, "[cef-boot] CreateBrowserSync failed\n");
        CefShutdown();
        return 1;
    }

    // Boot is proven once the browser was created AND its main frame finished loading.
    const bool booted = pump_until(
        [] { return g_state.created && g_state.load_ended; }, std::chrono::seconds(30));
    if (!booted) {
        std::fprintf(stderr, "[cef-boot] browser did not boot within 30s (created=%d load=%d)\n",
                     static_cast<int>(g_state.created), static_cast<int>(g_state.load_ended));
        // Best-effort teardown before the failing exit.
        browser->GetHost()->CloseBrowser(true);
        pump_until([] { return g_state.closed; }, std::chrono::seconds(5));
        CefShutdown();
        return 1;
    }

    std::fprintf(stderr, "[cef-boot] CEF browser booted headless (about:blank loaded)\n");

    // Orderly teardown: close the browser, pump until OnBeforeClose, then shut CEF down.
    browser->GetHost()->CloseBrowser(true);
    pump_until([] { return g_state.closed; }, std::chrono::seconds(10));
    browser = nullptr;
    client = nullptr;

#if defined(_WIN32)
    // Session-0 self-hosted Windows runner carve-out (mirrors src/render/ wgpu-native): CEF's native
    // process teardown is occasionally flaky under LocalSystem/Session-0. The boot is already proven,
    // so hard-exit success and skip the teardown crash surface. CefShutdown() is best-effort first.
    CefShutdown();
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(0);
#else
    CefShutdown();
    return 0;
#endif
}
