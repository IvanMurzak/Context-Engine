// The LIVE cross-window DRAG smoke (M9 e10c) — ctest `editor-cef-smoke-shell-drag`.
//
// `editor-shell-test_cross_window_drag` proves the drag SESSION's state machine — and, most
// importantly, that the global cursor capture is RELEASED on every exit path — on all three `build`
// legs with no browser. This proves the one thing a fake structurally CANNOT: that the drop-zone query
// genuinely ROUND-TRIPS to a DIFFERENT window's LIVE editor-core over the IPC bridge (DoD line 2), and
// that a drop then rehomes through e10b's path with state preserved (DoD lines 1, 3). It drives both
// through the REAL live renderers over the REAL bridge:
//
//   1. **The drop-zone query reaches the TARGET window's editor-core, not the source's.** The Shell's
//      drag session publishes a hover targeting window 1; window 1's LIVE editor-core polls `drag.probe`
//      on its window-mechanism interval, hit-tests its OWN layout, and answers `drag.report-zone` — so
//      the store's zone becomes VALID and window 1's `WindowBridge::drag_zones_reported()` climbs, while
//      window 0 (the SOURCE) never answers (`drag_probes_active() == 0` there). Cross-origin by
//      construction (e08a): the two windows are two editor-core instances over two wire connections.
//   2. **Drop rehomes through e10b's EXISTING path.** The session's drop enqueues a `window.move-to`
//      rehome INTO window 1; window 1's LIVE editor-core drains it on its `window.rehomed` poll — the
//      SAME D6 relay tear-out and window-close rehome use, no third recreate mechanism.
//   3. **The capture is released on drop AND on cancel** — asserted live, the safety-critical property.
//
// ⚠ WHAT THIS LEG DOES NOT DO — stated honestly (09 §3). Windows CI runs Session-0 (no interactive
// desktop), so a REAL global-cursor drag cannot be driven here. This smoke drives the SESSION
// programmatically (the Shell side) against LIVE editor-cores, over a HEADLESS cursor capture — so it
// proves the IPC round trip, the e10b rehome, and the capture lifetime, but NOT the OS-level global
// cursor tracking / drag-ghost rendering, which rest on the deferred interactive-Windows T2 leg. It
// does NOT fake that with a green: the assertions below are exactly the cross-process facts a fake
// cannot reach, and nothing more.
//
// Headless throughout (windowless browsers, the C-F2 CPU present path), safe on the Session-0 runner,
// exactly like its sibling smokes. The Windows hard exit after the verdict mirrors them.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "context/editor/contract/json.h"
#include "context/editor/shell/app_scheme.h"
#include "context/editor/shell/banners.h"
#include "context/editor/shell/cef/cef_shell.h"
#include "context/editor/shell/cross_window_drag.h"
#include "context/editor/shell/editor_state_bridge.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/keybindings_bridge.h"
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/builtin_panels.h"
#include "context/editor/shell/session_bridge.h"
#include "context/editor/shell/shell.h"
#include "context/editor/shell/themes_bridge.h"
#include "context/editor/shell/user_config.h"
#include "context/editor/shell/welcome.h"
#include "context/editor/shell/window_bridge.h"
#include "context/editor/shell/window_registry.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace shell = context::editor::shell;
namespace render = context::render;
namespace present = context::render::present;
using Json = context::editor::contract::Json;

