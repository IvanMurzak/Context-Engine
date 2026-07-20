// `context_editor` — the native Shell's entry point (M9 e04, design 03).
//
// Boots one window, binds a windowed-OSR browser to it, attaches the present path (GPU swapchain, or
// the C-F2 CPU fallback when nothing can present), and runs the SINGLE-THREADED OWNER LOOP: drain
// the OS queue, arbitrate + dispatch input, drive the browser's message work, composite, repeat.
//
// It is default-built on every leg. With CONTEXT_BUILD_GUI_CEF off there is no browser binding to
// link, so the editor boots over the scripted host and presents an empty composite — an honest
// "this build contains no browser" rather than a target that only exists on one CI job. That is also
// what lets the D10 `editor-boundary` job BUILD this binary against the installed client artifacts.

#include "context/editor/shell/app_scheme.h"
#include "context/editor/shell/browser.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/shell.h"
#include "context/editor/shell/window.h"
#include "context/render/rhi.h"

#if defined(CONTEXT_EDITOR_HAS_CEF)
#include "context/editor/shell/cef/cef_shell.h"
#endif

#if defined(CONTEXT_EDITOR_HAS_WGPU)
// The GPU backend is an opt-in dependency path (CONTEXT_BUILD_RENDER_WGPU, default OFF — the
// MSVC/Clang-ABI wgpu-native prebuilt cannot link under the local GCC dev gate). Without it the
// editor has NO way to present on the GPU, which is precisely the case the C-F2 CPU fallback
// exists for — so its absence is a supported configuration, not a broken build.
#include "context/render/wgpu/wgpu_rhi.h"
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace shell = context::editor::shell;
namespace render = context::render;

namespace
{

// editor-core's built asset root, compiled in by CMake (see the note at the target). Always defined
// for a real build; the fallback keeps this file compilable on its own.
#if !defined(CONTEXT_WEBUI_ASSET_DIR)
#define CONTEXT_WEBUI_ASSET_DIR ""
#endif

struct Options
{
    std::filesystem::path project = std::filesystem::current_path();
    // e05c: the editor now boots editor-core over its OWN scheme by default rather than a blank
    // page. `--url` still overrides it (a diagnostic escape hatch), but there is deliberately no
    // `file://` path anywhere: assets ship in-app and are served over context-editor:// (04 §1).
    std::string url = shell::kAppEntryUrl;
    std::filesystem::path app_root = CONTEXT_WEBUI_ASSET_DIR;
    bool headless = false;   // never open an OS window (CI / Session 0 / a remote box)
    bool devtools = false;   // dev-loop only (review B-F11)
    int max_frames = 0;      // 0 == run until the window closes; >0 caps the loop (smoke runs)
};

void print_usage()
{
    std::printf("context_editor — the Context Engine editor shell\n"
                "\n"
                "  --project <dir>   project root (default: the current directory)\n"
                "  --url <url>       the document to load (default: %s)\n"
                "  --app-root <dir>  editor-core's asset root, served over context-editor://app/\n"
                "  --headless        do not open an OS window; run the shell offscreen\n"
                "  --devtools        enable DevTools (dev loop only)\n"
                "  --frames <n>      run at most n loop iterations, then exit\n"
                "  --help            this message\n",
                shell::kAppEntryUrl);
}

// Returns false when the arguments are malformed (the caller exits non-zero).
bool parse_options(int argc, char** argv, Options& out, bool& asked_for_help)
{
    asked_for_help = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool has_value = (i + 1) < argc;
        if (arg == "--help" || arg == "-h")
        {
            asked_for_help = true;
            return true;
        }
        if (arg == "--headless")
        {
            out.headless = true;
        }
        else if (arg == "--devtools")
        {
            out.devtools = true;
        }
        else if (arg == "--project" && has_value)
        {
            out.project = argv[++i];
        }
        else if (arg == "--url" && has_value)
        {
            out.url = argv[++i];
        }
        else if (arg == "--app-root" && has_value)
        {
            out.app_root = argv[++i];
        }
        else if (arg == "--frames" && has_value)
        {
            // strtol, not atoi: atoi cannot distinguish "0" from unparseable input, so
            // `--frames garbage` would silently mean "run zero frames" instead of being rejected.
            const char* text = argv[++i];
            char* end = nullptr;
            const long frames = std::strtol(text, &end, 10);
            if (end == text || *end != '\0' || frames < 0)
            {
                std::fprintf(stderr, "context_editor: --frames expects a non-negative integer, got "
                                     "'%s'\n",
                             text);
                return false;
            }
            out.max_frames = static_cast<int>(frames);
        }
        else
        {
            std::fprintf(stderr, "context_editor: unrecognized argument '%s'\n", arg.c_str());
            return false;
        }
    }
    return true;
}

