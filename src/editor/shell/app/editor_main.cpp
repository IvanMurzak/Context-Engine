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
#include "context/editor/shell/banners.h"
#include "context/editor/shell/browser.h"
#include "context/editor/shell/chrome_facts.h" // a1: a maximized flip -> an editor.ui chrome fact
#include "context/editor/shell/cocoa_chrome.h" // c1: the macOS hybrid mode + measured inset query
#include "context/editor/shell/cocoa_menu.h"   // d3: the native NSMenu install (honest false off macOS)
#include "context/editor/shell/cross_window_drag.h"
#include "context/editor/shell/daemon_lifecycle.h"
#include "context/editor/shell/editor_state_bridge.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/keybindings_bridge.h"
#include "context/editor/shell/menu_facts.h" // d3: a native menu activation -> an editor.ui fact
#include "context/editor/shell/menu_model.h" // d3: the published menu model's fail-closed parse
#include "context/editor/shell/themes_bridge.h"
#include "context/editor/shell/user_config.h"
#include "context/editor/shell/package_sessions.h" // e13c-1: per-package BASELINE daemon sessions
#include "context/editor/shell/package_grants.h"   // e13c-4: the persisted grants + install consent
#include "context/editor/shell/package_store.h"    // e13c-3: ~/.context/packages — the mount producer
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/builtin_panels.h"
#include "context/editor/shell/session_bridge.h"
#include "context/editor/shell/shell.h"
#include "context/editor/shell/welcome.h"
#include "context/editor/shell/window.h"
#include "context/editor/shell/window_bridge.h"
#include "context/editor/shell/write_notice.h" // e09b-3: the LOUD refused-write relay (05 §8)
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
#include <optional>
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

// e10b: how a WINDOW-MANAGEMENT bridge's handlers reach the registry. Bound IDENTICALLY on window 0
// and on every factory-created window, so a tear-out / move / rehome / close behaves the same wherever
// it is asked. Captures only `manager` + `store`, both of which outlive every window AND the graveyard
// the registry retires windows into, so the handlers stay valid for a retired session's whole life
// (window_registry.h § LIFETIME RULE).
void bind_window_bridge_handlers(shell::WindowBridge& wb, shell::WindowManager& manager,
                                 shell::WindowMoveStore& store, bool headless)
{
    wb.bind_windows([&manager]() -> std::vector<shell::WindowId> { return manager.window_ids(); });
    wb.bind_tear_out(
        [&manager, &store, headless](const shell::WindowBridge::TearOut& req)
            -> shell::WindowMoveResult
        {
            shell::WindowSpec spec;
            if (!req.title.empty())
                spec.title = req.title;
            spec.headless = headless;
            const shell::WindowCreateResult created = manager.create_window(spec, req.source);
            if (!created.ok())
                return {false, shell::kInvalidWindowId, shell::to_string(created.outcome),
                        created.error};
            // Seed the NEW window: its editor-core reads exactly this panel + state at boot (D6).
            store.set_boot_seed(created.id, req.seed);
            return {true, created.id, shell::to_string(created.outcome), std::string{}};
        });
    wb.bind_move_to(
        [&manager, &store](const shell::WindowBridge::MoveTo& req) -> shell::WindowMoveResult
        {
            if (manager.window(req.target) == nullptr)
                return {false, shell::kInvalidWindowId, std::string{}, "no live window with that id"};
            store.enqueue_rehome(req.target, req.seed);
            return {true, req.target, std::string{}, std::string{}};
        });
    wb.bind_close(
        [&manager, &store](shell::WindowId self) -> shell::WindowMoveResult
        {
            // `close_window`, NOT `destroy_window`: the primary's ✕ is the app quit (the policy and
            // the reason live in shell.h § close_window). `ok()` stays "this window is gone", which
            // is what the seed-forget below keys on; `accepted()` is what the renderer is told.
            const shell::WindowDestroyResult d = manager.close_window(self);
            if (d.ok())
                store.forget(self); // drop any seeds queued for a window that is now gone
            return {d.accepted(), self, shell::to_string(d.outcome), d.error};
        });

    // --- the chrome contract (editor-window-chrome a1, target design 02 §1 / §5) ------------------
    // The control verbs reach THIS window's backend through the registry, so they stay valid for a
    // retired session (the lookup answers nullptr and the verb degrades to `accepted:false`) — the
    // same lifetime rationale the handlers above rely on. INTERIM HONESTY (tasks/README.md,
    // binding): the provider reports what each backend actually DOES. Since b1 the win32 backend
    // OWNS its frame (the WM_NCCALCSIZE / WM_NCHITTEST takeover, win32_window.cpp), so it
    // truthfully reports `custom` — the flip lands in the same PR as the behaviour, never ahead of
    // it. c1 flipped cocoa the same way: a live macOS window answers `hybrid` with the MEASURED
    // traffic-light inset (cocoa_chrome.h — the query re-reads the window's real style at call
    // time, so the flip can never outlive the behaviour). x11 stays `system` permanently (D6: the
    // WM owns the frame), which the default below IS; headless — every CI smoke — stays `system` too.
    const shell::WindowId self_id = wb.self_id();
    wb.bind_chrome_state(
        [&manager, self_id]() -> shell::ChromeState
        {
            shell::ChromeState state; // defaulted: mode system, inset 0
            if (shell::EditorWindow* window = manager.window(self_id))
            {
                if (std::string_view(window->backend().name()) == "win32")
                {
                    state.mode = shell::ChromeMode::custom; // b1: the frame is ours on Windows
                }
                state.maximized = window->backend().placement().maximized;
                state.focused = window->focused();
                shell::CocoaChromeState cocoa;
                if (shell::cocoa_hybrid_chrome(window->backend(), cocoa))
                {
                    state.mode = shell::ChromeMode::hybrid;
                    state.controls_inset_left = cocoa.inset_left;
                    state.controls_inset_right = cocoa.inset_right;
                }
            }
            return state;
        });
    // b1: the appearance report — the theme's dark/light choice reaches the backend, whose win32
    // arm sets the ONE Dwm dark-mode attribute (02 §3); every other backend's set_appearance is an
    // honest no-op today. Accepted iff a live window received it, like the control verbs.
    wb.bind_appearance(
        [&manager, self_id](bool dark) -> bool
        {
            shell::EditorWindow* window = manager.window(self_id);
            if (window == nullptr)
                return false;
            window->backend().set_appearance(dark);
            return true;
        });
    wb.bind_minimize(
        [&manager, self_id]() -> bool
        {
            shell::EditorWindow* window = manager.window(self_id);
            if (window == nullptr)
                return false;
            window->backend().minimize();
            return true;
        });
    wb.bind_toggle_maximize(
        [&manager, self_id]() -> std::optional<bool>
        {
            shell::EditorWindow* window = manager.window(self_id);
            if (window == nullptr)
                return std::nullopt;
            // The toggle is composed HERE from OS truth (window.h § the chrome verbs), so the
            // renderer never has to know the state to flip it — and a maximize from any other
            // source (Win+Up, the WM) is still toggled correctly.
            const bool next = !window->backend().placement().maximized;
            window->backend().set_maximized(next);
            return next;
        });
    // d3: the focus handler is TARGET-aware now (the Window menu's window list — window_bridge.h
    // § kWindowFocusMethod). The bridge resolves an absent wire id to `self_id`, so the a1
    // self-focus callers (the titlebar controls) behave exactly as before; a menu entry names a
    // peer, and an id no live window answers is the honest false.
    wb.bind_focus(
        [&manager](shell::WindowId target) -> bool
        {
            shell::EditorWindow* window = manager.window(target);
            if (window == nullptr)
                return false;
            window->backend().request_activation();
            return true;
        });
}

