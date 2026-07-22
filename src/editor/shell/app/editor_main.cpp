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

#include "context/editor/client/arbitration.h" // e14b: the D15/C-F23 presence marker + focus watcher
#include "context/editor/client/subscription.h"
#include "context/editor/shell/app_scheme.h"
#include "context/editor/shell/browser.h"
#include "context/editor/shell/daemon_lifecycle.h"
#include "context/editor/shell/editor_state_bridge.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/builtin_panels.h"
#include "context/editor/shell/shell.h"
#include "context/editor/shell/welcome.h"
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
    // e14c: whether `--project` was passed EXPLICITLY. A bare launch (no `--project`) that does not land
    // in a project shows the welcome screen (D13); an explicit project always opens. `current_path()` is
    // only the default so the daemon has a home when a project IS given by other means.
    bool project_explicit = false;
    // e05c: the editor now boots editor-core over its OWN scheme by default rather than a blank
    // page. `--url` still overrides it (a diagnostic escape hatch), but there is deliberately no
    // `file://` path anywhere: assets ship in-app and are served over context-editor:// (04 §1).
    std::string url = shell::kAppEntryUrl;
    std::filesystem::path app_root = CONTEXT_WEBUI_ASSET_DIR;
    // e05d3: the root scene the Scene tree + Inspector hydrate from (project-relative). EMPTY is the
    // honest no-scene state — both panels stay empty, exactly like Problems without a daemon. Scene
    // discovery/selection UX is a later seam (e08's session state); an explicit flag is the v1.
    std::string scene;
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
                "  --scene <path>    root scene the Scene tree + Inspector hydrate from "
                "(project-relative; empty = no scene)\n"
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
            out.project_explicit = true;
        }
        else if (arg == "--url" && has_value)
        {
            out.url = argv[++i];
        }
        else if (arg == "--app-root" && has_value)
        {
            out.app_root = argv[++i];
        }
        else if (arg == "--scene" && has_value)
        {
            out.scene = argv[++i];
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

    // --- the daemon lifecycle spine (e14a / D18) --------------------------------------------------
    //
    // The Shell SPAWNS-OR-ATTACHES the daemon: attach to a live one, else spawn `context daemon` as a
    // child and read the D20 token off its stdout (never argv/env). The lifecycle owns the read-only
    // STATE (03 §7), the reconnect-with-backoff, and the honest exit policy. It is CONFIGURED with its
    // handlers below (after the bridge + panels it drives exist) and STARTED just before the browser,
    // so the token + endpoint reach the bridge's egress guard before any renderer could ask for them —
    // the same "ahead of the browser" ordering e05c relied on, now spanning the full lifecycle.
    shell::DaemonLifecycle lifecycle;
    const std::filesystem::path daemon_binary =
        shell::locate_context_binary(std::filesystem::path(argv[0]));

    // --- the launch mode (e14c / D13) -------------------------------------------------------------
    //
    // A bare launch that is NOT already sitting in a project shows the WELCOME screen (recent projects,
    // "Open project…", "New from template"); every other launch opens the project. editor-core asks
    // `welcome.state` after the boot handshake and branches on this, so the daemon is spawned-or-attached
    // ONLY in project mode — there is no project to attach to on the welcome screen.
    const shell::LaunchMode launch_mode =
        shell::resolve_launch_mode(options.project, options.project_explicit);
    const bool project_mode = launch_mode == shell::LaunchMode::project;

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

    // --- the editor presence marker + the single-instance focus watcher (e14b, D15/C-F23) ---------
    //
    // Publish THIS editor's presence into `.editor/editor-state.json` (the Shell is its SINGLE writer,
    // C-F3 — the marker rides the store, never a second writer) so a second opener (`context edit .`)
    // FOCUSES us instead of duplicating. Written immediately via flush_now (not the layout debounce) so
    // an opener racing our boot sees us. The watcher, pumped from the owner loop below, consumes the
    // opener's focus request and raises window 0. The marker is retracted on clean exit (further down),
    // so a later open of a gone editor spawns a fresh one rather than trying to focus a corpse.
    {
        client::PresenceMarker marker;
        marker.pid = client::current_process_id();
        marker.boot_nonce = client::make_boot_nonce();
        manager.state_store().set_presence(marker, now_us());
        if (!manager.state_store().flush_now() && !manager.state_store().last_error().empty())
        {
            std::fprintf(stderr, "context_editor: could not publish the presence marker: %s\n",
                         manager.state_store().last_error().c_str());
        }
    }
    client::FocusRequestWatcher focus_watcher(options.project);

    shell::ShellHandshake handshake(shell::make_handshake_nonce());
    shell::BridgeRouter bridge;
    // The two daemon credentials that must never cross into the renderer (the token + the endpoint) are
    // registered with the egress guard by the lifecycle's on_attached handler below — on EVERY (re)attach,
    // because a reconnect after a daemon restart mints a NEW token (e14a). `daemon_secret_ok` latches a
    // guard refusal so the initial boot can refuse to start a renderer that could echo an unprotected
    // credential — control 3 is the backstop for the other two.
    bool daemon_secret_ok = true;
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

    // --- the welcome surface (e14c, design 07 §4 / 10 / D13) --------------------------------------
    //
    // The app's front door: `welcome.state` tells editor-core whether to render the welcome screen or
    // the editor, and `welcome.recentProjects` / `welcome.pickFolder` / `welcome.newProject` back its
    // three actions. Installed on the SAME privileged bridge; the native folder picker is boundary-clean
    // (folder_picker.cpp, OS-SDK only) and `context new` / `context edit` are spawned as subprocesses via
    // the located `context` binary. The CEF boot smokes install NO welcome surface, so editor-core's
    // `welcome.state` there answers `unknown_method` and it defaults to the panel path — unchanged.
    shell::WelcomeBridge welcome;
    welcome.set_launch_mode(launch_mode);
    if (project_mode)
    {
        const std::string project_name = options.project.lexically_normal().filename().string();
        welcome.set_project_name(project_name);
    }
    welcome.set_cli_binary(daemon_binary);
    if (!welcome.install(bridge))
    {
        std::fprintf(stderr, "context_editor: could not install the welcome bridge surface\n");
        return 1;
    }

    // --- the LIVE daemon feed + the lifecycle handlers (the Problems read path, e14a) -------------
    //
    // The Shell subscribes as an ordinary client (D10) and forwards what arrives into the panel models;
    // with no daemon there is simply no feed (Problems stays empty — the honest read-only state). The
    // subscription is OWNED BY THE LIFECYCLE, which drives it single-threaded from the owner loop
    // (poll_timeout_ms = 0, a short reconnect ladder — see daemon_lifecycle.cpp) and, crucially, RE-
    // ESTABLISHES it on a daemon restart with the fresh token, re-snapshotting the feed (03 §7). The
    // handlers registered here are re-attached to every new client automatically.
    //
    // `feed`/`tree_feed` stay POINTERS TO FORWARD-DECLARED types — this TU never sees ProblemsFeed's
    // complete definition (this executable is compiled -fno-rtti when CEF is on, and the full include
    // chain reaches a kernel header whose templated methods use typeid). The lambdas drive them through
    // the apply_* non-member seams builtin_panels.h exposes for exactly this caller.
    shell::panels::ProblemsFeed* feed = builtin.problems.get();
    shell::panels::SceneTreeFeed* tree_feed = builtin.scenetree.get();

    lifecycle.set_subscription_topics(
        {shell::panels::kDiagnosticsTopic, shell::panels::kDerivationTopic});
    lifecycle.set_reconnect_policy(shell::ReconnectPolicy{/*initial_ms*/ 200, /*max_ms*/ 2000,
                                                          /*multiplier*/ 2});
    // On EVERY (re)attach: register the daemon's token + endpoint with the egress guard so no renderer
    // can echo them back. A reconnect after a daemon restart mints a NEW token, so this runs per attach.
    lifecycle.on_attached(
        [&bridge, &lifecycle, &daemon_secret_ok](client::Client&)
        {
            const bool ok = bridge.protect_secret(lifecycle.instance().token) &&
                            bridge.protect_secret(lifecycle.instance().endpoint);
            if (!ok)
                daemon_secret_ok = false;
        });
    // 0 is only the FALLBACK generation stamp for a snapshot that carries none of its own; the real
    // cursor snapshot always does and apply_snapshot prefers it.
    lifecycle.on_snapshot(
        [feed](const std::string&, const contract::Json& snapshot)
        {
            if (feed != nullptr)
                shell::panels::apply_problems_snapshot(*feed, snapshot, 0);
        });
    lifecycle.on_event(
        [feed, tree_feed](const std::string&, const client::ClientEvent& event)
        {
            if (feed != nullptr)
                (void)shell::panels::apply_problems_event(*feed, event.topic, event.payload,
                                                          event.generation);
            if (tree_feed != nullptr)
                (void)shell::panels::apply_scenetree_event(*tree_feed, event.topic, event.payload,
                                                           event.generation);
        });

    // START the spine: attach to a live daemon, else spawn `context daemon` as a child and read the D20
    // token off its stdout. Read-only, not fatal (03 §7) — the editor opens and pump() keeps retrying.
    // e14c: ONLY in project mode. The welcome screen has no project, so there is nothing to attach to or
    // spawn; the daemon lifecycle stays idle (read-only) until the user opens or creates a project, which
    // launches a fresh `context_editor --project <path>` process (the e14a/D15 flow) with its own daemon.
    std::string daemon_error;
    if (project_mode && !lifecycle.spawn_or_attach(options.project, daemon_binary, daemon_error))
    {
        std::fprintf(stderr,
                     "context_editor: not attached to a daemon (%s); the editor opens read-only\n",
                     daemon_error.c_str());
    }
    // e14c: record this project as the most-recent so the NEXT bare launch surfaces it on the welcome
    // screen. Best-effort — a failure here (no HOME, a read-only disk) must never keep the editor from
    // opening, so its result is deliberately ignored.
    if (project_mode)
    {
        std::string recent_error;
        (void)shell::record_recent_project(shell::user_config_path(), options.project,
                                           static_cast<std::int64_t>(now_us() / 1000), 10,
                                           &recent_error);
    }
    // Control 3 backstop: if the guard refused a daemon credential on the initial attach, refuse to
    // boot a renderer that could echo it back (CEF is not yet initialized here, so a clean early exit).
    if (lifecycle.client() != nullptr && !daemon_secret_ok)
    {
        std::fprintf(stderr,
                     "context_editor: the egress guard refused a daemon credential (empty or shorter "
                     "than the minimum protected length) — refusing to start a renderer that could "
                     "echo it back\n");
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

    // The window is adopted now that its browser + present path exist; the manager applies any
    // remembered placement before the first frame (it was constructed far above so it could outlive
    // the bridge handlers — see there).
    manager.add(std::move(window));

    // --- the owner loop ----------------------------------------------------------------------------
    int frames = 0;
    while (manager.pump_once(now_us()))
    {
        ++frames;
        // Drive the daemon link (e14a / 03 §7): pump the live feed, and on a lost daemon enter the
        // read-only STATE + reconnect with backoff, re-snapshotting on reattach. A daemon restart is
        // stalled at most the ladder's bounded duration. now in ms for the backoff clock.
        lifecycle.pump(static_cast<std::int64_t>(now_us() / 1000));
        // e14b: a second opener that found our presence marker wrote a focus request — consume it and
        // raise window 0 (best-effort; a headless backend simply does nothing). This is the C-F23
        // single-instance FOCUS, arbitrated entirely through the project's own `.editor/` state.
        if (focus_watcher.poll())
        {
            if (shell::EditorWindow* front = manager.window(0))
            {
                front->backend().request_activation();
            }
        }
        if (client::Client* live_client = lifecycle.client())
        {
            // e05d3: drain the live-hydration work the feeds marked due (Scene tree re-reads on settle;
            // the Inspector's selection fetch). Uses the CURRENT client, so a reconnect's new client is
            // picked up automatically. Synchronous on the owner loop; a no-op when nothing is due.
            shell::panels::pump_panel_feeds(builtin, *live_client, options.scene);
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

    // The exit policy (e14a): an owned daemon this process is the last client of gets a clean in-band
    // `shutdown`; an owned daemon other clients still hold is left running; an external daemon is never
    // touched. Runs before the manager/CEF teardown so the daemon call still has a live wire.
    lifecycle.shutdown_at_exit();
    // e14b: retract the presence marker so a later opener spawns a fresh editor instead of trying to
    // focus this process after it is gone. Cleared before manager.shutdown() flushes + closes.
    manager.state_store().clear_presence(now_us());
    (void)manager.state_store().flush_now();
    manager.shutdown();
#if defined(CONTEXT_EDITOR_HAS_CEF)
    shell::cef::shutdown();
#endif
    return 0;
}