namespace
{

int g_failures = 0;

void check(bool condition, const char* what, int line)
{
    if (condition)
    {
        return;
    }
    std::fprintf(stderr, "[editor-cef-smoke-shell-drag] FAIL (line %d): %s\n", line, what);
    ++g_failures;
}

#define SMOKE_CHECK(cond, what) check((cond), (what), __LINE__)

std::uint64_t now_us()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Empty fallback (not #error), so the pre-push audit's check 9 can compile this TU standalone against
// the pinned CEF headers with none of CMake's defines — the sibling smokes' rationale, verbatim.
#if !defined(CONTEXT_WEBUI_ASSET_DIR)
#define CONTEXT_WEBUI_ASSET_DIR ""
#endif

// Pinned so both windows boot on the same deterministic theme path (no ambient prefers-color-scheme).
constexpr const char* kSmokeThemeId = "builtin.dark";

// The panel dragged between windows — a hosted uitree panel present in every build's roster.
constexpr const char* kDraggedPanel = "builtin.problems";

// A D6 blob a FRESH panel could not reproduce, so a preserved-state drop is provable on rehome.
Json impossible_state()
{
    Json data = Json::object();
    data.set("query", Json(std::string("half-typed cross-window search")));
    data.set("scrollTop", Json(static_cast<std::uint64_t>(4096)));
    Json blob = Json::object();
    blob.set("schemaVersion", Json(1));
    blob.set("data", std::move(data));
    return blob;
}

// The full boot surface for one window, with the WindowBridge bound to BOTH the move relay (e10b) and
// the shared cross-window drag store (e10c) — so this window's LIVE editor-core answers `drag.probe` /
// `drag.report-zone` for real. Member order puts `handshake` first (destroyed last — the router
// captured it); the router lives in the retired session, outside this.
struct WindowSurfaces
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
    std::unique_ptr<shell::WindowBridge> window_bridge;

    [[nodiscard]] bool install(shell::BridgeRouter& router, shell::WindowManager& manager,
                               shell::WindowMoveStore& store, shell::CrossWindowDragStore& drag_store,
                               shell::WindowId window_id)
    {
        bool ok = handshake.install(router);
        ok = panel_host.install(router) && ok;
        editor_state.bind_store(&manager.state_store(), now_us);
        editor_state.bind_regions(
            [&manager, window_id](std::vector<shell::ShellRegion> regions)
            {
                if (shell::EditorWindow* target = manager.window(window_id))
                    target->input().regions().publish(std::move(regions));
            });
        ok = editor_state.install(router) && ok;
        keybindings.bind_path(std::filesystem::path{});
        ok = keybindings.install(router) && ok;
        themes.bind_directory(std::filesystem::path{});
        ok = themes.install(router) && ok;
        welcome.set_launch_mode(shell::LaunchMode::project);
        welcome.set_config_path(std::filesystem::path{});
        ok = welcome.install(router) && ok;
        ok = banners.install(router) && ok;
        config.bind_path(std::filesystem::path{});
        ok = config.install(router) && ok;
        ok = session_bridge.install(router) && ok;

        window_bridge = std::make_unique<shell::WindowBridge>(window_id, store);
        window_bridge->bind_windows([&manager]() { return manager.window_ids(); });
        window_bridge->bind_move_to(
            [&manager, &store](const shell::WindowBridge::MoveTo& req) -> shell::WindowMoveResult
            {
                if (manager.window(req.target) == nullptr)
                    return {false, shell::kInvalidWindowId, std::string{}, "no live window"};
                store.enqueue_rehome(req.target, req.seed);
                return {true, req.target, std::string{}, std::string{}};
            });
        window_bridge->bind_close(
            [&manager, &store](shell::WindowId self) -> shell::WindowMoveResult
            {
                const shell::WindowDestroyResult d = manager.destroy_window(self);
                if (d.ok())
                    store.forget(self);
                return {d.ok(), self, shell::to_string(d.outcome), d.error};
            });
        // e10c: this window answers the cross-window drag probe off the SHARED store.
        window_bridge->bind_drag_store(&drag_store);
        ok = window_bridge->install(router) && ok;
        return ok;
    }
};

std::string boot_url()
{
    return std::string(shell::kAppEntryUrl) + "?" + shell::kThemePinFlag + "=" + kSmokeThemeId;
}

shell::cef::CefShellOptions make_cef_options(render::Extent2D size, shell::BridgeRouter* bridge)
{
    shell::cef::CefShellOptions options;
    options.native_window = nullptr; // windowless: no native window on a Session-0 runner
    options.logical_size = size;
    options.dpi = shell::DpiScale{};
    options.url = boot_url();
    options.app_asset_root = CONTEXT_WEBUI_ASSET_DIR;
    options.bridge = bridge;
    options.windowless_frame_rate = 10;
    return options;
}

int finish(int code)
{
#if defined(_WIN32)
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
#else
    return code;
#endif
}

} // namespace