// e10b: everything a FACTORY-CREATED window's editor-core calls during boot — the SAME surface set
// window 0 gets (mirrors the multiwindow smoke's `WindowSurfaces`), so a torn-out panel is genuinely
// LIVE in its new window (renders, restores its D6 state, and can be torn out / moved again), not a
// blank second window. Type-erased into `parts.surfaces` (window_registry.h) so the registry outlives
// it without knowing its shape; member order puts `handshake` first so it is destroyed LAST (the
// router's handlers captured it), and the router lives in the session, outside this object.
//
// THE DAEMON FEED IS WIRED HERE SINCE M9 e09e-3 — and the sentence that used to sit in this comment
// ("deliberately NOT wired; only window 0's builtins are fed by the app's lifecycle... cross-window
// live-feed mirroring is e10c/e10d") is RETIRED, because it had become false in both halves. e10c
// (drag) and e10d (the `editor.ui` mirror) landed and neither touched the DAEMON feed: e10d mirrors
// D7 tier-2 CHROME facts between windows, which is a different stream from the derived-world facts a
// panel model is built out of. So the deferral pointed at tasks that were never going to satisfy it,
// and the consequence was structural: design 05 §8's tail — "events: files.changed ->
// derivation.settled{gen} -> all subscribed clients (window 1, window 2, CLI, agents) update" — was
// UNREACHABLE for every window but the first. A secondary window's Scene tree and Inspector rendered
// their empty default state for the life of the process, and its Inspector could not write at all (a
// gateway with no client refuses, honestly but permanently).
//
// What is wired, and where: `secondary_feeds` below records every live window's bag + its OWN daemon
// connection, `lifecycle.on_event` / `on_snapshot` dispatch each fact into EVERY bag, and the owner
// loop re-derives each bag's non-owning client views and pumps its due hydration work per frame —
// the same four seams window 0 has always used, in the same order. The live proof is
// `editor-cef-smoke-shell-inspector-fanout` (M9 e09e-3), which composes two windows exactly this way.
//
// WHY THE EVENTS COME FROM THE ONE LIFECYCLE SUBSCRIPTION AND NOT FROM EACH WINDOW'S OWN. A settle is
// a GLOBAL fact about the derived world; it says nothing about who is listening, so N subscriptions
// would deliver N identical copies of it at N times the daemon's fan-out cost. What each window DOES
// need of its own is its `origin` — and it has that already: the factory gives every window its own
// wire connection (`parts.daemon_client`), so `bind_session_client` gives each feed its own
// daemon-minted id and per-window echo suppression stays correct.
struct SecondaryWindowSurfaces
{
    shell::ShellHandshake handshake{shell::make_handshake_nonce()};
    shell::PanelHost panel_host;
    shell::panels::BuiltinPanels builtin = shell::panels::install_builtin_panels(panel_host);
    shell::EditorStateBridge editor_state;
    shell::KeybindingsBridge keybindings;
    shell::ThemesBridge themes;
    shell::WelcomeBridge welcome;
    shell::BannerBridge banners;
    shell::UserConfigStore config;
    shell::SessionBridge session_bridge;
    shell::WindowBridge window_bridge;

    SecondaryWindowSurfaces(shell::WindowId window_id, shell::WindowMoveStore& store)
        : window_bridge(window_id, store)
    {
    }

