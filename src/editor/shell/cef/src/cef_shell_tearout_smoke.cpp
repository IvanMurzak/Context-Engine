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
// SINCE editor-window-chrome f1 THIS IS ALSO THE SECONDARY-WINDOW CHROME PROOF (02 §9 / D4): under
// the custom chrome this smoke serves, the torn-out window's `chrome.state` answers
// `window:"secondary"` off its own bridge, its compact strip publishes its OWN caption + control
// regions (a generation bump on WINDOW 1's map), the pure b1 hit-test answers the frame codes over
// those live rects — frameless holds for every window the factory creates — the `ctx-smoke-chrome`
// seam routes the three control verbs through window 1's own bridge, and a `window.close` from the
// secondary closes exactly it. The DOM half (compact strip contents, no play-bar/statusbar DOM) is
// the webui tier's (chrome.test.ts § f1), per the same split every chrome task used.
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
#include "context/editor/shell/package_grants.h"
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/builtin_panels.h"
#include "context/editor/shell/session_bridge.h"
#include "context/editor/shell/shell.h"
#include "context/editor/shell/smoke/smoke_window.h"
#include "context/editor/shell/themes_bridge.h"
#include "context/editor/shell/user_config.h"
#include "context/editor/shell/welcome.h"
#include "context/editor/shell/window.h" // f1: hit_test_frame + kHt* over the secondary's live map
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
namespace smoke = context::editor::shell::smoke;
namespace render = context::render;
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
    // e13c-4: editor-core's boot reads the operator's install-consent answers with
    // `package.grants.list` before any panel mounts (boot.ts, `ShellPackageGrants.load`), for the
    // same deny-by-default reason as every surface above — uninstalled it is an `unknown_method`
    // REFUSAL that trips this smoke's `refused() == 0` invariant on EVERY window. Bound to an EMPTY
    // scan (no package is installed here, so no contribution can carry a grant) and an EMPTY grants
    // path (deterministic regardless of the host's own `~/.context/package-grants.json`, and
    // `PackageGrantStore::save` REFUSES an empty path, so a `decide` can never write a developer's
    // real consent document), so the served answer is the same deny-all state a refusal produces and
    // NO package gains authority. `package_scan` is declared BEFORE the host that references it, so
    // it outlives it; the host is built in `install` for the same reason `window_bridge` is — it has
    // no default constructor.
    shell::PackageStoreScan package_scan;
    std::unique_ptr<shell::PackageGrantHost> package_grants;
    std::unique_ptr<shell::WindowBridge> window_bridge;

    // `window_mode` (e12a-x11-legs) reaches the TEAR-OUT handler below, which is where this
    // smoke's second window is specified: without it the handler could not honour a run that asked
    // for real windows, and the tear-out target would silently stay offscreen.
    [[nodiscard]] bool install(shell::BridgeRouter& router, shell::WindowManager& manager,
                               shell::WindowMoveStore& store, shell::WindowId window_id,
                               smoke::WindowMode window_mode)
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
        // d1: install() also routes `session.control` — session_bridge.h § kSessionControlMethod.
        ok = router.has_method(shell::kSessionControlMethod) && ok;
        package_grants =
            std::make_unique<shell::PackageGrantHost>(package_scan, std::filesystem::path{});
        ok = package_grants->install(router) && ok;

        // BOUND: the tear-out handler creates a window + seeds it; move-to enqueues a rehome; close
        // destroys. Identical to editor_main.cpp's binding, so this smoke drives the REAL path.
        window_bridge = std::make_unique<shell::WindowBridge>(window_id, store);
        window_bridge->bind_windows([&manager]() { return manager.window_ids(); });
        window_bridge->bind_tear_out(
            [&manager, &store, window_mode](const shell::WindowBridge::TearOut& req)
                -> shell::WindowMoveResult
            {
                shell::WindowSpec spec;
                // e12a-x11-legs: a REAL torn-out window under --real-window, offscreen otherwise.
                // This is the WindowSpec::headless switch itself, which is why the DoD names it: a
                // run that asks for real windows must get one HERE too, not only for window 0.
                spec.headless = window_mode == smoke::WindowMode::headless;
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
        // f1 (02 §9): serve CUSTOM chrome to EVERY window — the smoke-tier counterpart of the DOM
        // tier's injected states, exactly as the boot smoke binds it (cef_shell_smoke.cpp § a2).
        // Under `custom` each window's titlebar renders its controls cluster and publishes its
        // caption + control regions, so the SECONDARY-window assertions below have live rects to
        // read. The `window` field is NOT served here — the bridge derives it from its own
        // `self_id`, which is precisely the fact the f1 assertions pin.
        window_bridge->bind_chrome_state(
            []() -> shell::ChromeState
            {
                shell::ChromeState state;
                state.mode = shell::ChromeMode::custom;
                return state;
            });
        ok = window_bridge->install(router) && ok;
        return ok;
    }
};

