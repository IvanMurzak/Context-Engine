// The LIVE tear-out + rehome smoke (M9 e10b) — ctest `editor-cef-smoke-shell-tearout`.
//
// `editor-shell-test_window_bridge` proves the relay's machinery on all three `build` legs with no
// browser. This proves the three things a fake structurally CANNOT, each a DoD line of e10b — and it
// drives every one through the REAL live renderer over the REAL bridge, so a break here is a live
// runtime bug (a missing seed read, a mishandled bridge reply) that no unit tier can see:
//
//   1. **Tear-out via the bridge produces a second window that RESTORES the moved state.** A
//      `window.tear-out` dispatched on window 0's router (exactly as editor-core's
//      `tearOutActivePanel` does) creates a second native window, seeded with an OPAQUE D6 blob a
//      fresh panel could NOT have. The new window boots its OWN fresh editor-core (a DIFFERENT
//      handshake nonce) and READS its seed — proven because the move relay's `has_boot_seed` drops to
//      false ONLY when the live renderer's `window.seed` consumed it, and the new window's
//      `WindowBridge::seeds_served()` climbs. Recreate over ONE mechanism, no `retainContext`.
//   2. **A create FAILURE is LOUD, not silent** (03 §7). With a factory bound to fail, the same
//      `window.tear-out` answers `created:false` + the `WindowCreateOutcome` token + a reason — the
//      structured result editor-core degrades to a floating group on. A silent success is a DoD fail.
//   3. **Rehome/move delivers to a LIVE window's running editor-core.** A `window.move-to` targeting
//      window 0 enqueues a panel; window 0's live editor-core drains it on its `window.rehomed` poll —
//      proven because the relay's `pending_rehomes(0)` drops to zero without the smoke touching it.
//      This is the SAME relay window-close rehome uses, so "never silently lost" is exercised here.
//
// Headless throughout (windowless browsers, the C-F2 CPU present path), so it is safe on the
// Session-0 self-hosted Windows runner, exactly like its sibling smokes. The Windows hard exit after
// the verdict mirrors them.

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
#include <vector>

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
    std::fprintf(stderr, "[editor-cef-smoke-shell-tearout] FAIL (line %d): %s\n", line, what);
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

// The panel torn out — a hosted uitree panel that exists in every build's roster.
constexpr const char* kMovedPanel = "builtin.problems";

// A D6 blob a FRESH panel could not reproduce: a typed-in query plus a scroll offset. If the relay or
// the live restore dropped it, the round-trip check below would catch it — not a re-rendered-but-empty
// panel (standing lesson #2, "prove don't assert").
Json impossible_state()
{
    Json data = Json::object();
    data.set("query", Json(std::string("half-typed search")));
    data.set("scrollTop", Json(static_cast<std::uint64_t>(4096)));
    Json blob = Json::object();
    blob.set("schemaVersion", Json(1));
    blob.set("data", std::move(data));
    return blob;
}

// ---------------------------------------------------------------- one window's bridge surfaces
//
// The SAME set as the multiwindow smoke, but with a BOUND WindowBridge: this smoke drives tear-out /
// move through the bridge (as editor-core does), so window 0 AND every factory window can serve
// `window.tear-out` / `window.move-to` / `window.close` for real. Member order puts `handshake` first
// (destroyed last — the router captured it); the router lives in the retired session, outside this.
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
                               shell::WindowMoveStore& store, shell::WindowId window_id)
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

        // BOUND: the tear-out handler creates a window + seeds it; move-to enqueues a rehome; close
        // destroys. Identical to editor_main.cpp's binding, so this smoke drives the REAL path.
        window_bridge = std::make_unique<shell::WindowBridge>(window_id, store);
        window_bridge->bind_windows([&manager]() { return manager.window_ids(); });
        window_bridge->bind_tear_out(
            [&manager, &store](const shell::WindowBridge::TearOut& req) -> shell::WindowMoveResult
            {
                shell::WindowSpec spec;
                spec.headless = true; // Session-0 safe: never a real OS window in this smoke
                if (!req.title.empty())
                    spec.title = req.title;
                const shell::WindowCreateResult created = manager.create_window(spec, req.source);
                if (!created.ok())
                    return {false, shell::kInvalidWindowId, shell::to_string(created.outcome),
                            created.error};
                store.set_boot_seed(created.id, req.seed);
                return {true, created.id, shell::to_string(created.outcome), std::string{}};
            });
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