    [[nodiscard]] bool install(shell::BridgeRouter& router, shell::WindowManager& manager,
                               shell::WindowMoveStore& store, shell::CrossWindowDragStore& drag_store,
                               shell::UiMirrorStore& mirror_store, shell::WindowId window_id,
                               bool headless)
    {
        bool ok = handshake.install(router);
        ok = panel_host.install(router) && ok;
        editor_state.bind_store(&manager.state_store(), now_us);
        editor_state.bind_regions(
            [&manager, window_id](std::vector<shell::ShellRegion> regions)
            {
                // Routed to THIS window (window_registry.h): a hard-coded window 0 would hand the new
                // window's region rects to the primary's input arbiter.
                if (shell::EditorWindow* target = manager.window(window_id))
                    target->input().regions().publish(std::move(regions));
            });
        ok = editor_state.install(router) && ok;
        keybindings.bind_path(shell::default_keybindings_path());
        ok = keybindings.install(router) && ok;
        themes.bind_directory(shell::default_themes_directory());
        ok = themes.install(router) && ok;
        welcome.set_launch_mode(shell::LaunchMode::project);
        ok = welcome.install(router) && ok;
        ok = banners.install(router) && ok;
        config.bind_path(shell::user_config_path());
        ok = config.install(router) && ok;
        ok = session_bridge.install(router) && ok; // unbound -> the honest edit/attached:false baseline
        bind_window_bridge_handlers(window_bridge, manager, store, headless);
        // e10c: this window can be a cross-window drag TARGET, so it answers `drag.probe` / `drag.
        // report-zone` off the SHARED relay — the same store window 0 and the drag session use.
        window_bridge.bind_drag_store(&drag_store);
        // e10d/e09b-3: and off the SHARED `editor.ui` mirror relay, so a fact broadcast by the Shell
        // — or published by a peer window — reaches THIS window's editor-core too. A secondary window
        // that did not drain the queue would be the one place a loud write notice never appeared.
        window_bridge.bind_ui_mirror_store(&mirror_store);
        ok = window_bridge.install(router) && ok;
        return ok;
    }
};

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
            // REPORTED, never silent. Since e12b every v1 platform HAS a backend, so reaching here
            // is a creation FAILURE (no display, no GUI session, a refused window) or a
            // non-v1 platform — never "nobody implemented this yet".
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
    //
    // e10b: the cross-window MOVE relay (boot seeds + rehome queues). Declared BEFORE the manager so
    // it OUTLIVES every window's WindowBridge — including those in retired sessions the manager frees
    // only in ~WindowManager (window_registry.h § LIFETIME RULE).
    shell::WindowMoveStore move_store;
    // e10c: the cross-window DRAG relay (the hover the Shell publishes for the window under the cursor,
    // and the drop zone that window's editor-core answers over the bridge). ONE per app, shared by every
    // window's WindowBridge and by the drag session below; declared with `move_store` so it OUTLIVES
    // every window's bridge — including the retired ones the manager frees in ~WindowManager.
    shell::CrossWindowDragStore drag_store;
    // e10d: the cross-window `editor.ui` MIRROR relay. Declared with the two stores above and for the
    // same lifetime reason — every window's WindowBridge references it, retired sessions included.
    //
    // ⚠ e09b-3 IS WHAT FIRST BINDS IT IN THE SHIPPING EDITOR. e10d landed the relay and wired it only
    // in its own live smoke, so `ui.mirror` / `ui.mirror-poll` were ROUTED but inert here: editor-core
    // polled an empty queue forever. That was harmless while nothing published — the built-in
    // focus/layout/palette publishers are later seams — and it is exactly the channel a loud write
    // notice needs (write_notice.h § WHY IT RIDES THE `ui.mirror` RELAY), so binding it is the whole
    // transport half of this task. Cross-window chrome mirroring becomes live as a side effect, which
    // is the behaviour e10d specified in the first place.
    shell::UiMirrorStore mirror_store;
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
    // --- e09e-3: the live SECONDARY windows the daemon feed fans out to ---------------------------
    //
    // Declared HERE, ahead of the daemon lifecycle's handlers (which iterate it) and of the window
    // factory (which appends to it), because both need to name it.
    //
    // WHY A LOCAL REGISTRY AND NOT AN ACCESSOR ON `WindowManager`. The registry stores a session's
    // surfaces TYPE-ERASED (`std::vector<std::shared_ptr<void>>`, window_registry.h) and its daemon
    // client behind a `unique_ptr` it owns, and exposes neither — deliberately, so the registry stays
    // ignorant of what a window's surfaces are. The factory is the one place that knows BOTH concrete
    // types, so it records them here on the way past.
    //
    // LIFETIMES, both of them safe and for different reasons: the surfaces are held WEAKLY, so a
    // window whose session the manager eventually frees (only in ~WindowManager) cannot be fed through
    // a dangling pointer; the client is a RAW pointer into a session the registry RETIRES rather than
    // frees on a mid-process destroy (the CE #319 rule), so it stays valid for the life of the
    // process. Every use below is additionally gated on the window still being LIVE
    // (`manager.window(id) != nullptr`), so a destroyed window stops being fed and pumped even though
    // its bag is still allocated.
    struct SecondaryFeed
    {
        shell::WindowId id = shell::kInvalidWindowId;
        std::weak_ptr<SecondaryWindowSurfaces> surfaces;
        client::Client* client = nullptr;
    };
    std::vector<SecondaryFeed> secondary_feeds;
    // e09b-3: the LOUD write-notice relay. Declared BEFORE `builtin` so it OUTLIVES it — the sinks
    // `bind_write_notice_relay` installs on the two feeds capture a raw pointer to it, and members
    // (and locals) destroy in reverse declaration order, so a relay declared after the bag would die
    // while those sinks were still reachable.
    shell::WriteNoticeRelay notice_relay;
    notice_relay.bind_store(&mirror_store);
    notice_relay.bind_windows([&manager]() -> std::vector<shell::WindowId>
                              { return manager.window_ids(); });
    // a1: the chrome-fact relay — a maximized flip the placement poll observed becomes an
    // `editor.ui` fact in the affected window's mirror queue (02 §1: no new push channel, no extra
    // poll). The sink the manager stores captures this relay by reference; that is safe because the
    // manager only fires it from pump_once, which never runs after the explicit `manager.shutdown()`
    // below — the same ordering rationale the bridges rely on.
    shell::ChromeFactRelay chrome_relay;
    chrome_relay.bind_store(&mirror_store);
    manager.on_chrome_maximized(
        [&chrome_relay](shell::WindowId id, bool maximized)
        { (void)chrome_relay.publish_maximized(id, maximized); });
    // d3: the menu-activation relay — a native NSMenu item activation becomes an `editor.ui.menu`
    // fact in the primary window's mirror queue, which editor-core drains and executes through the
    // ONE e07b registry (menu_facts.h: no second dispatch system). Same lifetime rationale as the
    // chrome relay above: the Cocoa menu's activation callback captures it by reference, and menu
    // activations only ever fire from the owner loop's pump.
    shell::MenuActivationRelay menu_relay;
    menu_relay.bind_store(&mirror_store);
    shell::panels::BuiltinPanels builtin = shell::panels::install_builtin_panels(panel_host);
    // A refused write (an L-30 drop, or a write path that said no) now reaches the human: the relay
    // turns it into an `editor.ui` fact in EVERY live window, where editor-core's notification host
    // renders it (design 05 §8's three sinks; design 10's non-negotiable "LOUD, never silent").
    shell::panels::bind_write_notice_relay(builtin, notice_relay);
    if (builtin.bound != shell::panels::hostable_panel_ids().size())
    {
        // REPORTED, not fatal: an editor that refused to start because one panel could not bind
        // would be less useful than one that opens with the rest and says which is missing.
        std::fprintf(stderr,
                     "context_editor: only %zu of %zu built-in panels bound; the rest will report "
                     "as unavailable\n",
                     builtin.bound, shell::panels::hostable_panel_ids().size());
    }
    // e09c: RESTORE the session undo journal from the previous run. `WindowManager`'s constructor
    // already loaded `.editor/editor-state.json`, so the document is in hand — this adopts its `undo`
    // blob into the live journal, which is what makes Ctrl+Z on the FIRST gesture after a restart
    // revert the edit from BEFORE the restart. A fresh project (or a corrupt blob, which
    // undo_feed.cpp reports) simply restores nothing and starts empty.
    // (Reported through the free seam alone — `UndoFeed` stays forward-declared here, so this TU
    // keeps its narrow include set, the discipline builtin_panels.h documents for every feed.)
    // True only when a step actually came back (see the seam): a well-formed but EMPTY journal
    // loads fine and restores nothing, and announcing "restored" for that is the kind of cheerful
    // noise that teaches a reader to stop believing the log.
    if (shell::panels::restore_undo_state(builtin, manager.state_store().state()))
    {
        std::fprintf(stderr, "context_editor: restored the previous session's undo history\n");
    }
    // e09d: ANNOUNCE a corrupt-and-reset `.editor/editor-state.json` (07 §6 — recovery is never
    // silent and never blocking). `WindowManager`'s constructor already loaded the file; this is the
    // first point where the Problems panel exists to render the diagnostic in, which is why the
    // report is carried on the store rather than announced at load time.
    //
    // BOTH channels, for the reason the daemon publishes both: stderr so an operator sees it with no
    // UI attached, and the Problems panel so the human whose layout and undo history just vanished is
    // told by the editor itself rather than by a terminal they may never look at.
    {
        const shell::EditorStateRestoreReport& restore = manager.state_store().restore_report();
        if (restore.outcome == shell::EditorStateRestoreOutcome::recovered)
        {
            // The suffix is decided from the STRUCTURED facts, not from an empty `quarantined_path`
            // alone: "no copy was needed" and "no copy exists, and those bytes are the only ones the
            // user has left" both leave it empty, and only the second is the one worth shouting.
            const std::string outcome_suffix =
                !restore.quarantined_path.empty()
                    ? "; the corrupt file was moved aside to " + restore.quarantined_path
                    : (restore.preservation_failed
                           ? std::string("; it could NOT be preserved, so it has been left exactly "
                                         "as it is and this editor will not save session state "
                                         "over it")
                           : std::string());
            const std::string message =
                "the editor's own session file was unreadable and has been reset to defaults "
                "(window layout, panel state and session undo history): " +
                restore.detail + outcome_suffix;
            std::fprintf(stderr, "[editor-state] %s\n", message.c_str());
            std::fflush(stderr);
            // The PANEL gets the project-relative spelling, not `restore.path`. That argument lands
            // in the payload's `file` member, which is the member ProblemsFeed renders and groups
            // by, and every other row in the panel is project-relative — an absolute native path
            // would be the one entry with a `C:\…` group header. stderr above keeps the absolute
            // form, where the reader may have no project context at all.
            (void)shell::panels::report_local_problem(builtin, shell::kEditorStateInvalidCode,
                                                      message, restore.project_relative_path);
        }
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
    // the editor, and `welcome.open` / `welcome.pickFolder` / `welcome.newProject` back its
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

    // --- the keybindings read/watch surface (e07c, design 05 §6 / 03 §6) --------------------------
    //
    // The Shell owns the per-user override `~/.context/keybindings.json`: editor-core is a pure
    // wire-client and cannot read it, so this bridge reads + WATCHES the file and serves its bytes over
    // `keybindings.get`; editor-core (keymap.ts) schema-validates + merges them. Boundary-clean (plain
    // std::filesystem, nothing kernel-internal), so the D10 shell-boundary gate's FORBIDDEN list is
    // untouched. Declared alongside the other bridges so it OUTLIVES the handler `install` captures; the
    // watch tick runs in the owner loop below. An empty home directory yields a permanently-absent
    // snapshot (the editor runs on the default keymap) rather than a failure.
    shell::KeybindingsBridge keybindings_bridge;
    keybindings_bridge.bind_path(shell::default_keybindings_path());
    if (!keybindings_bridge.install(bridge))
    {
        std::fprintf(stderr, "context_editor: could not install the keybindings bridge surface\n");
        return 1;
    }

    // --- the watched-themes feed (e06b, design 06 §4) ---------------------------------------------
    //
    // The exact sibling of the keybindings bridge above, one directory wider: `~/.context/themes/
    // *.theme.json` are per-user CONFIG files editor-core cannot read (pure wire-client), so this
    // bridge reads + WATCHES the directory and serves the bytes over `themes.get`; editor-core
    // (theme.ts) schema-validates them against the e06a token schema and registers what passes. A
    // malformed theme is rejected THERE with a diagnostic and the applied theme stands — a bad theme
    // is never a broken UI. Boundary-clean for the same reason (plain std::filesystem), so the D10
    // FORBIDDEN list is untouched, and an unresolvable home yields a permanently-empty snapshot (the
    // editor runs on the four built-in themes).
    shell::ThemesBridge themes_bridge;
    themes_bridge.bind_directory(shell::default_themes_directory());
    if (!themes_bridge.install(bridge))
    {
        std::fprintf(stderr, "context_editor: could not install the themes bridge surface\n");
        return 1;
    }

    // --- the per-user config surface (e06d, design 06 4 / C-F14) ---------------------------------
    //
    // THE SHELL IS THE SINGLE WRITER of `~/.context/config.json`. This store reads + WATCHES it and
    // serves `config.get`, and it is the ONLY thing in the process that writes it: the Settings panel
    // REQUESTS a change over `config.set`, the store validates against a closed settable vocabulary and
    // persists. Boundary-clean like its two siblings above (plain std::filesystem + stream IO), so the
    // D10 FORBIDDEN list is untouched. An unresolvable home yields a permanently-empty document and
    // every write refuses honestly - the editor still runs, it just cannot remember a choice.
    shell::UserConfigStore user_config;
    user_config.bind_path(shell::user_config_path());
    if (!user_config.install(bridge))
    {
        std::fprintf(stderr, "context_editor: could not install the config bridge surface\n");
        return 1;
    }

    // --- the two notification banners (e14d, design 07 §4 / 08 threat row) ------------------------
    //
    // THE UPDATE NOTICE runs its ONE HTTPS GET on a worker thread started here and adopted by the
    // owner loop below, so a boot-time network round trip never stalls the first frame. The request is
    // a byte-identical constant carrying no identifier of any kind (banners.h + the
    // `editor-shell-release-request` gate); the version comparison happens locally.
    //
    // THE DAEMON-LOST PROBE IS BOUND ONLY IN PROJECT MODE, and that is a correctness point rather than
    // an optimisation: `DaemonLifecycle::read_only()` is true whenever the Shell is not live
    // read-write, which on the WELCOME screen is always — no project is open, so there is no daemon to
    // have lost. Binding it there would paint a permanent "reconnecting" banner on the app's front
    // door. A daemon can only be LOST once one was wanted.
    shell::ReleaseNotice release_notice;
    release_notice.bind_transport(shell::native_https_get);
    release_notice.bind_url_opener(shell::native_open_url);
    release_notice.begin_check();

    shell::BannerBridge banners;
    banners.bind_release_notice(&release_notice);
    if (project_mode)
    {
        banners.bind_link_probe(shell::make_daemon_link_probe(lifecycle));
    }
    if (!banners.install(bridge))
    {
        std::fprintf(stderr, "context_editor: could not install the banner bridge surface\n");
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
    // M9 e1: the Files feed (D10 read half). Same forward-declared-pointer discipline.
    shell::panels::FilesFeed* files_feed = builtin.files.get();
    // e08b: the daemon session feed (selection / play). Same forward-declared-pointer discipline.
    shell::panels::SessionFeed* session_feed = builtin.session.get();
    // e09e-2: the Inspector rides the SAME `derivation` stream, so design 05 §8's fan-out tail — a
    // landed write reaching every subscribed client — reaches this panel too. Same discipline. (Since
    // M9 x9 a plain `edit` PUBLISHES that settle, so the cross-window case is live: any client's write,
    // including this window's own, arrives here — inspector_feed.h § THE PUBLISHER HALF.)
    shell::panels::InspectorFeed* inspector_feed = builtin.inspector.get();

    // --- the daemon SESSION read surface for editor-core (e08d, design 05 §4 / §6) ---------------
    // The Shell subscribes to the `session` topic below and `SessionFeed` holds the resulting L-51
    // play state. editor-core needs that same fact for its `when`-contexts and has no daemon socket
    // of its own, so it READS this relay over the privileged bridge — the boot.ts wiring that
    // replaces e08b's frozen `STUB_SESSION_STATE` boot baseline.
    //
    // The provider reads the feed through the non-member seams (this TU only ever sees the
    // forward-declared `SessionFeed`) and reports `attached` from the lifecycle, so a Shell with no
    // daemon says "edit, not attached" rather than implying a live session in edit. Installed even
    // OUTSIDE project mode: the welcome screen boots the same bundle, and an uninstalled method is a
    // deny-by-default refusal on a channel whose smokes forbid refusals.
    shell::SessionBridge session_bridge;
    session_bridge.bind_provider(
        [session_feed, &lifecycle]() -> shell::SessionStateSnapshot
        {
            shell::SessionStateSnapshot snapshot;
            snapshot.attached = lifecycle.client() != nullptr;
            if (session_feed != nullptr)
            {
                snapshot.play_state = shell::panels::session_play_state(*session_feed);
                // The composed d1 generation — applied facts + control generation, so locally
                // driven transitions move it too (builtin_panels.h § session_state_generation
                // owns the reasoning).
                snapshot.generation = shell::panels::session_state_generation(*session_feed);
                snapshot.sim_tick = shell::panels::session_sim_tick(*session_feed);
            }
            return snapshot;
        });
    // The WRITE half (d1): `session.control` relays to the feed's PlaybarModel — the e08b chain
    // (`editor.play|pause|stop|step` through the Shell's own client, echo-suppressed) — so the
    // strip, the palette and the dock panel drive ONE implementation. With no feed the bridge's own
    // unbound degrade answers "nothing to drive".
    if (session_feed != nullptr)
    {
        session_bridge.bind_control(
            [session_feed](const std::string& verb) -> shell::SessionControlOutcome
            { return shell::panels::session_control(*session_feed, verb); });
        // d3: the selection write — `selection.clear`'s relay to the feed's `editor.select` writer
        // (the same proven chain, echo suppression included). With no feed the bridge's own unbound
        // degrade answers the honest `applied:false`.
        session_bridge.bind_select(
            [session_feed](const std::vector<std::string>& ids) -> shell::SessionSelectOutcome
            { return shell::panels::session_select(*session_feed, ids); });
    }
    if (!session_bridge.install(bridge))
    {
        std::fprintf(stderr, "context_editor: could not install the session bridge surface\n");
        return 1;
    }

    // --- the window-management surface (e10b, design 03 §1 / §7) -----------------------------------
    //
    // editor-core's ONLY way to move a panel between windows: tear-out, "move to window N", and
    // window-close rehome all travel through here (the browser side has no window registry of its
    // own). Bound to window 0 (the primary) + the shared move relay; the factory below installs the
    // SAME surface on every window it creates. This is the ONE new boot-time bridge method e10b adds,
    // so it is installed on EVERY live CEF smoke too, or the router's deny-unknown-methods default
    // reddens the ones not updated (window_registry.h; the e06d `refused()==0` rule).
    shell::WindowBridge window_bridge(shell::kPrimaryWindowId, move_store);
    bind_window_bridge_handlers(window_bridge, manager, move_store, options.headless);
    // e10c: window 0 answers the cross-window drag probe off the shared relay too — the primary is a
    // valid drop target like any peer (a panel dragged out of window 1 can rehome back into it).
    window_bridge.bind_drag_store(&drag_store);
    // e10d/e09b-3: window 0 drains its `editor.ui` mirror queue off the shared relay. Without this
    // bind the method routes but answers an empty batch forever — which is how a loud write notice
    // would reach the browser side and be silently discarded one hop short of the human.
    window_bridge.bind_ui_mirror_store(&mirror_store);
    // d3: the menu publish — bound on WINDOW 0 ONLY, because the model describes the app's one menu
    // bar and only the primary window's editor-core publishes it (secondary windows render the
    // compact chrome, no menu — 02 §9). The handler parses the model (fail-closed, menu_model.h)
    // and offers it to the Cocoa backend; on Windows/Linux `cocoa_install_menu` answers its honest
    // false and the publish degrades to `accepted:false` — the web menubar in the titlebar strip IS
    // the rendering there. An activated native item routes through the relay above and back to
    // editor-core's ONE registry over the existing ui.mirror poll.
    window_bridge.bind_menu(
        [&manager, &menu_relay](const contract::Json& model_json) -> bool
        {
            const std::optional<shell::MenuModel> model = shell::parse_menu_model(model_json);
            if (!model.has_value())
            {
                return false; // unreachable through the bridge (it validates first) — belt-and-braces
            }
            shell::EditorWindow* window = manager.window(shell::kPrimaryWindowId);
            if (window == nullptr)
            {
                return false;
            }
            return shell::cocoa_install_menu(
                window->backend(), *model,
                [&menu_relay](const std::string& command_id)
                { (void)menu_relay.publish_activation(shell::kPrimaryWindowId, command_id); });
        });
    if (!window_bridge.install(bridge))
    {
        std::fprintf(stderr, "context_editor: could not install the window bridge surface\n");
        return 1;
    }

    // --- the installed packages (M9 e13c-3) -------------------------------------------------------
    // THE FIRST REAL PRODUCER of `CefShellOptions::ext_packages`. Scanned unconditionally, outside the
    // CEF guard, so a CEF-free build still REPORTS what it found: the store is a real user-facing
    // directory, and "my panel did not appear" must have an answer on every build rather than only on
    // the one that could have shown it.
    //
    // Every refusal is printed. A silent skip here is the failure mode a user cannot diagnose and a
    // reviewer cannot distinguish from an absent check (package_store.h, decision 3) — and it is why
    // the scan returns refusals rather than logging them itself: the policy about WHERE a diagnostic
    // goes belongs to the composition root, not to the reader.
    //
    // ⚠ SCANNED HERE, AHEAD OF THE PACKAGE SESSION HOST — moved up by M9 e13c-4, which is the task
    // that gave the scan a SECOND consumer. The session host's attach scope is now derived from the
    // grant document indexed by this scan (`PackageGrantHost` below), so the scan must exist before
    // the host is constructed. Nothing about the scan itself changed, and the CEF options below still
    // read the same object.
    const std::filesystem::path package_store = shell::package_store_root();
    // NOT const: `PackageGrantHost` takes `PackageStoreScan&` and rewrites `sandbox.granted_scopes`.
    shell::PackageStoreScan package_scan = shell::scan_package_store(package_store);
    for (const shell::PackageRefusal& refusal : package_scan.refusals)
    {
        // ONE label, and it NAMES THE PATH: three of the refusals (store absent, store unreadable,
        // manifest missing) carry no path in their message, so without this an operator is told which
        // package failed but never which directory — and a store-level refusal, whose `id` is empty by
        // construction, rendered as a bare `package store: : <message>`.
        const std::string where = refusal.id.empty()
                                      ? refusal.path.string()
                                      : "'" + refusal.id + "' (" + refusal.path.string() + ")";
        std::fprintf(stderr, "context_editor: package store: %s: %s\n", where.c_str(),
                     refusal.message.c_str());
    }
    for (const shell::InstalledPackage& package : package_scan.packages)
    {
        // "installed at", NOT "mounted from": no mount has been attempted at this point, and on a
        // CEF-free build none ever will be. Claiming a mount here would mislead exactly the
        // "my panel did not appear" investigation this block exists to serve.
        std::printf("context_editor: package '%s' v%s (%zu contribution(s)) installed at %s\n",
                    package.id.c_str(), package.version.c_str(), package.contributions.size(),
                    package.root.string().c_str());
    }
    // ⚠ THE CONTRIBUTIONS ARE READ AND REPORTED, NOT REGISTERED — see package_store.h's boundary
    // note. Appending one to the built-in roster below would red the blocking `gui-a11y-coverage`
    // gate (which asserts roster == coverage.manifest.jsonl in BOTH directions) and would be wrong
    // anyway: a package panel is not covered by the first-party a11y scan. e13f owns the
    // registration; e13c-4 (below) owns the consent that must precede it.

    // --- the install-consent surface + the persisted grants (M9 e13c-4, design 08 §2-§3) ----------
    //
    // Constructing the host LOADS the grant document and APPLIES it to the scanned contributions'
    // `sandbox.granted_scopes`, clamped to each contribution's own manifest declaration
    // (package_grants.h decisions 3 + 4). Before this line every contribution holds least privilege
    // by construction (`read_package_manifest` grants nothing); after it, each holds exactly what the
    // operator consented to and the manifest declared — the intersection, never either alone.
    //
    // The pending prompts are PRINTED for the same reason the store refusals above are: this build
    // has no install verb to ask at, so the editor's own boot is where "this package is asking for
    // more than the read-only baseline, and nobody has answered" becomes visible to a human.
    // editor-core reads the SAME requests over `package.grants.list` and can render them properly.
    shell::PackageGrantHost package_grants(package_scan, shell::package_grants_path());
    for (const shell::GrantDiagnostic& diagnostic : package_grants.diagnostics())
    {
        // NOT on stderr for the first-run `package.grants_absent` case: an editor that has never been
        // asked a consent question is the ordinary state, and reporting it as an error would train an
        // operator to ignore this channel.
        std::FILE* sink =
            diagnostic.error_code == shell::kErrGrantsAbsent ? stdout : stderr;
        std::fprintf(sink, "context_editor: package grants: %s\n", diagnostic.message.c_str());
    }
    for (const shell::PackageConsentRequest& pending :
         shell::pending_consent_requests(package_scan, package_grants.grants()))
    {
        std::printf("context_editor: consent required: %s\n",
                    shell::consent_prompt_line(pending).c_str());
    }
    if (!package_grants.install(bridge))
    {
        std::fprintf(stderr, "context_editor: could not install the package consent surface\n");
        return 1;
    }

    // --- the per-package BASELINE daemon sessions (e13c-1, design 04 §5 / 08 §2) -------------------
    //
    // The route a package panel's `bridge.call` lands on. Every session it opens holds the
    // `read_query` BASELINE and nothing else (package_sessions.h § control 1), which is what makes a
    // panel's `set` / `build` refused BY THE DISPATCHER — the e13 DoD line — rather than by anything
    // on this side of the wire.
    //
    // ⚠ A SEPARATE `client::Client` PER PACKAGE, deliberately NOT `lifecycle.client()`. That one is
    // the SHELL's session and carries `kShellScope` ("read,write,session"): forwarding a panel's call
    // over it would hand untrusted third-party code the Shell's own write + session grants, which is
    // the whole failure this task exists to prevent. The sessions are LAZY (a package that never calls
    // costs no connection) and sub-capped at `kMaxPackageSessions` of the daemon's 16 (control 4).
    //
    // The factory does NOT attach — PackageSessionHost does, so the scope has exactly one decision
    // site. `connect_to_project` retains the discovered D20 token, which `attach()` falls back to, so
    // no credential passes through this lambda.
    //
    // Installed even OUTSIDE project mode (like the session bridge above): the welcome screen boots
    // the same bundle, and an uninstalled method is a deny-by-default `unknown_method` on a channel
    // whose smokes forbid refusals. With no project the factory simply fails and the call is refused
    // `panel.daemon.unavailable`, which is the honest state of a Shell with no daemon.
    //
    // ⚠ THE SCOPE IS NO LONGER A CONSTANT (M9 e13c-4). The second argument is the grant-derived
    // resolver control 1 now names: it reads the OPERATOR's persisted consent for that package,
    // already clamped to the package's manifest, and turns it into the `AttachOptions::scope` spec.
    // A package nobody consented to resolves to `kPackageSessionScope` — the same baseline e13c-1
    // hardcoded — so the deny-all behaviour is the floor rather than a special case.
    shell::PackageSessionHost package_sessions(
        [project = options.project](std::string& error) -> std::unique_ptr<client::Client>
        { return client::Client::connect_to_project(project, 5000, error); },
        [&package_grants](const std::string& package_id) -> std::string
        { return package_grants.attach_scope_spec_for(package_id); });
    if (!package_sessions.install(bridge))
    {
        std::fprintf(stderr, "context_editor: could not install the package daemon-session surface\n");
        return 1;
    }

    lifecycle.set_subscription_topics({shell::panels::kDiagnosticsTopic,
                                       shell::panels::kDerivationTopic,
                                       shell::panels::kSessionTopic});
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
            // The session feed is deliberately NOT bound here: an attach hook can only ever tell it
            // about a NEW client, never about a destroyed one, and the destroyed one is the hazard
            // (session_feed.h § LIFETIME). The owner loop below re-derives the binding from the
            // lifecycle instead, which covers attach, reattach AND detach in one place.
        });
    // 0 is only the FALLBACK generation stamp for a snapshot that carries none of its own; the real
    // cursor snapshot always does and apply_snapshot prefers it.
    // e09e-3: ONE fact -> EVERY live window's bag. Both handlers below dispatch window 0's bag first
    // (it is a local, always present) and then each live secondary window's, through the SAME free
    // seams — see SecondaryWindowSurfaces' header for why the deferral that used to live there is
    // retired and why the events come from this ONE subscription rather than from each window's own.
    const auto for_each_secondary_bag = [&secondary_feeds, &manager](auto&& visit)
    {
        for (const SecondaryFeed& entry : secondary_feeds)
        {
            // LIVE only: a destroyed window's session is retired rather than freed, so its bag is
            // still allocated and would otherwise keep being fed after the window is gone.
            if (manager.window(entry.id) == nullptr)
                continue;
            if (const std::shared_ptr<SecondaryWindowSurfaces> surfaces = entry.surfaces.lock())
                visit(*surfaces, entry.client);
        }
    };
    lifecycle.on_snapshot(
        [feed, &for_each_secondary_bag](const std::string&, const contract::Json& snapshot)
        {
            if (feed != nullptr)
                shell::panels::apply_problems_snapshot(*feed, snapshot, 0);
            for_each_secondary_bag(
                [&snapshot](SecondaryWindowSurfaces& surfaces, client::Client*)
                {
                    if (surfaces.builtin.problems != nullptr)
                        shell::panels::apply_problems_snapshot(*surfaces.builtin.problems, snapshot,
                                                               0);
                });
        });
    lifecycle.on_event(
        [feed, tree_feed, files_feed, session_feed, inspector_feed, &for_each_secondary_bag](
            const std::string&, const client::ClientEvent& event)
        {
            if (feed != nullptr)
                (void)shell::panels::apply_problems_event(*feed, event.topic, event.payload,
                                                          event.generation);
            if (tree_feed != nullptr)
                (void)shell::panels::apply_scenetree_event(*tree_feed, event.topic, event.payload,
                                                           event.generation);
            // M9 e1: the Files panel re-fetches on the SAME `derivation.settled` signal (a write
            // anywhere — including this window's own — can create/move/delete a file).
            if (files_feed != nullptr)
                (void)shell::panels::apply_files_event(*files_feed, event.topic, event.payload,
                                                       event.generation);
            // e09e-2: the fan-out half. A `false` here is ordinary — the feed WITHHOLDS the re-read
            // while a gesture is staged, so the human's in-flight edit and its L-30 CAS base survive
            // a concurrent writer's settle (inspector_feed.h § apply_event).
            if (inspector_feed != nullptr)
                (void)shell::panels::apply_inspector_event(*inspector_feed, event.topic,
                                                           event.payload);
            // e08b: the `session` facts (another client's selection / play state). The feed itself
            // drops the echo of our own writes, so nothing here needs to know about `origin`.
            if (session_feed != nullptr)
                (void)shell::panels::apply_session_event(*session_feed, event.topic, event.payload);
            // …and the same four, for every OTHER live window. This is design 05 §8's tail: a fact is
            // a fact, so each window's own feeds decide what to do with it (the Inspector's
            // mid-gesture deferral is per-bag state, which is exactly why it must be asked per bag).
            for_each_secondary_bag(
                [&event](SecondaryWindowSurfaces& surfaces, client::Client*)
                {
                    shell::panels::BuiltinPanels& bag = surfaces.builtin;
                    if (bag.problems != nullptr)
                        (void)shell::panels::apply_problems_event(*bag.problems, event.topic,
                                                                  event.payload, event.generation);
                    if (bag.scenetree != nullptr)
                        (void)shell::panels::apply_scenetree_event(*bag.scenetree, event.topic,
                                                                   event.payload, event.generation);
                    if (bag.files != nullptr)
                        (void)shell::panels::apply_files_event(*bag.files, event.topic, event.payload,
                                                               event.generation);
                    if (bag.inspector != nullptr)
                        (void)shell::panels::apply_inspector_event(*bag.inspector, event.topic,
                                                                   event.payload);
                    if (bag.session != nullptr)
                        (void)shell::panels::apply_session_event(*bag.session, event.topic,
                                                                 event.payload);
                });
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
        // e13c-3: what the user has actually installed, plus the store it came from. BOTH, always
        // together — an empty store root refuses every mount by design, so these two lines are one
        // decision and must not drift apart.
        cef_options.ext_packages = shell::package_mounts(package_scan);
        cef_options.ext_store_root = package_store;
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
    // the bridge handlers — see there). It becomes `kPrimaryWindowId` — window 0, which hosts the
    // app menu + welcome screen (D13) and is the one window the registry refuses to destroy.
    //
    // It is adopted with an EMPTY session on purpose: window 0's bridge is the `bridge` local above
    // and its daemon connection belongs to the lifecycle, both of which already outlive
    // `shell::cef::shutdown()` by declaration order. Only a window the REGISTRY created hands its
    // session to the manager (see the factory below).
    manager.add(std::move(window));

    // --- the window factory (e10a, design 03 §1) --------------------------------------------------
    //
    // HOW A SECOND NATIVE WINDOW IS BUILT. The registry owns WHEN; this owns HOW, because only the
    // app knows how to make a browser. Nothing triggers it yet — the tear-out gesture that asks for a
    // window is e10b's, and this task deliberately moves no panel — but every seam it needs is here
    // and exercised by `editor-cef-smoke-shell-multiwindow`.
    //
    // Each window gets its OWN of everything that carries identity:
    //   * its own `BridgeRouter` + handshake, so a fresh editor-core instance boots into it (03 §1 —
    //     not a shared instance, not `retainContext`), and so a handler can always tell which window
    //     asked (the ambiguity e10b's tear-out would otherwise have to invent an answer for);
    //   * its own WIRE CONNECTION to the daemon, and therefore its own `origin` (e08a mints ids per
    //     connection, so N windows are genuinely N origins — what makes e10c/e10d's cross-window
    //     echo-suppression drills mean anything).
    //
    // Ownership of both is handed to the manager, which retires rather than frees them (shell.h) —
    // the CE #319 rule, now applying to a MID-PROCESS destroy as well as the process-wide one.
    manager.bind_window_factory(
        [&options, project_mode, &manager, &move_store, &drag_store, &mirror_store,
         &secondary_feeds](const shell::WindowSpec& spec, shell::WindowSessionParts& parts,
                           std::string& error) -> bool
        {
            shell::WindowDesc desc;
            desc.title = spec.title;
            desc.logical_size = spec.logical_size;
            desc.visible = !spec.headless;
            desc.placement = spec.placement;
            if (spec.headless)
            {
                parts.backend = std::make_unique<shell::HeadlessWindowBackend>(desc);
            }
            else
            {
                shell::WindowBackendSelection selection = shell::make_window_backend(desc);
                if (selection.backend == nullptr)
                {
                    // REPORTED as a create failure rather than degraded to headless: an invisible
                    // second window is the silent failure 03 §7 exists to prevent, and the caller's
                    // fallback (e10b's floating group in the SOURCE window) is the right answer.
                    error = selection.diagnostic.empty()
                                ? std::string("no native window backend on this platform")
                                : selection.diagnostic;
                    return false;
                }
                parts.backend = std::move(selection.backend);
            }

            // e10b: ITS OWN router + the FULL boot surface (not just a handshake as in e10a), so a
            // torn-out panel is genuinely LIVE in this window. Installed against the id the registry is
            // ABOUT to mint (last_minted_id()+1 on a fresh create — the multiwindow smoke's proven
            // pattern), so region routing + `window.seed` resolve to THIS window.
            auto window_bridge_router = std::make_unique<shell::BridgeRouter>();
            const shell::WindowId expected_id =
                static_cast<shell::WindowId>(manager.last_minted_id() + 1u);
            auto surfaces = std::make_shared<SecondaryWindowSurfaces>(expected_id, move_store);
            if (!surfaces->install(*window_bridge_router, manager, move_store, drag_store,
                                   mirror_store, expected_id, spec.headless))
            {
                error = "a bridge surface refused to install on the new window";
                return false;
            }

            if (project_mode)
            {
                shell::DaemonAttach attach = shell::attach_to_project(options.project);
                if (attach.attached)
                {
                    parts.daemon_client = std::move(attach.client);
                }
                else
                {
                    // NOT a create failure: a window that opens read-only is exactly what 03 §7
                    // prescribes for a lost daemon, and the primary window does the same. Its origin
                    // is then 0 — which e08a also spells "not attached", so nothing reads it as an
                    // identity.
                    parts.diagnostic = "this window opened without its own daemon connection (" +
                                       attach.error + "); it has no origin of its own";
                }
            }

#if defined(CONTEXT_EDITOR_HAS_CEF)
            shell::cef::CefShellOptions cef_options;
            cef_options.native_window = parts.backend->native_window().handle;
            cef_options.logical_size =
                shell::to_logical(parts.backend->client_size(), parts.backend->dpi());
            cef_options.dpi = parts.backend->dpi();
            cef_options.url = options.url;
            cef_options.devtools_enabled = options.devtools;
            cef_options.app_asset_root = options.app_root;
            cef_options.bridge = window_bridge_router.get();
            std::string browser_error;
            parts.browser = shell::cef::make_cef_browser_host(cef_options, browser_error);
            if (parts.browser == nullptr)
            {
                error = "the browser did not start: " + browser_error;
                return false;
            }
#else
            parts.browser = std::make_unique<shell::ScriptedBrowserHost>();
#endif
            // e09e-3: record this window's bag + its own daemon connection so the lifecycle's event
            // dispatch and the owner loop reach it (see `secondary_feeds`). Taken BEFORE the two moves
            // below, which is the only point both concrete types are in hand — and the raw client
            // pointer is read off the unique_ptr the session is ABOUT to own, whose object the registry
            // retires rather than frees.
            secondary_feeds.push_back(
                SecondaryFeed{expected_id, surfaces, parts.daemon_client.get()});
            // The full surface bundle goes in as ONE type-erased surface (it owns the handshake +
            // panel host + every bridge, incl. this window's WindowBridge); it must outlive the router
            // that captured it, which WindowSessionParts' member order guarantees (window_registry.h
            // § LIFETIME RULE — surfaces are destroyed AFTER the bridge).
            parts.surfaces.push_back(std::move(surfaces));
            parts.bridge = std::move(window_bridge_router);
            error.clear();
            return true;
        });

    // The 03 §7 degradation seam. e10a reports; e10b decides what editor-core does about it (fall
    // back to a floating Dockview group in the source window). The registry already logs the same
    // line, so this is the hook, not the report.
    manager.on_window_create_failed(
        [](const shell::WindowCreateFailure& failure)
        {
            std::fprintf(stderr, "context_editor: %s\n", shell::describe(failure).c_str());
        });

    // --- the cross-window drag session (e10c, design 04 §2) ---------------------------------------
    //
    // The Shell-mediated drag of a panel BETWEEN windows: once a drag leaves a window's bounds the Shell
    // tracks the global cursor, targets the window under it, queries THAT window's editor-core for its
    // drop zone over the bridge (`drag.probe` / `drag.report-zone`, already live on EVERY window above),
    // and on drop rehomes through e10b's EXISTING move path — no third recreate mechanism (D6).
    //
    // The seam is ASSEMBLED here — the window-under-cursor + screen->local resolvers, and the e10b drop
    // path — but the interactive GESTURE that drives it (begin on drag-leaves-bounds, update on the
    // global cursor, drop / Escape, and the OS cursor capture the session takes at `begin`) rides the
    // deferred interactive-Windows pass (docs/shell.md), for the same reason e10a wired the window
    // factory with no trigger and the profile defers every interactive-Windows drill: a Session-0 CI
    // runner cannot drive a real global-cursor drag. The session's SAFETY-CRITICAL invariant (the
    // capture is released on EVERY exit path) is proven by `editor-shell-test_cross_window_drag`; the
    // cross-origin round trip by `editor-cef-smoke-shell-drag`.
    shell::CrossWindowDragSession drag_session(drag_store);
    drag_session.bind_window_at_point(
        [&manager](shell::PointI screen) -> shell::WindowId
        {
            // The last (most recently created) window whose remembered screen rect contains the point —
            // a reasonable stacking proxy until the interactive pass reads true OS z-order.
            shell::WindowId hit = shell::kInvalidWindowId;
            for (const shell::WindowId id : manager.window_ids())
            {
                shell::EditorWindow* candidate = manager.window(id);
                if (candidate == nullptr)
                {
                    continue;
                }
                const shell::WindowPlacement placement = candidate->last_placement();
                const std::int32_t right = placement.x + static_cast<std::int32_t>(placement.width);
                const std::int32_t bottom = placement.y + static_cast<std::int32_t>(placement.height);
                if (screen.x >= placement.x && screen.x < right && screen.y >= placement.y &&
                    screen.y < bottom)
                {
                    hit = id;
                }
            }
            return hit;
        });
    drag_session.bind_to_local(
        [&manager](shell::WindowId target, shell::PointI screen) -> shell::PointI
        {
            if (shell::EditorWindow* window = manager.window(target))
            {
                const shell::WindowPlacement placement = window->last_placement();
                return shell::PointI{screen.x - placement.x, screen.y - placement.y};
            }
            return screen;
        });
    drag_session.bind_drop(
        [&manager, &move_store](shell::WindowId target, const shell::PanelSeed& seed) -> bool
        {
            // e10b's EXISTING rehome path — the SAME `enqueue_rehome` a `window.move-to` uses, drained
            // by the target window's editor-core on its `window.rehomed` poll. NOT a third recreate
            // mechanism (D6): a new one emerging here would be a REPORTABLE finding, not this line.
            if (manager.window(target) == nullptr)
            {
                return false;
            }
            move_store.enqueue_rehome(target, seed);
            return true;
        });

    // --- the owner loop ----------------------------------------------------------------------------
    int frames = 0;
    while (manager.pump_once(now_us()))
    {
        ++frames;
        // Drive the daemon link (e14a / 03 §7): pump the live feed, and on a lost daemon enter the
        // read-only STATE + reconnect with backoff, re-snapshotting on reattach. A daemon restart is
        // stalled at most the ladder's bounded duration. now in ms for the backoff clock.
        lifecycle.pump(static_cast<std::int64_t>(now_us() / 1000));
        // e13c-2: drain every package session's pushed daemon events into their BOUNDED buffers.
        //
        // ⚠ EVERY FRAME, NOT ONLY WHEN editor-core POLLS — that is the whole reason the bound is real.
        // Between two renderer polls the frames otherwise pile up in `Client::pending_events_`, an
        // UNBOUNDED deque, so a buffer filled only on demand would leave the actual accumulation point
        // unbounded and `kMaxBufferedEventsPerPackage` cosmetic (package_sessions.h § pump).
        // Non-blocking and per-session frame-capped, so a quiet editor pays one 0 ms read per open
        // package session and a chatty topic cannot hold the frame.
        (void)package_sessions.pump();
        // e08b: re-derive the session feed's non-owning client view from the lifecycle, immediately
        // after the ONE call that can change it. Attach and reattach hand it the new client (with the
        // new per-connection echo-suppression id); a lost daemon — which DESTROYS the client inside
        // that same pump — clears it, so the next frame's renderer-driven panel write refuses
        // honestly instead of calling through a freed pointer (session_feed.h § LIFETIME).
        if (session_feed != nullptr)
            shell::panels::bind_session_client(*session_feed, lifecycle.client());
        // e09b-2: the SAME re-derive for the Inspector's wire override-write gateway, for the SAME
        // reason and at the SAME point — a gesture commit is renderer-driven, so it can land in the
        // window between "the daemon died" (which DESTROYS the client inside the pump above) and
        // "we reattached". Re-pointing here is what makes the next commit refuse honestly instead of
        // writing through a freed pointer (wire_override_gateway.h § LIFETIME).
        shell::panels::bind_write_client(builtin, lifecycle.client());
        // e09c: offer the session undo journal to the editor-state store — the ONE Shell-side seam
        // that file is written through (C-F3). Free while nothing changed (an unmoved journal is not
        // even re-serialized), and the store's debounce turns a burst of gestures into one crash-safe
        // write per quiet period. Placed in the loop rather than at exit deliberately: a crash must
        // not cost the user their undo history, which is exactly what a save-on-exit-only would do.
        (void)shell::panels::publish_undo_state(builtin, manager.state_store(), now_us());
        // e10a: keep window 0's reported `origin` in step with the connection it actually has. The
        // primary's client belongs to the lifecycle rather than to a registry session, so the
        // registry is TOLD rather than asking — and a reconnect mints a NEW id (e14a), which is
        // exactly why this is re-derived here, immediately after the one call that can change it,
        // rather than latched once at boot.
        manager.set_window_origin(shell::kPrimaryWindowId,
                                  lifecycle.client() != nullptr ? lifecycle.client()->client_id()
                                                                : 0);
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
        // e09e-3: the SAME three per-frame steps for every OTHER live window — its bag's non-owning
        // client views re-derived, then its due hydration work drained. Each secondary window reads and
        // writes over its OWN connection (`entry.client`, the one the factory gave it), so its `origin`
        // and its writes are genuinely its own; only the EVENTS are shared, from the one subscription
        // above. Cost while nothing is due is a claimed-fetch check per window, the same as window 0's.
        //
        // A window with NO connection of its own (`attach_to_project` failed — 03 §7's read-only
        // degrade, which the factory reports rather than treating as a create failure) is UNBOUND here
        // rather than skipped: leaving a stale client bound is what turns a lost daemon into a
        // use-after-free, and an unbound gateway refuses honestly (wire_override_gateway.h § LIFETIME).
        for_each_secondary_bag(
            [&options](SecondaryWindowSurfaces& surfaces, client::Client* window_client)
            {
                shell::panels::bind_write_client(surfaces.builtin, window_client);
                if (surfaces.builtin.session != nullptr)
                    shell::panels::bind_session_client(*surfaces.builtin.session, window_client);
                if (window_client != nullptr)
                    shell::panels::pump_panel_feeds(surfaces.builtin, *window_client, options.scene);
            });
        // e07c: watch the per-user keybindings file. A cheap stat gates the re-read, so an unchanged
        // file costs one stat per loop; a change bumps the generation editor-core re-reads over the
        // bridge (the hot-reload trigger). The return is intentionally discarded here — the renderer
        // pulls the fresh snapshot on its own cadence over `keybindings.get`.
        (void)keybindings_bridge.poll();
        // e06b: the same watch, one directory wider — a cheap re-enumeration gates the re-reads, so an
        // unchanged theme directory costs one stat per file per loop and a real edit bumps the
        // generation editor-core re-reads over `themes.get`. That is what makes editing a theme file
        // repaint the live editor with no restart (design 06 §4's "instant feedback").
        (void)themes_bridge.poll();
        // e06d: the same cheap-stat watch over the per-user config. It matters for MULTI-WINDOW: a
        // theme picked in one editor window is written by that window's Shell, and this poll is how
        // THIS window notices, so `config.get` serves the current document rather than the one it read
        // at boot.
        (void)user_config.poll();
        // e14d: adopt the update check's result the frame it lands. Cheap — one integer compare while
        // nothing is pending, and it runs at most once per launch (the check is one-shot).
        (void)release_notice.poll();
        if (options.max_frames > 0 && frames >= options.max_frames)
        {
            break;
        }
        // A short sleep keeps an idle editor off a spin loop. The compositor is damage-driven, so an
        // idle frame costs nothing beyond this wait; a real vsync-paced loop arrives with the frame
        // scheduler, not here.
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    // e08b: clear the session feed's client view BEFORE the lifecycle frees the client below. The
    // teardown that follows still PUMPS — manager.shutdown() closes the browser, which drives CEF —
    // so a renderer message queued before exit can still reach a panel provider and issue a write.
    // Unbinding first is what makes that write refuse instead of calling a destroyed Client; the
    // ordering is the whole point, so it must precede shutdown_at_exit (session_feed.h § LIFETIME).
    if (session_feed != nullptr)
        shell::panels::bind_session_client(*session_feed, nullptr);
    // e09b-2: clear the write gateway's client view for the SAME reason and BEFORE the same call —
    // the teardown below still PUMPS, so a renderer message queued before exit can still reach the
    // Inspector's gesture provider and issue a commit. Unbinding first is what makes that commit
    // refuse instead of calling a destroyed Client.
    shell::panels::bind_write_client(builtin, nullptr);
    // e09e-3: the SAME unbind for every OTHER window's bag, BEFORE the same call and for the same
    // reason. A secondary window's Inspector can commit too since this task wired its gateway, so it
    // has exactly window 0's exposure to a renderer message queued before exit.
    //
    // ⚠ DELIBERATELY NOT `for_each_secondary_bag`, which is LIVE-gated. That gate is right for feeding
    // and pumping — a destroyed window must stop being fed — but WRONG here: a mid-process destroy
    // RETIRES a session rather than freeing it (the CE #319 rule, shell.h § RetiredSession), and retire
    // clears the compositor sink WITHOUT touching the bag, so a retired window's Inspector gateway is
    // still bound to its still-live `Client`. Skipping it would leave exactly the binding these two
    // calls exist to remove, on the one bag nobody will look at again, across `manager.shutdown()`'s
    // all-closing CEF drain. Before this task secondary bags were bound to no client at all, so this
    // exposure is one THIS task introduced; the unbind is idempotent, so covering live and retired
    // windows alike is strictly safer and costs a pointer store each.
    for (const SecondaryFeed& entry : secondary_feeds)
    {
        if (const std::shared_ptr<SecondaryWindowSurfaces> surfaces = entry.surfaces.lock())
        {
            shell::panels::bind_write_client(surfaces->builtin, nullptr);
            if (surfaces->builtin.session != nullptr)
                shell::panels::bind_session_client(*surfaces->builtin.session, nullptr);
        }
    }
    // e13c-1: close THIS process's package sessions before the daemon teardown below, the same
    // ordering rule as the two unbinds above — they are our own connections and the daemon should not
    // have to reap them from an exiting process.
    //
    // ⚠ THIS DOES NOT CORRECT THE EXIT CENSUS, AND MUST NOT BE READ AS DOING SO. Each package session
    // attached as its own client, so each published `{event:"attached"}` on the `clients` topic that
    // `ClientCensus` counts, and `others()` is `count_ - 1`. Closing the sockets here makes the daemon
    // publish `detached`, but `shutdown_at_exit()` reads `census_.others()` SYNCHRONOUSLY on its very
    // first line (daemon_lifecycle.cpp) and nothing pumps the subscription in between — so the
    // `detached` events are never observed before the decision is taken. NET EFFECT, MEASURED FROM THE
    // SOURCE AND NOT YET FIXED: once any package panel has made one `bridge.call`, `others() >= 1`,
    // `decide_daemon_exit_action` returns `leave_running` instead of `shutdown_owned`, and this
    // process DETACHES its own spawned daemon rather than shutting it down — one orphaned daemon per
    // editor session, holding a socket, an instance token and slots out of the 16-connection budget
    // package_sessions.h § control 4 exists to protect. The fix belongs in the exit policy (teach it
    // this process's OWN extra client count, e.g. an `others()` subtrahend fed
    // `package_sessions.sessions_open()`), NOT here: subtracting wrongly would shut down a daemon a
    // CLI or AI client is still using, which is a worse failure than leaking one. Tracked as e13c-1
    // follow-up; see the PR body.
    package_sessions.reset();
    // The exit policy (e14a): an owned daemon this process is the last client of gets a clean in-band
    // `shutdown`; an owned daemon other clients still hold is left running; an external daemon is never
    // touched. Runs before the manager/CEF teardown so the daemon call still has a live wire.
    lifecycle.shutdown_at_exit();
    // e09c: one last journal publish before the store is flushed. The loop's per-frame offer runs at
    // the TOP of a frame, so a gesture that committed during the final frame would otherwise never
    // reach disk — and losing the last thing the user did is precisely the failure this whole task
    // exists to prevent. Idempotent: a journal already published is not dirty and this is a no-op.
    (void)shell::panels::publish_undo_state(builtin, manager.state_store(), now_us());
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
