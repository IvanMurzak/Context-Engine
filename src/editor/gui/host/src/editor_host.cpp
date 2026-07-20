// src/editor/gui/host/src/editor_host.cpp — M5-F0b CEF editor host boot smoke (issue #152).
//
// The editor HOST that the F0a build substrate (src/editor/cef/) exists for. It proves the M5-F0b
// foundation end-to-end on each desktop OS: boot CEF headless, then
//   1. build the built-in PLACEHOLDER PANEL from the headless UI-logic tree (context_gui_uitree) and
//      REGISTER it through the R-EDIT-001 extension contract (context_gui_contract) — every built-in
//      panel is built ON the extension contract from day one;
//   2. attach to the bridge as a client through the CAPABILITY-SCOPED bridge shim (ExtensionBridge
//      over the ONE dispatcher) at the R-SEC-007 read/query baseline, RUN ONE COMMAND (`describe`),
//      and confirm a build/install verb is scope-DENIED (no ambient elevation);
//   3. select the L-41 compositing surface mode for this platform (context_gui_compositor);
//   4. RENDER the placeholder panel: write its semantic HTML (ARIA roles/labels — R-A11Y-001) to a
//      temp document (with the sandbox CSP as a <meta> header) and load it headless, observing the
//      load end;
// then tear down and exit 0.
//
// Like src/editor/cef/ this is a CI-ONLY dependency path (the MSVC/Clang-ABI CEF prebuilt cannot link
// under the local Strawberry-GCC dev gate); it is built + booted ONLY behind CONTEXT_BUILD_GUI_CEF by
// the per-OS `editor-cef-smoke` CI job. The three headless libs it links (uitree/contract/compositor)
// are unit-tested WITHOUT CEF on the default matrix; this smoke proves they compose behind CEF.
//
// The cross-process / mac model + the headless-boot carve-outs mirror cef_boot_smoke.cpp verbatim.

#if defined(_WIN32)
#include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <variant>

#if !defined(_WIN32)
#include <unistd.h> // getpid()
#endif

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_render_handler.h"
#include "include/wrapper/cef_helpers.h"

#if defined(__APPLE__)
#include "include/wrapper/cef_library_loader.h"
#endif

#include "context/editor/gui/compositor/surface.h"
#include "context/editor/gui/contract/builtin_roster.h"
#include "context/editor/gui/contract/extension.h"
#include "context/editor/gui/contract/registry.h"
#include "context/editor/gui/contract/sandbox.h"
#include "context/editor/gui/contract/shim.h"
#include "context/editor/gui/uitree/builtin.h"
#include "context/editor/gui/uitree/node.h"
#include "context/editor/gui/uitree/panel.h"

#include "context/editor/bridge/dispatcher.h"
#include "context/editor/bridge/scope.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/contract/json.h"

namespace uitree = context::editor::gui::uitree;
namespace guic = context::editor::gui::contract;
namespace compositor = context::editor::gui::compositor;
namespace bridge = context::editor::bridge;
namespace cli = context::editor::contract;

namespace
{

struct BootState
{
    bool created = false;
    bool load_ended = false;
    bool closed = false;
};

BootState g_state;

class HostClient : public CefClient,
                   public CefRenderHandler,
                   public CefLifeSpanHandler,
                   public CefLoadHandler
{
public:
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect.Set(0, 0, 1280, 800); }
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int,
                 int) override
    {
    }
    void OnAfterCreated(CefRefPtr<CefBrowser>) override { g_state.created = true; }
    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override
    {
        if (frame->IsMain())
        {
            g_state.load_ended = true;
        }
    }
    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, ErrorCode, const CefString&,
                     const CefString&) override
    {
        if (frame->IsMain())
        {
            g_state.load_ended = true;
        }
    }
    void OnBeforeClose(CefRefPtr<CefBrowser>) override { g_state.closed = true; }

private:
    IMPLEMENT_REFCOUNTING(HostClient);
};

class HostApp : public CefApp, public CefBrowserProcessHandler
{
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    void OnBeforeCommandLineProcessing(const CefString&,
                                       CefRefPtr<CefCommandLine> command_line) override
    {
        command_line->AppendSwitch("disable-gpu");
        command_line->AppendSwitch("disable-gpu-compositing");
        command_line->AppendSwitch("disable-software-rasterizer");
        command_line->AppendSwitch("no-sandbox");
    }

private:
    IMPLEMENT_REFCOUNTING(HostApp);
};

template <typename Pred>
bool pump_until(Pred done, std::chrono::seconds budget)
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!done())
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }
        CefDoMessageLoopWork();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