std::string boot_url()
{
    // `ctx-smoke-chrome` (f1): every window's editor-core drives the three window-control verbs
    // once at boot — the a1 seam the boot smoke uses — so the SECONDARY window's control routing
    // is assertable from its bridge counters below (a headless CEF run cannot click the buttons).
    return std::string(shell::kAppEntryUrl) + "?" + shell::kThemePinFlag + "=" + kSmokeThemeId +
           "&ctx-smoke-chrome=1";
}

shell::cef::CefShellOptions make_cef_options(const smoke::BrowserGeometry& geometry,
                                             shell::BridgeRouter* bridge)
{
    shell::cef::CefShellOptions options;
    // Windowless in BOTH window modes, and not merely for Session 0: cef_shell.cpp reads
    // `native_window` only under _WIN32, so on Linux CEF stays windowless-OSR either way and the
    // Shell's own X11 window is purely the PRESENT target.
    options.native_window = nullptr;
    // The size the WINDOW actually got: a real display's DPI makes it differ from the
    // request, and CEF's view rect is DIP, so passing the request lays the document out wrong.
    options.logical_size = geometry.logical_size;
    options.dpi = geometry.dpi;
    options.url = boot_url();
    options.app_asset_root = CONTEXT_WEBUI_ASSET_DIR;
    options.bridge = bridge;
    options.windowless_frame_rate = 10;
    // Isolate the OSCrypt profile-encryption key from the MACHINE keychain (issue #437). Without
    // this, macOS blocks CefShutdown() forever on a SecurityAgent authorization prompt no automated
    // run can answer, so the smoke prints its whole verdict and then never exits — see
    // CefShellOptions::use_mock_keychain for the mechanism. EVERY CEF smoke sets it, and
    // tools/check_cef_keychain_isolation.py fails the build if one stops.
    options.use_mock_keychain = true;
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

    // e12a-x11-legs: `--real-window` (passed by the ctest registration on Linux) runs this whole
    // scenario over REAL X11 windows — window 0 AND every window the factory creates — presenting
    // through the REAL X11 blitter. Parsed AFTER the subprocess re-entry above: a CEF renderer/GPU
    // child inherits the flag on its command line and must never reach this body.
    const smoke::WindowMode window_mode = smoke::window_mode_from_args(argc, argv);

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

    SMOKE_CHECK(primary_surfaces.install(primary_bridge, manager, move_store,
                                         shell::kPrimaryWindowId, window_mode),
                "every bridge surface installed on window 0");

    {
        shell::WindowDesc desc;
        desc.title = "Context Editor (tearout smoke)";
        desc.logical_size = size;
        smoke::WindowSetup window_setup = smoke::make_smoke_window(desc, window_mode);
        if (window_setup.backend == nullptr)
        {
            std::fprintf(stderr, "[editor-cef-smoke-shell-tearout] FAIL: no %s window 0: %s\n",
                         smoke::to_string(window_mode), window_setup.diagnostic.c_str());
            return finish(1);
        }
        const smoke::BrowserGeometry geometry = smoke::browser_geometry(*window_setup.backend);
        std::string error;
        std::unique_ptr<shell::IBrowserHost> browser =
            shell::cef::make_cef_browser_host(make_cef_options(geometry, &primary_bridge), error);
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
        auto window = std::make_unique<shell::EditorWindow>(std::move(window_setup.backend),
                                                            std::move(browser), config);
        const smoke::PresentSetup present_setup =
            smoke::attach_smoke_present(*window, window_mode);
        if (!present_setup.ok)
        {
            std::fprintf(stderr, "[editor-cef-smoke-shell-tearout] FAIL: no %s present path for window 0: %s\n",
                         smoke::to_string(window_mode), present_setup.diagnostic.c_str());
            return finish(1);
        }
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
            // The per-window real/headless switch editor_main.cpp honours: the SPEC decides,
            // so the Nth window is a real OS window exactly when this run asked for one. A
            // create failure is REPORTED (03 §7), never degraded to headless.
            const smoke::WindowMode child_mode =
                spec.headless ? smoke::WindowMode::headless : window_mode;
            smoke::WindowSetup child_setup = smoke::make_smoke_window(desc, child_mode);
            if (child_setup.backend == nullptr)
            {
                error = child_setup.diagnostic;
                return false;
            }
            parts.backend = std::move(child_setup.backend);

            auto window_bridge_router = std::make_unique<shell::BridgeRouter>();
            auto surfaces = std::make_shared<WindowSurfaces>();
            const shell::WindowId expected_id =
                static_cast<shell::WindowId>(manager.last_minted_id() + 1u);
            if (!surfaces->install(*window_bridge_router, manager, move_store, expected_id,
                                   window_mode))
            {
                error = "a bridge surface refused to install on the new window";
                return false;
            }
            std::string browser_error;
            parts.browser = shell::cef::make_cef_browser_host(
                make_cef_options(smoke::browser_geometry(*parts.backend), window_bridge_router.get()), browser_error);
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
        {
            // `mode_of` and NOT `window_mode`: this window was built by the FACTORY, which
            // honours the spec's own headless flag — so the present path must match the
            // window that was actually created rather than the one the run asked for.
            const smoke::PresentSetup child_present =
                smoke::attach_smoke_present(*window, smoke::mode_of(window->backend()));
            SMOKE_CHECK(child_present.ok, "the created window took its present path");
        }
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

        // --- f1: SECONDARY-WINDOW chrome (editor-window-chrome f1, target design 02 §9 / D4) -----
        //
        // The seed wait above doubles as the ordering guarantee for everything here: boot fetches
        // `chrome.state`, drives the `ctx-smoke-chrome` verbs and AWAITS the strips' initial region
        // publish BEFORE it reads `window.seed` (boot.ts), so a consumed seed means the torn-out
        // window's chrome is fully up. What the DOM tier cannot prove and this can: the role, the
        // regions and the verbs travel WINDOW 1'S OWN bridge + input arbiter, never the primary's.
        if (created_surfaces->window_bridge != nullptr)
        {
            // The role is the BRIDGE's own self_id derivation — the same handler the live renderer
            // fetched (chrome_reads climbed), so this pins the value editor-core actually saw.
            SMOKE_CHECK(created_surfaces->window_bridge->chrome_reads() >= 1,
                        "the torn-out window's editor-core fetched chrome.state at its own boot");
            const Json secondary_chrome =
                dispatch(*created_bridge, shell::kChromeStateMethod, Json::object());
            SMOKE_CHECK(secondary_chrome.at("window").as_string() == "secondary",
                        "chrome.state.window is \"secondary\" in the torn-out window (02 §9)");
            SMOKE_CHECK(secondary_chrome.at("mode").as_string() == "custom",
                        "…with the same chrome MODE every window of this app gets");
            const Json primary_chrome =
                dispatch(primary_bridge, shell::kChromeStateMethod, Json::object());
            SMOKE_CHECK(primary_chrome.at("window").as_string() == "primary",
                        "window 0 stays the primary — the role is per-window, not per-app");

            // The compact strip's controls dispatch over WINDOW 1's own bridge: the a1 seam drove
            // each verb once in that window's boot, and the counters move even unbound (routing is
            // the claim, exactly as the boot smoke asserts for window 0).
            SMOKE_CHECK(created_surfaces->window_bridge->minimizes() == 1,
                        "window.minimize routed once in the SECONDARY window (the seam drove it)");
            SMOKE_CHECK(created_surfaces->window_bridge->maximize_toggles() == 1,
                        "window.toggle-maximize routed once in the SECONDARY window");
            SMOKE_CHECK(created_surfaces->window_bridge->focus_requests() == 1,
                        "window.focus routed once in the SECONDARY window");
        }
        if (shell::EditorWindow* second = manager.window(new_id))
        {
            // THE PER-WINDOW REGION PROOF (02 §9): the torn-out window's compact strip measured and
            // published ITS OWN caption + control rects over ITS OWN channel — the generation bump
            // is on WINDOW 1's map, which only its own `editor.regions.publish` route can move (a
            // hard-coded window 0 in the factory's region sink would leave it at zero forever).
            const shell::RegionMap& map = second->input().regions();
            SMOKE_CHECK(map.generation() >= 1,
                        "the SECONDARY window published its own regions (generation bump observed)");
            const shell::ShellRegion* caption = map.find("chrome.caption");
            const shell::ShellRegion* min_region = map.find("chrome.caption-min");
            const shell::ShellRegion* max_region = map.find("chrome.caption-max");
            const shell::ShellRegion* close_region = map.find("chrome.caption-close");
            SMOKE_CHECK(caption != nullptr,
                        "the compact strip's caption drag surface arrived in window 1's map");
            SMOKE_CHECK(min_region != nullptr && max_region != nullptr && close_region != nullptr,
                        "…with the mode-correct controls (custom keeps the full cluster)");
            SMOKE_CHECK(map.size() == 4,
                        "the compact strip publishes exactly the chrome vocabulary, wholesale");
            SMOKE_CHECK(!map.regions().empty() && map.regions().front().id == "chrome.caption",
                        "caption first — the last-match-wins arbitration order holds here too");
            // The b1 frame takeover, for a FACTORY window on this leg: the pure hit-test the real
            // WndProc calls answers, over window 1's LIVE rects, the NC codes that make the OS own
            // the drag and light Snap Layouts — the same two-halves proof the boot smoke pins for
            // window 0 (the real-HWND leg stays the deferred interactive verification b1 named).
            const render::Extent2D client = second->backend().client_size();
            const shell::DpiScale dpi = second->backend().dpi();
            const auto mid = [](const shell::ShellRegion& region) -> shell::PointI
            {
                return shell::PointI{
                    static_cast<std::int32_t>(region.rect.origin.x + region.rect.size.width / 2u),
                    static_cast<std::int32_t>(region.rect.origin.y +
                                              region.rect.size.height / 2u)};
            };
            if (caption != nullptr && min_region != nullptr && max_region != nullptr &&
                close_region != nullptr)
            {
                SMOKE_CHECK(shell::hit_test_frame(mid(*caption), client, dpi, false, map) ==
                                shell::kHtCaption,
                            "the factory window's caption answers HTCAPTION — frameless holds for "
                            "every window the factory creates, not just window 0");
                SMOKE_CHECK(shell::hit_test_frame(mid(*min_region), client, dpi, false, map) ==
                                shell::kHtMinButton,
                            "…its minimize control answers HTMINBUTTON");
                SMOKE_CHECK(shell::hit_test_frame(mid(*max_region), client, dpi, false, map) ==
                                shell::kHtMaxButton,
                            "…its maximize control answers HTMAXBUTTON (Snap Layouts)");
                SMOKE_CHECK(shell::hit_test_frame(mid(*close_region), client, dpi, false, map) ==
                                shell::kHtClose,
                            "…and its close control answers HTCLOSE");
            }
        }
        // …and the channels never crossed: window 0's OWN strip regions are still on window 0's
        // map (the mis-routing hazard editor_main.cpp's region sink names — a secondary publish
        // handed to the primary's arbiter would double up here and vanish above).
        SMOKE_CHECK(primary->input().regions().find("chrome.caption") != nullptr &&
                        primary->input().regions().size() == 4,
                    "window 0 keeps its own four chrome regions — per-window channels never crossed");
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

    // --- f1: the compact strip's ✕, end to end — window.close dispatched FROM the secondary ------
    //
    // The third control verb the compact cluster carries (the DOM tier proves the button dispatches
    // it; this proves the dispatch WORKS from a secondary window): `window.close` on window 1's own
    // router destroys exactly that window — the primary-vs-secondary policy the handler already
    // carries — through the same mid-run destroy path the multiwindow smoke hardened (CE #319).
    if (created_bridge != nullptr && manager.window_count() == 2)
    {
        const Json close_result = dispatch(*created_bridge, shell::kWindowCloseMethod,
                                           Json::object());
        SMOKE_CHECK(close_result.at("closed").as_bool(),
                    "window.close from the SECONDARY window closed it");
        SMOKE_CHECK(manager.window_count() == 1, "…and only it — the primary survives");
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
                "create-fail was loud; rehome relay drained live; secondary chrome (role, own "
                "regions, control verbs, close) held on the torn-out window\n");
    return finish(0);
}