// Dispatch one bridge method on `router` and return its `result` (or the error object). Exactly how a
// renderer's query reaches the Shell — so this drives the SAME handler editor-core would.
Json dispatch(shell::BridgeRouter& router, const char* method, const Json& params)
{
    Json request = Json::object();
    request.set("jsonrpc", Json("2.0"));
    request.set("id", Json(42));
    request.set("method", Json(std::string(method)));
    request.set("params", params);
    const shell::BridgeDispatch out = router.dispatch(request.dump());
    const Json response = Json::parse(out.response);
    return response.contains("result") ? response.at("result") : response.at("error");
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

    std::printf("[editor-cef-smoke-shell-tearout] tear-out over D6 -> a live second window that "
                "restores the moved state; loud create-fail; rehome relay\n");

    std::error_code ec;
    const std::filesystem::path project =
        std::filesystem::temp_directory_path(ec) / "context-editor-cef-tearout-smoke";
    std::filesystem::remove_all(project, ec);
    std::filesystem::create_directories(project, ec);

    const render::Extent2D size{640, 480};

    SMOKE_CHECK(std::string(CONTEXT_WEBUI_ASSET_DIR).empty() == false,
                "CONTEXT_WEBUI_ASSET_DIR was compiled in (the webui asset root is wired)");

    // --- window 0: the primary, plus the shared move relay ---------------------------------------
    shell::WindowMoveStore move_store;
    shell::BridgeRouter primary_bridge;
    WindowSurfaces primary_surfaces;

    shell::WindowManager manager(project);

    SMOKE_CHECK(primary_surfaces.install(primary_bridge, manager, move_store, shell::kPrimaryWindowId),
                "every bridge surface installed on window 0");

    {
        shell::WindowDesc desc;
        desc.title = "Context Editor (tearout smoke)";
        desc.logical_size = size;
        desc.visible = false;
        auto backend = std::make_unique<shell::HeadlessWindowBackend>(desc);
        std::string error;
        std::unique_ptr<shell::IBrowserHost> browser =
            shell::cef::make_cef_browser_host(make_cef_options(size, &primary_bridge), error);
        if (browser == nullptr)
        {
            std::fprintf(stderr, "[editor-cef-smoke-shell-tearout] FAIL: window 0's browser did not "
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

    // The factory: how the tear-out handler's `create_window` builds the second window. Full surface +
    // a BOUND WindowBridge sharing the ONE move relay, so the new window's `window.seed` reads what the
    // tear-out handler stashed. Records the created surfaces so the assertions can read them.
    WindowSurfaces* created_surfaces = nullptr;
    shell::BridgeRouter* created_bridge = nullptr;
    manager.bind_window_factory(
        [&](const shell::WindowSpec& spec, shell::WindowSessionParts& parts,
            std::string& error) -> bool
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
            if (!surfaces->install(*window_bridge_router, manager, move_store, expected_id))
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
            created_surfaces = surfaces.get();
            created_bridge = window_bridge_router.get();
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
    if (manager.window(shell::kPrimaryWindowId) != primary)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-tearout] FAIL: window 0 did not survive boot\n");
        return finish(1);
    }
    SMOKE_CHECK(primary_bridge.refused() == 0, "window 0's bridge refused nothing at boot");

    // --- DoD 1: tear-out via the bridge -> a live second window that RESTORES the moved state -----
    //
    // Dispatch `window.tear-out` exactly as editor-core's tearOutActivePanel does: the moved panel +
    // its OPAQUE D6 blob. The handler creates window 1 and seeds it.
    Json tear_params = Json::object();
    tear_params.set("panelId", Json(std::string(kMovedPanel)));
    tear_params.set("state", impossible_state());
    tear_params.set("title", Json(std::string("Context Editor - window 1")));
    const Json tear_result = dispatch(primary_bridge, shell::kWindowTearOutMethod, tear_params);

    SMOKE_CHECK(tear_result.at("created").as_bool(), "the bridge created a second window for tear-out");
    SMOKE_CHECK(manager.window_count() == 2, "two windows are live after tear-out");
    SMOKE_CHECK(shell::cef::browsers_created() == 2, "a SECOND real CEF browser was created");
    const auto new_id = static_cast<shell::WindowId>(tear_result.at("windowId").as_int());
    SMOKE_CHECK(new_id == 1, "the torn-out window is a peer with id 1");
    // The seed is queued for the new window until its live renderer reads it.
    SMOKE_CHECK(move_store.has_boot_seed(new_id), "the moved panel + state is seeded for the new window");

    if (created_surfaces != nullptr)
        attach_present_path(new_id);

    if (created_surfaces != nullptr)
    {
        SMOKE_CHECK(boot_window(new_id, *created_surfaces, 30),
                    "window 1 composited a live CEF frame and completed its OWN handshake");
        // A FRESH editor-core instance, not window 0's document seen twice.
        SMOKE_CHECK(created_surfaces->handshake.nonce() != primary_surfaces.handshake.nonce(),
                    "the torn-out window minted its OWN handshake nonce (a fresh editor-core)");
        SMOKE_CHECK(created_bridge != nullptr && created_bridge->refused() == 0,
                    "the torn-out window's bridge refused nothing");

        // THE DoD PROOF, on state a fresh panel could not have: the live renderer READ its seed. The
        // relay's boot seed is consumed ONLY by a live `window.seed`, so `has_boot_seed` going false —
        // and the new window's `seeds_served()` climbing — is the end-to-end evidence that the moved
        // state reached the new window's editor-core, which then restores it over `panel.state.set`.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline && move_store.has_boot_seed(new_id))
        {
            if (!manager.pump_once(now_us()))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        SMOKE_CHECK(!move_store.has_boot_seed(new_id),
                    "the torn-out window's LIVE editor-core read its seed (window.seed consumed it)");
        SMOKE_CHECK(created_surfaces->window_bridge != nullptr &&
                        created_surfaces->window_bridge->seeds_served() >= 1,
                    "the new window served its boot seed to the live renderer");
    }

    // --- DoD 2: a create FAILURE is LOUD, not silent (03 §7) --------------------------------------
    //
    // Rebind the factory to fail; the SAME `window.tear-out` must answer created:false + a reason, the
    // structured result editor-core degrades to a floating group on (never a silent no-op).
    manager.bind_window_factory([](const shell::WindowSpec&, shell::WindowSessionParts&,
                                   std::string& error) -> bool
                                {
                                    error = "no native window backend on this platform (smoke)";
                                    return false;
                                });
    const Json fail_result = dispatch(primary_bridge, shell::kWindowTearOutMethod, tear_params);
    SMOKE_CHECK(fail_result.at("created").as_bool() == false,
                "a failed tear-out reports created:false (LOUD, not a silent success)");
    SMOKE_CHECK(fail_result.at("outcome").as_string() ==
                    std::string(shell::to_string(shell::WindowCreateOutcome::factory_failed)),
                "the failure carries the WindowCreateOutcome token editor-core degrades on");
    SMOKE_CHECK(!fail_result.at("error").as_string().empty(), "a human reason the user can be shown");
    SMOKE_CHECK(manager.window_count() == 2, "the failed create added no window");

    // --- DoD 3: the rehome/move relay delivers to a LIVE window's editor-core ---------------------
    //
    // Enqueue a panel to rehome INTO window 0 (a `window.move-to`, the SAME relay window-close rehome
    // uses). Window 0's LIVE editor-core drains it on its `window.rehomed` poll — proven by
    // `pending_rehomes(0)` dropping to zero without the smoke touching it.
    Json move_params = Json::object();
    move_params.set("panelId", Json(std::string(kMovedPanel)));
    move_params.set("state", impossible_state());
    move_params.set("windowId", Json(static_cast<std::uint64_t>(shell::kPrimaryWindowId)));
    // Issue the move FROM the (now torn-out) window 1's router if it exists, else from window 0 — either
    // way it targets window 0, whose live poll must drain it.
    shell::BridgeRouter& source_router =
        created_bridge != nullptr ? *created_bridge : primary_bridge;
    const Json move_result = dispatch(source_router, shell::kWindowMoveToMethod, move_params);
    SMOKE_CHECK(move_result.at("moved").as_bool(), "the move-to enqueued the panel for window 0");
    SMOKE_CHECK(move_store.pending_rehomes(shell::kPrimaryWindowId) >= 1,
                "the panel is queued for window 0 until its live poll drains it");

    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline &&
               move_store.pending_rehomes(shell::kPrimaryWindowId) > 0)
        {
            if (!manager.pump_once(now_us()))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        SMOKE_CHECK(move_store.pending_rehomes(shell::kPrimaryWindowId) == 0,
                    "window 0's LIVE editor-core drained the rehome on its window.rehomed poll "
                    "(the panel reached window 0, never silently lost)");
    }

    // --- teardown, in the ONE order that is safe (CE #319) ---------------------------------------
    manager.shutdown();
    shell::cef::shutdown();
    std::filesystem::remove_all(project, ec);

    if (g_failures != 0)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-tearout] FAILED with %d assertion failure(s)\n",
                     g_failures);
        return finish(1);
    }
    std::printf("[editor-cef-smoke-shell-tearout] PASS: tear-out -> live second window read its seed; "
                "create-fail was loud; rehome relay drained live\n");
    return finish(0);
}