const char* mode_token(compositor::CompositingMode mode)
{
    switch (mode)
    {
    case compositor::CompositingMode::accelerated_osr:
        return "accelerated-osr";
    case compositor::CompositingMode::software_osr:
        return "software-osr";
    case compositor::CompositingMode::iosurface:
        return "iosurface";
    }
    return "software-osr";
}

// The HEADLESS half of the host: register the placeholder panel through the R-EDIT-001 contract,
// attach the capability-scoped bridge, run one command, and select the L-41 surface. All CEF-free —
// so it is ALSO exercised by the default-matrix ctests; here it runs inside the real host process.
// Fills `panel_html` with the document to render. Returns true on success.
bool run_host_contract(std::string& panel_html)
{
    // 1. Build + register the built-in placeholder panel through the R-EDIT-001 extension contract.
    uitree::Panel panel = uitree::make_placeholder_panel();
    if (!uitree::audit_a11y(panel).empty())
    {
        std::fprintf(stderr, "[editor-host] placeholder panel failed the a11y audit\n");
        return false;
    }

    // M9 e05b — the SINGLE GLOBAL ROSTER. This used to be a stack-local registry carrying one
    // hand-built placeholder contribution, which could (and did) drift from the a11y scan list. The
    // host now builds the roster from contract::builtin_contributions() — the one authoritative
    // manifest-v2 list — so the panels the host knows about and the panels the a11y gate scans are the
    // same set by construction. Deny-by-default is unchanged: every built-in goes through
    // register_contribution(), so a built-in violating a contract invariant is REFUSED here.
    bool roster_ok = false;
    const guic::ExtensionRegistry registry = guic::make_builtin_registry(&roster_ok);
    if (!roster_ok || registry.size() != guic::builtin_contributions().size())
    {
        std::fprintf(stderr, "[editor-host] built-in roster failed to register (%zu of %zu)\n",
                     registry.size(), guic::builtin_contributions().size());
        return false;
    }
    const guic::Contribution* contribution = registry.find("placeholder");
    if (contribution == nullptr)
    {
        std::fprintf(stderr, "[editor-host] the placeholder panel is not on the built-in roster\n");
        return false;
    }
    // The non-negotiable renderer trust boundary still holds for the panel this host renders.
    if (!guic::sandbox_conformant(contribution->sandbox))
    {
        std::fprintf(stderr, "[editor-host] built-in sandbox policy is not conformant\n");
        return false;
    }
    std::fprintf(stderr, "[editor-host] built-in roster registered (%zu contributions)\n",
                 registry.size());

    // 2. Attach the capability-scoped bridge shim at the read/query baseline and RUN ONE COMMAND.
    bridge::Dispatcher dispatcher;
    cli::ClientHandshake handshake;
    handshake.protocol_major = cli::kProtocolMajor;
    handshake.capabilities = {"describe"};
    auto attached = guic::ExtensionBridge::attach(dispatcher, handshake, contribution->sandbox);
    if (!std::holds_alternative<guic::ExtensionBridge>(attached))
    {
        std::fprintf(stderr, "[editor-host] bridge attach hard-failed\n");
        return false;
    }
    const guic::ExtensionBridge& shim = std::get<guic::ExtensionBridge>(attached);
    const cli::Envelope described = shim.invoke("describe", cli::Json::object());
    if (!described.ok())
    {
        std::fprintf(stderr, "[editor-host] `describe` bridge command failed\n");
        return false;
    }
    // The capability-scoping is real: a build/install verb is DENIED (no ambient elevation).
    const cli::Envelope install = shim.invoke("package.add", cli::Json::object());
    if (install.ok() || !install.error().has_value() ||
        install.error()->code != bridge::kScopeDeniedCode)
    {
        std::fprintf(stderr, "[editor-host] capability scoping broken: package.add was not denied\n");
        return false;
    }

    // 3. Select the L-41 compositing surface mode for this platform (advisory probe).
    compositor::SurfaceCapabilities caps; // GPU disabled in this smoke; the seam still selects a mode
    caps.gpu_compositing = false;
    const compositor::SurfaceHandoff handoff =
        compositor::make_handoff(compositor::current_platform(), caps);
    std::fprintf(stderr, "[editor-host] L-41 surface mode = %s (external_begin_frame=%d)\n",
                 mode_token(handoff.mode), static_cast<int>(handoff.external_begin_frame));

    // 4. Compose the render document: semantic-HTML panel body + the sandbox CSP as a <meta> header.
    // Composed through uitree::render_document so EVERY interpolation — the CSP and the title, not
    // just the body — goes through the C-F6 escaping contract (this site used to concatenate the CSP
    // raw). See node.h § the C-F6 escaping contract.
    panel_html = uitree::render_document(panel, contribution->sandbox.csp);
    std::fprintf(stderr, "[editor-host] placeholder panel composed (%zu bytes of HTML)\n",
                 panel_html.size());
    return true;
}

