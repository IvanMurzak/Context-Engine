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

#include "context/editor/client/subscription.h"
#include "context/editor/shell/app_scheme.h"
#include "context/editor/shell/browser.h"
#include "context/editor/shell/editor_state_bridge.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/builtin_panels.h"
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
#include <vector>

namespace shell = context::editor::shell;
namespace render = context::render;
// e05d1: the live diagnostics subscription rides the published client SDK, and its handlers receive
// contract::Json payloads — both spelled often enough below to earn an alias.
namespace client = context::editor::client;
namespace contract = context::editor::contract;

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
    //
    // `handshake` is declared BEFORE `bridge` on purpose, even though it is installed after it:
    // `ShellHandshake::install` binds handlers that capture `this`, so the handshake must OUTLIVE
    // the router that holds them. Declaration order here is destruction order reversed, so
    // handshake-then-bridge gives bridge-destroyed-first, which is the required order.
    // --- the window/session manager (03 §1) -------------------------------------------------------
    //
    // Declared HERE, ahead of the bridge, so it OUTLIVES every handler the bridge captures: the
    // editor-state bridge below holds a pointer to this manager's store, and the region sink reaches
    // this manager's window, so the manager must be the longest-lived composition object. Its
    // constructor loads `.editor/editor-state.json` (the Shell is that file's SINGLE writer, 03 §1);
    // the window itself is adopted later, once the browser + present path exist.
    shell::WindowManager manager(options.project);

    shell::ShellHandshake handshake(shell::make_handshake_nonce());
    shell::BridgeRouter bridge;
    if (daemon.client != nullptr)
    {
        // The two values that must never cross into the renderer. Read from the client's own
        // discovery output rather than re-read from disk, so the guard protects exactly what this
        // process is actually holding. (A later reconnect that mints a new token must re-protect it
        // — that path arrives with e05d's client layer.)
        //
        // CHECKED, not fire-and-forget: control 3 is the backstop for the other two, so booting a
        // renderer with an unprotected credential is strictly worse than not booting at all.
        if (!bridge.protect_secret(daemon.client->instance().token) ||
            !bridge.protect_secret(daemon.client->instance().endpoint))
        {
            std::fprintf(stderr,
                         "context_editor: the egress guard refused a daemon credential (empty or "
                         "shorter than the minimum protected length) — refusing to start a "
                         "renderer that could echo it back\n");
            return 1;
        }
    }
    if (!handshake.install(bridge))
    {
        std::fprintf(stderr, "context_editor: could not install the bridge handshake\n");
        return 1;
    }

    // --- the panel surface (e05d1, design 04 §3-§4) -----------------------------------------------
    //
    // The Shell publishes `panel.*` over the SAME privileged bridge: the roster, one panel's uitree
    // render, command + gesture dispatch, and the D6 state pair. editor-core's hydration runtime
    // consumes exactly that and nothing else — it has no daemon socket and no panel knowledge.
    //
    // LIFETIME. The chain is `bridge` -> `panel_host` -> `builtin`: the router holds handlers
    // capturing the host, and the host's providers capture the models `builtin` owns. `builtin` must
    // be declared AFTER `panel_host` (it takes a reference to it), so it is also destroyed FIRST —
    // the reverse of the ownership order. That is safe because teardown is ORDERED EXPLICITLY:
    // `manager.shutdown()` and `shell::cef::shutdown()` run before any of these locals unwind, so
    // the browser and every renderer are gone and no handler can be invoked during teardown. The
    // implicit-destruction order is therefore never load-bearing here — unlike `handshake`/`bridge`
    // above, where it is, because those two carry no such explicit shutdown between them.
    shell::PanelHost panel_host;
    shell::panels::BuiltinPanels builtin = shell::panels::install_builtin_panels(panel_host);
    if (builtin.bound != shell::panels::hostable_panel_ids().size())
    {
        // REPORTED, not fatal: an editor that refused to start because one panel could not bind
        // would be less useful than one that opens with the rest and says which is missing.
        std::fprintf(stderr,
                     "context_editor: only %zu of %zu built-in panels bound; the rest will report "
                     "as unavailable\n",
                     builtin.bound, shell::panels::hostable_panel_ids().size());
    }
    if (!panel_host.install(bridge))
    {
        std::fprintf(stderr, "context_editor: could not install the panel bridge surface\n");
        return 1;
    }

    // --- the editor-state + region-map surface (e05d2, design 03 §1 / §6) --------------------------
    //
    // editor-core publishes its Dockview arrangement + per-panel D6 blobs, and its viewport/native
    // region rects, over the SAME privileged bridge. The Shell records the former into the
    // editor-state store — of which it is the SINGLE writer (03 §1) — and routes the latter into the
    // window's input arbiter (03 §6). On boot editor-core reads the persisted blob back through
    // `editor.state.get` and rebuilds the arrangement itself; this bridge never opens that file.
    //
    // The store is bound now (it exists); the region SINK reaches the window through the manager at
    // call time, because the window is adopted a few lines below. A publish that somehow arrived
    // before the window existed would find `manager.window(0) == nullptr` and be dropped rather than
    // crash — but the renderer boots only after all of this is wired.
    shell::EditorStateBridge editor_state_bridge;
    editor_state_bridge.bind_store(&manager.state_store(), now_us);
    editor_state_bridge.bind_regions(
        [&manager](std::vector<shell::ShellRegion> regions)
        {
            // `target_window`, not `window`: the owner loop declares its own `window` local further
            // down, and the CEF-ON `context_editor` leg compiles under CEF's /W4 /WX whose C4456
            // (declaration shadows a local) is invisible to the local GCC gate — a distinct name
            // keeps that diagnostic out of reach entirely.
            if (shell::EditorWindow* target_window = manager.window(0))
            {
                target_window->input().regions().publish(std::move(regions));
            }
        });
    if (!editor_state_bridge.install(bridge))
    {
        std::fprintf(stderr, "context_editor: could not install the editor-state bridge surface\n");
        return 1;
    }

    // --- the LIVE diagnostics subscription (the Problems read path) -------------------------------
    //
    // The Shell subscribes as an ordinary client (D10) and forwards what arrives into the panel
    // models. Without a daemon there is simply no feed — the editor opens read-only and Problems is
    // empty, which is the honest state rather than a failure.
    //
    // SINGLE-THREADED, DELIBERATELY, and this is the trade worth naming. `SubscriptionConsumer` is
    // documented as blocking for up to `poll_timeout_ms` per pump and, on a dropped wire, for the
    // whole reconnect ladder — its own header says not to drive it from a thread that must stay
    // responsive. The alternative to a thread is what is done here: `poll_timeout_ms = 0` (a
    // non-blocking poll) and a deliberately SHORT reconnect ladder, pumped from the owner loop.
    //
    // Why not a thread: the feed mutates the panel models, and the bridge handlers that RENDER those
    // models run on this same owner/UI thread. A background pump would be a data race on every
    // panel model, requiring a lock around the whole PanelHost — which is a real design (and may
    // become the right one when panels get heavier), but it is strictly more machinery than e05d1
    // needs and strictly more ways to be wrong. The cost of the choice made here is bounded: a
    // daemon restart can stall the loop for the ladder's duration, which the tuning below caps at
    // roughly a second rather than the ~21 s the defaults would allow.
    std::unique_ptr<client::SubscriptionConsumer> diagnostics;
    if (daemon.client != nullptr && builtin.problems != nullptr)
    {
        client::SubscriptionOptions subscription_options;
        subscription_options.poll_timeout_ms = 0; // never block the owner loop on a quiet stream
        subscription_options.reconnect_timeout_ms = 250;
        subscription_options.backoff.initial_ms = 50;
        subscription_options.backoff.max_ms = 250;
        subscription_options.backoff.max_attempts = 3;

        diagnostics = std::make_unique<client::SubscriptionConsumer>(
            *daemon.client, shell::make_shell_attach_options(), subscription_options);

        // `feed` stays a POINTER TO THE FORWARD-DECLARED type — this TU never sees `ProblemsFeed`'s
        // complete definition (builtin_panels.h's comment on the forward declare explains why: this
        // executable is compiled `-fno-rtti` when CEF is on, and `problems_feed.h`'s full include
        // chain reaches a kernel header whose templated methods use `typeid`). The lambdas below
        // drive it through `apply_problems_snapshot`/`apply_problems_event`, the non-member seams
        // builtin_panels.h/.cpp expose for exactly this caller.
        shell::panels::ProblemsFeed* feed = builtin.problems.get();
        // 0 is only the FALLBACK stamp for a snapshot that carries no `generation` of its own; the
        // real cursor snapshot always does, and `apply_snapshot` prefers it. Passing 0 as the
        // authoritative stamp would mark every recovered provisional diagnostic stale-by-
        // construction, since the stream never settles below generation 1.
        diagnostics->on_snapshot(
            [feed](const std::string&, const contract::Json& snapshot)
            { shell::panels::apply_problems_snapshot(*feed, snapshot, 0); });
        diagnostics->on_event(
            [feed](const std::string&, const client::ClientEvent& event)
            { (void)shell::panels::apply_problems_event(*feed, event.topic, event.payload,
                                                        event.generation); });

        client::SubscriptionSpec spec;
        spec.topics = {shell::panels::kDiagnosticsTopic, shell::panels::kDerivationTopic};
        (void)diagnostics->add(spec);

        std::string subscribe_error;
        if (!diagnostics->start(subscribe_error))
        {
            // Same posture as a failed attach: reported, non-fatal, and the editor still opens.
            std::fprintf(stderr,
                         "context_editor: could not subscribe to the diagnostics stream (%s); the "
                         "Problems panel will stay empty\n",
                         subscribe_error.c_str());
            diagnostics.reset();
        }
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

    // The window is adopted now that its browser + present path exist; the manager applies any
    // remembered placement before the first frame (it was constructed far above so it could outlive
    // the bridge handlers — see there).
    manager.add(std::move(window));

    // --- the owner loop ----------------------------------------------------------------------------
    int frames = 0;
    while (manager.pump_once(now_us()))
    {
        ++frames;
        if (diagnostics != nullptr)
        {
            // One non-blocking drain per frame. A pump failure here is UNRECOVERABLE by the
            // consumer's own definition (the backoff ladder was exhausted, or the daemon REFUSED a
            // re-subscribe), so the feed is dropped rather than retried forever: an editor silently
            // burning a pump every frame against a daemon that will never answer is worse than one
            // that stops and says so.
            std::string pump_error;
            if (!diagnostics->pump(pump_error))
            {
                std::fprintf(stderr,
                             "context_editor: the diagnostics subscription stopped (%s); the "
                             "Problems panel will no longer update\n",
                             pump_error.c_str());
                diagnostics.reset();
            }
        }
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