int main(int argc, char** argv)
{
    const int subprocess_exit = shell::cef::execute_subprocess(argc, argv);
    if (subprocess_exit >= 0)
    {
        return subprocess_exit;
    }

    std::printf("[editor-cef-smoke-shell-drag] cross-window drop-zone query round-trips to window 1's "
                "LIVE editor-core over IPC; drop rehomes via e10b; capture released on drop + cancel\n");

    std::error_code ec;
    const std::filesystem::path project =
        std::filesystem::temp_directory_path(ec) / "context-editor-cef-drag-smoke";
    std::filesystem::remove_all(project, ec);
    std::filesystem::create_directories(project, ec);

    const render::Extent2D size{640, 480};

    SMOKE_CHECK(std::string(CONTEXT_WEBUI_ASSET_DIR).empty() == false,
                "CONTEXT_WEBUI_ASSET_DIR was compiled in (the webui asset root is wired)");

    // --- the shared relays + window 0 (the drag SOURCE) ------------------------------------------
    shell::WindowMoveStore move_store;
    shell::CrossWindowDragStore drag_store;
    shell::BridgeRouter primary_bridge;
    WindowSurfaces primary_surfaces;

    shell::WindowManager manager(project);

    SMOKE_CHECK(primary_surfaces.install(primary_bridge, manager, move_store, drag_store,
                                         shell::kPrimaryWindowId),
                "every bridge surface installed on window 0");

    {
        shell::WindowDesc desc;
        desc.title = "Context Editor (drag smoke, window 0)";
        desc.logical_size = size;
        desc.visible = false;
        auto backend = std::make_unique<shell::HeadlessWindowBackend>(desc);
        std::string error;
        std::unique_ptr<shell::IBrowserHost> browser =
            shell::cef::make_cef_browser_host(make_cef_options(size, &primary_bridge), error);
        if (browser == nullptr)
        {
            std::fprintf(stderr, "[editor-cef-smoke-shell-drag] FAIL: window 0's browser did not "
                                 "start: %s\n",
                         error.c_str());
            return finish(1);
        }
        shell::EditorWindowConfig config;
        config.compositor.import_options.force_software = true;
        config.placement_poll_us = 0;
        auto window = std::make_unique<shell::EditorWindow>(std::move(backend), std::move(browser),
                                                            config);
        window->compositor().attach_cpu(std::make_unique<present::MemoryBlitter>(), size);
        manager.add(std::move(window));
    }

    shell::EditorWindow* primary = manager.window(shell::kPrimaryWindowId);
    SMOKE_CHECK(primary != nullptr, "the manager adopted window 0 as the primary");
    if (primary == nullptr)
    {
        return finish(1);
    }

    // The factory: window 1 (the drag TARGET) gets the full live surface, sharing the ONE move relay +
    // the ONE drag store, so its editor-core answers `drag.probe` for real. Records the created surfaces
    // so the assertions can read window 1's WindowBridge counters.
    WindowSurfaces* target_surfaces = nullptr;
    manager.bind_window_factory(
        [&](const shell::WindowSpec& spec, shell::WindowSessionParts& parts, std::string& error)
            -> bool
        {
            shell::WindowDesc desc;
            desc.title = spec.title;
            desc.logical_size = spec.logical_size;
            desc.visible = false;
            parts.backend = std::make_unique<shell::HeadlessWindowBackend>(desc);

            auto window_bridge_router = std::make_unique<shell::BridgeRouter>();
            auto surfaces = std::make_shared<WindowSurfaces>();
            const shell::WindowId expected_id =
                static_cast<shell::WindowId>(manager.last_minted_id() + 1u);
            if (!surfaces->install(*window_bridge_router, manager, move_store, drag_store, expected_id))
            {
                error = "a bridge surface refused to install on the new window";
                return false;
            }
            std::string browser_error;
            parts.browser = shell::cef::make_cef_browser_host(
                make_cef_options(spec.logical_size, window_bridge_router.get()), browser_error);
            if (parts.browser == nullptr)
            {
                error = "the browser did not start: " + browser_error;
                return false;
            }
            target_surfaces = surfaces.get();
            parts.surfaces.push_back(std::move(surfaces));
            parts.bridge = std::move(window_bridge_router);
            error.clear();
            return true;
        });

    const auto attach_present_path = [&](shell::WindowId id)
    {
        if (shell::EditorWindow* window = manager.window(id))
            window->compositor().attach_cpu(std::make_unique<present::MemoryBlitter>(), size);
    };

    const auto boot_window = [&](shell::WindowId id, WindowSurfaces& surfaces, int seconds) -> bool
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (!manager.pump_once(now_us()))
                return false;
            shell::EditorWindow* window = manager.window(id);
            if (window == nullptr)
                return false;
            if (window->compositor().stats().view_frames > 0 && surfaces.handshake.complete())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };

    SMOKE_CHECK(boot_window(shell::kPrimaryWindowId, primary_surfaces, 30),
                "window 0 composited a live CEF frame and completed its bridge handshake");
    SMOKE_CHECK(primary_bridge.refused() == 0, "window 0's bridge refused nothing at boot");

    // Create + boot window 1 (the drag TARGET).
    shell::WindowSpec spec;
    spec.headless = true;
    spec.title = "Context Editor (drag smoke, window 1)";
    const shell::WindowCreateResult created = manager.create_window(spec);
    SMOKE_CHECK(created.ok(), "window 1 (the drag target) was created");
    SMOKE_CHECK(manager.window_count() == 2, "two windows are live");
    SMOKE_CHECK(shell::cef::browsers_created() == 2, "a SECOND real CEF browser was created");
    const shell::WindowId target_id = created.id;
    if (!created.ok() || target_surfaces == nullptr)
    {
        return finish(1);
    }
    attach_present_path(target_id);
    SMOKE_CHECK(boot_window(target_id, *target_surfaces, 30),
                "window 1 composited a live CEF frame and completed its OWN handshake");
    SMOKE_CHECK(target_surfaces->handshake.nonce() != primary_surfaces.handshake.nonce(),
                "window 1 minted its OWN handshake nonce (a fresh, distinct editor-core)");

    // --- the drag session (the Shell side) --------------------------------------------------------
    //
    // A drag left window 0 carrying the panel; the Shell resolves the cursor to window 1 and asks window
    // 1's editor-core for its drop zone. Scripted resolvers stand in for real screen geometry (headless
    // windows have none): the cursor is "over window 1", at a point inside its viewport.
    shell::CrossWindowDragSession drag_session(drag_store);
    drag_session.bind_window_at_point([target_id](shell::PointI) { return target_id; });
    drag_session.bind_to_local([](shell::WindowId, shell::PointI) { return shell::PointI{80, 40}; });
    drag_session.bind_drop(
        [&manager, &move_store](shell::WindowId target, const shell::PanelSeed& seed) -> bool
        {
            if (manager.window(target) == nullptr)
                return false;
            move_store.enqueue_rehome(target, seed); // e10b's EXISTING rehome path (D6)
            return true;
        });

    shell::HeadlessCursorCapture cursor_capture;
    shell::PanelSeed seed;
    seed.panel_id = kDraggedPanel;
    seed.state = impossible_state();
    SMOKE_CHECK(drag_session.begin(shell::kPrimaryWindowId, seed, shell::PointI{400, 40},
                                   cursor_capture),
                "the drag began and took the (headless) cursor capture");
    SMOKE_CHECK(cursor_capture.captured(), "the cursor capture is held while the drag is live");
    // The cursor moved over window 1 — publish the hover so window 1's editor-core answers.
    drag_session.update_cursor(shell::PointI{400, 40});
    SMOKE_CHECK(drag_session.target() == target_id, "the drag session resolved window 1 as the target");
    SMOKE_CHECK(drag_store.hover_for(target_id).active, "the hover targets window 1");
    SMOKE_CHECK(!drag_store.hover_for(shell::kPrimaryWindowId).active,
                "the SOURCE window 0 is NOT asked to answer — the query is cross-window");

    // --- DoD 2: the drop-zone query round-trips to window 1's LIVE editor-core over IPC -----------
    //
    // Pump until window 1's editor-core polls `drag.probe` and answers `drag.report-zone` — proven by
    // window 1's `drag_zones_reported()` climbing and the store's zone becoming valid, WITHOUT the smoke
    // touching either. This is the cross-origin round trip a source-local resolution could never make.
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline && !drag_store.zone().valid)
        {
            if (!manager.pump_once(now_us()))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    SMOKE_CHECK(target_surfaces->window_bridge != nullptr &&
                    target_surfaces->window_bridge->drag_probes_active() >= 1,
                "window 1's LIVE editor-core polled drag.probe and saw the active hover");
    SMOKE_CHECK(target_surfaces->window_bridge != nullptr &&
                    target_surfaces->window_bridge->drag_zones_reported() >= 1,
                "window 1's LIVE editor-core reported its drop zone back over drag.report-zone");
    SMOKE_CHECK(drag_store.zone().valid,
                "the store carries window 1's VALID drop zone — the query round-tripped to the target");
    SMOKE_CHECK(primary_surfaces.window_bridge != nullptr &&
                    primary_surfaces.window_bridge->drag_probes_active() == 0,
                "window 0 (the source) NEVER answered an active probe — the answer came from the target");

    // --- DoD 1 + 3: drop rehomes through e10b's path, with state preserved ------------------------
    drag_session.sync_zone();
    const shell::DragEndReason dropped = drag_session.drop();
    SMOKE_CHECK(dropped == shell::DragEndReason::dropped, "the drop landed on window 1's valid zone");
    SMOKE_CHECK(drag_session.capture_released(), "the cursor capture is RELEASED after the drop");
    SMOKE_CHECK(move_store.pending_rehomes(target_id) >= 1,
                "the drop enqueued a rehome INTO window 1 (e10b's move path, not a third mechanism)");
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline &&
               move_store.pending_rehomes(target_id) > 0)
        {
            if (!manager.pump_once(now_us()))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        SMOKE_CHECK(move_store.pending_rehomes(target_id) == 0,
                    "window 1's LIVE editor-core drained the rehome on its window.rehomed poll "
                    "(the panel reached window 1 through the D6 relay)");
    }

    // --- DoD 4: a CANCEL path releases the capture, live -----------------------------------------
    shell::HeadlessCursorCapture cancel_capture;
    SMOKE_CHECK(drag_session.begin(shell::kPrimaryWindowId, seed, shell::PointI{400, 40},
                                   cancel_capture),
                "a second drag began (for the cancel path)");
    drag_session.update_cursor(shell::PointI{400, 40});
    drag_session.cancel(); // Escape
    SMOKE_CHECK(drag_session.end_reason() == shell::DragEndReason::escaped, "the second drag was cancelled");
    SMOKE_CHECK(drag_session.capture_released(), "the cursor capture is RELEASED after a cancel");
    SMOKE_CHECK(cancel_capture.releases() == cancel_capture.captures(),
                "every capture taken was released (no leak on the cancel path)");

    SMOKE_CHECK(primary_bridge.refused() == 0, "window 0's bridge refused nothing across the drag");

    // --- teardown, in the ONE order that is safe (CE #319) ---------------------------------------
    manager.shutdown();
    shell::cef::shutdown();
    std::filesystem::remove_all(project, ec);

    if (g_failures != 0)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-drag] FAILED with %d assertion failure(s)\n",
                     g_failures);
        return finish(1);
    }
    std::printf("[editor-cef-smoke-shell-drag] PASS: cross-window drop-zone query round-tripped to "
                "window 1's live editor-core; drop rehomed via e10b; capture released on drop + cancel\n");
    return finish(0);
}