// Build a file:// URL for a local path (forward slashes; three leading slashes for a POSIX absolute
// path, `file:///C:/...` on Windows).
std::string file_url(const std::filesystem::path& p)
{
    const std::string generic = p.generic_string();
    if (!generic.empty() && generic.front() == '/')
    {
        return "file://" + generic; // /tmp/x -> file:///tmp/x
    }
    return "file:///" + generic; // C:/Users/x -> file:///C:/Users/x
}

} // namespace

int main(int argc, char* argv[])
{
#if defined(__APPLE__)
    CefScopedLibraryLoader library_loader;
    if (!library_loader.LoadInMain())
    {
        std::fprintf(stderr, "[editor-host] failed to load the CEF framework (LoadInMain)\n");
        return 1;
    }
#endif

#if defined(_WIN32)
    CefMainArgs main_args(::GetModuleHandle(nullptr));
#else
    CefMainArgs main_args(argc, argv);
#endif

    CefRefPtr<HostApp> app(new HostApp);

    // Subprocess re-entry: renderer/GPU/utility processes reuse this exe (non-mac) and return here.
    const int exit_code = CefExecuteProcess(main_args, app.get(), nullptr);
    if (exit_code >= 0)
    {
        return exit_code;
    }

    // Per-PID writable cache root (Chromium process-singleton lock — mirrors cef_boot_smoke.cpp).
#if defined(_WIN32)
    const long long boot_pid = static_cast<long long>(::GetCurrentProcessId());
#else
    const long long boot_pid = static_cast<long long>(::getpid());
#endif
    std::error_code cache_ec;
    const std::filesystem::path cache_dir =
        std::filesystem::temp_directory_path(cache_ec) /
        ("context-editor-host-" + std::to_string(boot_pid));
    std::filesystem::create_directories(cache_dir, cache_ec);

    // Run the headless host contract (panel registration + capability-scoped bridge command + L-41
    // selection) BEFORE booting the browser. A failure here is a host-contract regression, not a CEF
    // boot failure.
    std::string panel_html;
    if (!run_host_contract(panel_html))
    {
        return 1;
    }

    // Write the placeholder document next to the cache dir and prepare its file:// URL.
    const std::filesystem::path html_path = cache_dir / "placeholder.html";
    {
        std::ofstream out(html_path, std::ios::binary);
        if (!out)
        {
            std::fprintf(stderr, "[editor-host] failed to write the placeholder document\n");
            return 1;
        }
        out << panel_html;
    }
    const std::string panel_url = file_url(html_path);

    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;
    settings.multi_threaded_message_loop = false;
    settings.log_severity = LOGSEVERITY_WARNING;
#if defined(_WIN32)
    CefString(&settings.root_cache_path).FromWString(cache_dir.wstring());
#else
    CefString(&settings.root_cache_path).FromString(cache_dir.string());
#endif

    if (!CefInitialize(main_args, settings, app.get(), nullptr))
    {
        std::fprintf(stderr, "[editor-host] CefInitialize failed\n");
        return 1;
    }

    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);
    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = 1;

    CefRefPtr<HostClient> client(new HostClient);
    CefRefPtr<CefBrowser> browser = CefBrowserHost::CreateBrowserSync(
        window_info, client, panel_url, browser_settings, nullptr, nullptr);
    if (!browser)
    {
        std::fprintf(stderr, "[editor-host] CreateBrowserSync failed\n");
        CefShutdown();
        return 1;
    }

    // Rendered once the browser was created AND the placeholder panel finished loading.
    const bool rendered = pump_until(
        [] { return g_state.created && g_state.load_ended; }, std::chrono::seconds(30));
    if (!rendered)
    {
        std::fprintf(stderr, "[editor-host] panel did not render within 30s (created=%d load=%d)\n",
                     static_cast<int>(g_state.created), static_cast<int>(g_state.load_ended));
        browser->GetHost()->CloseBrowser(true);
        pump_until([] { return g_state.closed; }, std::chrono::seconds(5));
        CefShutdown();
        return 1;
    }

    std::fprintf(stderr, "[editor-host] editor host booted; placeholder panel rendered; "
                         "one bridge command executed\n");

    browser->GetHost()->CloseBrowser(true);
    pump_until([] { return g_state.closed; }, std::chrono::seconds(10));
    browser = nullptr;
    client = nullptr;

#if defined(_WIN32)
    // Session-0 self-hosted Windows runner carve-out (mirrors cef_boot_smoke.cpp / src/render/).
    CefShutdown();
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(0);
#else
    CefShutdown();
    return 0;
#endif
}