std::uint64_t now_us()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

int main(int argc, char** argv)
{
#if defined(CONTEXT_EDITOR_HAS_CEF)
    // Subprocess re-entry FIRST, before anything else runs: CEF's renderer/GPU/utility processes
    // re-exec this same binary (non-mac) and must return here immediately. Parsing arguments or
    // touching the filesystem ahead of this would happen once per subprocess.
    const int subprocess_exit = shell::cef::execute_subprocess(argc, argv);
    if (subprocess_exit >= 0)
    {
        return subprocess_exit;
    }
#endif

    Options options;
    bool asked_for_help = false;
    if (!parse_options(argc, argv, options, asked_for_help))
    {
        print_usage();
        return 2;
    }
    if (asked_for_help)
    {
        print_usage();
        return 0;
    }

    // --- the window ------------------------------------------------------------------------------
    shell::WindowDesc window_desc;
    window_desc.title = "Context Editor";
    window_desc.visible = !options.headless;

    std::unique_ptr<shell::IWindowBackend> backend;
    if (options.headless)
    {
        backend = std::make_unique<shell::HeadlessWindowBackend>(window_desc);
    }
    else
    {
        shell::WindowBackendSelection selection = shell::make_window_backend(window_desc);
        if (selection.backend == nullptr)
        {
            // REPORTED, never silent: a platform whose backend is e12's, or a creation failure.
            // Degrading to headless keeps the editor usable (and diagnosable) instead of exiting.
            std::fprintf(stderr, "context_editor: %s\n", selection.diagnostic.c_str());
            backend = std::make_unique<shell::HeadlessWindowBackend>(window_desc);
        }
        else
        {
            backend = std::move(selection.backend);
        }
    }

    // --- the daemon (D10: an ordinary AUTHENTICATED client) ---------------------------------------
    //
    // MOVED AHEAD OF THE BROWSER by e05c, and the order is load-bearing: the attach is where the
    // Shell learns the D20 token and the daemon endpoint, and BOTH must be registered with the
    // bridge's egress guard BEFORE a renderer exists that could ask for them. Attaching after the
    // browser was created would leave a window — however short — in which the guard knew no secrets.
    shell::DaemonAttach daemon = shell::attach_to_project(options.project);
    if (!daemon.attached)
    {
        // Read-only, not fatal (03 §7): the editor opens and reports, and reconnect is the caller's
        // to drive. A shell that refused to start without a daemon could not be used to diagnose why
        // the daemon would not start.
        std::fprintf(stderr, "context_editor: not attached to a daemon (%s%s%s); the editor opens "
                             "read-only\n",
                     daemon.error.c_str(), daemon.error_code.empty() ? "" : "; code=",
                     daemon.error_code.c_str());
    }

    // --- the privileged bridge (e05c, design 04 §1 / 08 §1) ---------------------------------------
    //
    // THE TOKEN-ISOLATION BOUNDARY IS HERE. editor-core gets a router; it does NOT get the client,
    // the socket or the token. The three controls (ipc_bridge.h) are: the router holds no
    // credential by construction, the credential-bearing method names cannot be registered at all,
    // and every outbound response is scanned for the values registered just below.
    //
    // Declared before the browser and destroyed after it: the CEF handler holds a raw pointer to
    // this router for the browser's whole life.
    shell::BridgeRouter bridge;
    if (daemon.client != nullptr)
    {
        // The two values that must never cross into the renderer. Read from the client's own
        // discovery output rather than re-read from disk, so the guard protects exactly what this
        // process is actually holding. (A later reconnect that mints a new token must re-protect it
        // — that path arrives with e05d's client layer.)
        bridge.protect_secret(daemon.client->instance().token);
        bridge.protect_secret(daemon.client->instance().endpoint);
    }
    shell::ShellHandshake handshake(shell::make_handshake_nonce());
    if (!handshake.install(bridge))
    {
        std::fprintf(stderr, "context_editor: could not install the bridge handshake\n");
        return 1;
    }

    // --- the browser ------------------------------------------------------------------------------
    std::unique_ptr<shell::IBrowserHost> browser;
#if defined(CONTEXT_EDITOR_HAS_CEF)
    {
        shell::cef::CefShellOptions cef_options;
        cef_options.native_window = backend->native_window().handle;
        cef_options.logical_size = shell::to_logical(backend->client_size(), backend->dpi());
        cef_options.dpi = backend->dpi();
        cef_options.url = options.url;
        cef_options.devtools_enabled = options.devtools;
        cef_options.app_asset_root = options.app_root;
        cef_options.bridge = &bridge;
        std::string error;
        browser = shell::cef::make_cef_browser_host(cef_options, error);
        if (browser == nullptr)
        {
            std::fprintf(stderr, "context_editor: the browser did not start: %s\n", error.c_str());
            // CEF is already initialized by this point, so returning straight out would leave it
            // initialized and its subprocesses orphaned.
            shell::cef::shutdown();
            return 1;
        }
        std::printf("context_editor: serving editor-core from %s over %s\n",
                    options.app_root.string().c_str(), shell::kAppUrlPrefix);
    }
#else
    // No browser in this build. The shell still boots and composites — see the file header.
    std::fprintf(stderr, "context_editor: built without CEF (CONTEXT_BUILD_GUI_CEF=OFF); running "
                         "the shell with no web content\n");
    browser = std::make_unique<shell::ScriptedBrowserHost>();
#endif

    // --- the window binding + the present path ----------------------------------------------------
    shell::EditorWindowConfig config;
    config.state_index = 0; // window 0 hosts the app menu + welcome screen (D13)
    auto window = std::make_unique<shell::EditorWindow>(std::move(backend), std::move(browser),
                                                        config);

    std::unique_ptr<render::IRhi> rhi;
#if defined(CONTEXT_EDITOR_HAS_WGPU)
    rhi = render::create_wgpu_rhi();
#endif
    if (rhi != nullptr)
    {
        const shell::PresentPath path = window->attach_present(*rhi);
        std::printf("context_editor: present path = %s\n",
                    path == shell::PresentPath::gpu_swapchain ? "gpu-swapchain"
                    : path == shell::PresentPath::cpu_blit    ? "cpu-blit"
                                                              : "none");
    }
    else
    {
        // No RHI at all in this build/configuration — the CPU fallback is the whole point of C-F2.
        window->attach_cpu_present();
        std::printf("context_editor: present path = cpu-blit (no render backend in this build)\n");
    }
    if (!window->diagnostic().empty())
    {
        std::fprintf(stderr, "context_editor: %s\n", window->diagnostic().c_str());
    }

    shell::WindowManager manager(options.project);
    manager.add(std::move(window));

    // --- the owner loop ----------------------------------------------------------------------------
    int frames = 0;
    while (manager.pump_once(now_us()))
    {
        ++frames;
        if (options.max_frames > 0 && frames >= options.max_frames)
        {
            break;
        }
        // A short sleep keeps an idle editor off a spin loop. The compositor is damage-driven, so an
        // idle frame costs nothing beyond this wait; a real vsync-paced loop arrives with the frame
        // scheduler, not here.
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    manager.shutdown();
#if defined(CONTEXT_EDITOR_HAS_CEF)
    shell::cef::shutdown();
#endif
    return 0;
}
