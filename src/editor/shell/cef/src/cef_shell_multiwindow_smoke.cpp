// The LIVE multi-window smoke (M9 e10a) — ctest `editor-cef-smoke-shell-multiwindow`.
//
// `editor-shell-test_window_registry` proves the registry's machinery on all three `build` legs with
// a fake browser. This proves the three things a fake structurally CANNOT, each of which is a DoD
// line of e10a:
//
//   1. **N windows really exist.** A SECOND native window is created at runtime through the registry
//      and boots its OWN FRESH editor-core instance (03 §1 — not a shared instance, not
//      `retainContext`). Proven by the handshake NONCE: each window's Shell mints a different one
//      and only the document running in THAT window can round-trip it, so two completed handshakes
//      with two different nonces is two live editor-core instances, not one document seen twice.
//   2. **`window.open` cannot produce an unmanaged window.** Driven as a REAL renderer
//      `window.open` — a real user gesture injected through the real input path first, so the popup
//      is one Chromium would otherwise honour — and asserted in BOTH directions: `OnBeforePopup`
//      actually fired (`popups_suppressed()` climbed) and NO browser was created
//      (`browsers_created()` unchanged). The positive half is what stops the assertion passing
//      vacuously when the script never ran at all.
//   3. **A window can be destroyed MID-PROCESS and another created after it.** This is the CE #319
//      hazard class with the process-wide teardown removed: `CloseBrowser` returning is not proof
//      CEF is done with the client that holds the window's `BridgeRouter*`. The registry retires
//      the session instead of freeing it (shell.h), and this exercises that on the REAL CEF path,
//      on the same Session-0 Windows runner where CE #319 faulted.
//
// Headless throughout (windowless browsers, no native window, the C-F2 CPU present path through
// e03's MemoryBlitter), so it is safe on the Session-0 self-hosted Windows runner: no visible
// window, no GPU device, no native-render teardown. The Windows hard exit after the verdict mirrors
// its sibling smokes.
//
// ⚠ e10a adds NO boot-time bridge METHOD. Every surface installed below already exists and is
// already installed by every sibling smoke — the router denies unknown methods by default, so a new
// boot-time method would have to be installed in all of them or they red on `refused() == 0` (the
// e06d regression). The registry's create-failure report is a C++ callback for exactly that reason;
// e10b, which needs editor-core to hear about it, adds the one method then.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "context/editor/shell/app_scheme.h"
#include "context/editor/shell/banners.h"
#include "context/editor/shell/cef/cef_shell.h"
#include "context/editor/shell/editor_state_bridge.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/keybindings_bridge.h"
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/builtin_panels.h"
#include "context/editor/shell/shell.h"
#include "context/editor/shell/themes_bridge.h"
#include "context/editor/shell/user_config.h"
#include "context/editor/shell/welcome.h"
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

namespace
{

int g_failures = 0;

void check(bool condition, const char* what, int line)
{
    if (condition)
    {
        return;
    }
    std::fprintf(stderr, "[editor-cef-smoke-shell-multiwindow] FAIL (line %d): %s\n", line, what);
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

// See the sibling smokes: an empty fallback rather than an #error, so the pre-push audit's check 9
// can compile this TU standalone against the pinned CEF headers with none of CMake's defines. The
// real guards are cef/CMakeLists.txt's configure-time FATAL_ERROR and the runtime assertion below.
#if !defined(CONTEXT_WEBUI_ASSET_DIR)
#define CONTEXT_WEBUI_ASSET_DIR ""
#endif

// The theme pinned into every boot URL, for the same reason the boot smoke pins it: a CI host has no
// `prefers-color-scheme`, so leaving the choice ambient makes the editor's own first-run rule (06 §4)
// an input to the test. Nothing here asserts pixels by value, but pinning keeps both windows on the
// same deterministic boot path.
constexpr const char* kSmokeThemeId = "builtin.dark";

// ---------------------------------------------------------------- one window's bridge surfaces
//
// Everything editor-core calls during boot, for ONE window. The router denies unknown methods by
// default, so a missing surface here is a `refused()` — which this smoke asserts stays at zero for
// EVERY window, not just the first. Each window gets its own instance: that is what "its own fresh
// editor-core instance" means on the native side.
//
// MEMBER ORDER: `panel_host` before `builtin` (which takes a reference to it), and `handshake`
// first so it is destroyed last — the router's handlers captured it, and the router itself lives
// outside this object, in the session the registry retires (window_registry.h § LIFETIME RULE).
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

    // Install every surface on `router`, routing region publishes to `window_id`. All the empty
    // paths are deliberate — a permanently-absent per-user file makes the boot deterministic
    // regardless of what the CI host has in `~/.context/` (the sibling smokes' rationale, verbatim).
    [[nodiscard]] bool install(shell::BridgeRouter& router, shell::WindowManager& manager,
                               shell::WindowId window_id)
    {
        bool ok = handshake.install(router);
        ok = panel_host.install(router) && ok;
        editor_state.bind_store(&manager.state_store(), now_us);
        editor_state.bind_regions(
            [&manager, window_id](std::vector<shell::ShellRegion> regions)
            {
                // Routed to the window that published them. With N windows a single hard-coded
                // window 0 would hand the SECOND window's viewport rects to the FIRST window's input
                // arbiter — the exact class of bug the registry's ids exist to make impossible.
                if (shell::EditorWindow* target = manager.window(window_id))
                {
                    target->input().regions().publish(std::move(regions));
                }
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
    // A frame, not a frame rate.
    options.windowless_frame_rate = 10;
    return options;
}

int finish(int code)
{
#if defined(_WIN32)
    // Session-0 carve-out (mirrors the sibling smokes): CEF's teardown is flaky on the self-hosted
    // Windows runner, so exit hard once the verdict is decided.
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
    // Subprocess re-entry FIRST: CEF's renderer/GPU/utility processes re-exec this binary.
    const int subprocess_exit = shell::cef::execute_subprocess(argc, argv);
    if (subprocess_exit >= 0)
    {
        return subprocess_exit;
    }

    std::printf("[editor-cef-smoke-shell-multiwindow] N live windows, one editor-core each, "
                "window.open suppressed\n");

    std::error_code ec;
    const std::filesystem::path project =
        std::filesystem::temp_directory_path(ec) / "context-editor-cef-multiwindow-smoke";
    std::filesystem::remove_all(project, ec);
    std::filesystem::create_directories(project, ec);

    const render::Extent2D size{640, 480};

    SMOKE_CHECK(std::string(CONTEXT_WEBUI_ASSET_DIR).empty() == false,
                "CONTEXT_WEBUI_ASSET_DIR was compiled in (the webui asset root is wired)");

    // --- window 0: the primary, built by the app itself (exactly as editor_main.cpp does) --------
    //
    // Declared BEFORE the manager so its bridge outlives every handler AND outlives the manager's
    // own teardown; `shell::cef::shutdown()` below runs while both are still in scope, which is the
    // CE #319 invariant `cef_shell.h` states.
    shell::BridgeRouter primary_bridge;
    WindowSurfaces primary_surfaces;

    shell::WindowManager manager(project);

    SMOKE_CHECK(primary_surfaces.install(primary_bridge, manager, shell::kPrimaryWindowId),
                "every bridge surface installed on window 0");

    {
        shell::WindowDesc desc;
        desc.title = "Context Editor (multiwindow smoke)";
        desc.logical_size = size;
        desc.visible = false;
        auto backend = std::make_unique<shell::HeadlessWindowBackend>(desc);

        std::string error;
        std::unique_ptr<shell::IBrowserHost> browser =
            shell::cef::make_cef_browser_host(make_cef_options(size, &primary_bridge), error);
        if (browser == nullptr)
        {
            std::fprintf(stderr,
                         "[editor-cef-smoke-shell-multiwindow] FAIL: window 0's browser did not "
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
    SMOKE_CHECK(shell::cef::browsers_created() == 1, "exactly one browser exists after window 0");

    // --- the window factory: how a SECOND window is built ----------------------------------------
    //
    // The same shape editor_main.cpp binds, with the smoke's headless backend and asset root. It
    // records what it produced so the assertions below can prove the two windows share NOTHING that
    // carries identity.
    shell::BridgeRouter* created_bridge = nullptr;
    WindowSurfaces* created_surfaces = nullptr;
    int factory_calls = 0;
    manager.bind_window_factory(
        [&](const shell::WindowSpec& spec, shell::WindowSessionParts& parts,
            std::string& error) -> bool
        {
            ++factory_calls;
            shell::WindowDesc desc;
            desc.title = spec.title;
            desc.logical_size = spec.logical_size;
            desc.visible = false; // Session-0: never a real OS window in this smoke
            parts.backend = std::make_unique<shell::HeadlessWindowBackend>(desc);

            // ITS OWN router + ITS OWN surfaces. The window id is not known until the manager adopts
            // it, and region routing needs it — so the surfaces are installed against the id the
            // registry is ABOUT to mint, which is `last_minted_id() + 1` on a fresh create. Reading
            // it back after adoption is not possible from inside the factory, so the smoke asserts
            // the routing separately below by checking the ids it actually got.
            auto window_bridge = std::make_unique<shell::BridgeRouter>();
            auto surfaces = std::make_shared<WindowSurfaces>();
            const shell::WindowId expected_id =
                static_cast<shell::WindowId>(manager.last_minted_id() + 1u);
            if (!surfaces->install(*window_bridge, manager, expected_id))
            {
                error = "a bridge surface refused to install on the new window";
                return false;
            }

            std::string browser_error;
            parts.browser = shell::cef::make_cef_browser_host(
                make_cef_options(spec.logical_size, window_bridge.get()), browser_error);
            if (parts.browser == nullptr)
            {
                error = "the browser did not start: " + browser_error;
                return false;
            }

            created_bridge = window_bridge.get();
            created_surfaces = surfaces.get();
            parts.surfaces.push_back(std::move(surfaces));
            parts.bridge = std::move(window_bridge);
            error.clear();
            return true;
        });

    // Drive both windows until a window's editor-core has booted: a composited OSR frame AND a
    // completed handshake. Waiting on BOTH is what makes the wait non-vacuous — the frame proves the
    // scheme served a renderable document, the handshake proves the bundle executed and a value made
    // the full native->JS->native round trip.
    const auto boot_window = [&](shell::WindowId id, WindowSurfaces& surfaces, int seconds) -> bool
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (!manager.pump_once(now_us()))
            {
                return false;
            }
            shell::EditorWindow* window = manager.window(id);
            if (window == nullptr)
            {
                return false;
            }
            if (window->compositor().stats().view_frames > 0 && surfaces.handshake.complete())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };

    SMOKE_CHECK(boot_window(shell::kPrimaryWindowId, primary_surfaces, 30),
                "window 0 composited a live CEF frame and completed its bridge handshake");

    // `primary` is a raw pointer INTO the registry, and `pump_once` retires a window that died on
    // its own — so if window 0 did not survive its boot, every later use of `primary` reads freed
    // memory. Bail with a legible verdict instead: an ACCESS_VIOLATION here would wear the exact
    // `0xC0000005` signature the CE #319 catalogue entry describes and be triaged as that, turning a
    // real break into a rerun.
    if (manager.window(shell::kPrimaryWindowId) != primary)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-multiwindow] FAIL: window 0 did not survive "
                             "its own boot — the rest of the smoke would read freed memory\n");
        return finish(1);
    }

    // --- DoD: the Shell can CREATE a second window, with its own fresh editor-core instance -------
    shell::WindowSpec spec;
    spec.title = "Context Editor — window 1";
    spec.logical_size = size;
    spec.headless = true;
    spec.state_index = 1;

    const shell::WindowCreateResult second = manager.create_window(spec, shell::kPrimaryWindowId);
    SMOKE_CHECK(second.ok(), "the registry created a second live window");
    SMOKE_CHECK(second.id == 1, "the second window is a peer with id 1");
    SMOKE_CHECK(manager.window_count() == 2, "two windows are live");
    SMOKE_CHECK(shell::cef::browsers_created() == 2, "a SECOND real CEF browser was created");
    SMOKE_CHECK(created_bridge != nullptr && created_bridge != &primary_bridge,
                "the second window got its OWN bridge router, not the primary's");
    SMOKE_CHECK(created_surfaces != nullptr,
                "the second window got its own bridge surfaces");

    // `create_window()` deliberately leaves the present path to the caller — attaching one needs an
    // RHI the registry does not own (window_registry.h: "a window with no present path composites
    // nothing"). Window 0 gets this from main() below; the factory-created window needs the exact
    // same step, or its compositor's `view_frames` never leaves zero and `boot_window` times out
    // waiting on a frame that was never going to arrive.
    if (second.ok())
    {
        if (shell::EditorWindow* second_window = manager.window(second.id))
        {
            second_window->compositor().attach_cpu(std::make_unique<present::MemoryBlitter>(), size);
        }
    }

    if (second.ok() && created_surfaces != nullptr)
    {
        SMOKE_CHECK(boot_window(second.id, *created_surfaces, 30),
                    "window 1 composited a live CEF frame and completed its OWN bridge handshake");
        // A FRESH editor-core instance, not the primary's document seen twice: the two Shells minted
        // DIFFERENT handshake nonces, and only the document running in that window can round-trip
        // its own. Both completing means two live instances.
        SMOKE_CHECK(created_surfaces->handshake.nonce() != primary_surfaces.handshake.nonce(),
                    "the two windows minted different handshake nonces");
        SMOKE_CHECK(created_surfaces->handshake.complete() && primary_surfaces.handshake.complete(),
                    "both editor-core instances round-tripped their OWN nonce");
        SMOKE_CHECK(created_surfaces->handshake.nonce_mismatches() == 0 &&
                        primary_surfaces.handshake.nonce_mismatches() == 0,
                    "neither window saw a replayed or guessed nonce");
        // Nothing was refused on EITHER router: with N windows the deny-by-default router is now N
        // routers, and a surface installed on only one of them is a class of bug that did not exist
        // before this task.
        SMOKE_CHECK(primary_bridge.refused() == 0, "window 0's bridge refused nothing");
        SMOKE_CHECK(created_bridge->refused() == 0, "window 1's bridge refused nothing");
    }

    // --- DoD: `window.open` cannot produce an unmanaged window ------------------------------------
    //
    // A REAL renderer window.open, not a unit-level stub. The click first is load-bearing: Chromium
    // gates a script-initiated popup on transient user activation, so without a real gesture through
    // the real input path the call could be dropped BEFORE it ever reaches our boundary — and the
    // test would then "pass" having proven nothing about `OnBeforePopup` at all.
    {
        const int browsers_before = shell::cef::browsers_created();
        const int suppressed_before = shell::cef::popups_suppressed();

        shell::ShellEvent click;
        click.kind = shell::ShellEventKind::pointer;
        click.pointer.position = shell::PointI{40, 40};
        click.pointer.action = shell::PointerAction::down;
        click.pointer.button = shell::MouseButton::left;
        // The primary's backend is the headless one this smoke created; reach it through the window.
        auto& headless = static_cast<shell::HeadlessWindowBackend&>(primary->backend());
        headless.post(click);
        shell::ShellEvent release = click;
        release.pointer.action = shell::PointerAction::up;
        headless.post(release);
        // Tracked, not discarded: a false `pump_once` means no window is left, and `primary` would
        // then be a dangling pointer for the rest of this block (see the guard after window 0's boot).
        bool pump_alive = true;
        for (int i = 0; i < 20 && pump_alive; ++i)
        {
            pump_alive = manager.pump_once(now_us());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        SMOKE_CHECK(pump_alive && manager.window(shell::kPrimaryWindowId) == primary,
                    "window 0 survived the injected user gesture");
        if (!pump_alive || manager.window(shell::kPrimaryWindowId) != primary)
        {
            return finish(1);
        }

        primary->browser().execute_script(
            "window.open('about:blank', '_blank', 'width=320,height=240');");

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (pump_alive && std::chrono::steady_clock::now() < deadline &&
               shell::cef::popups_suppressed() == suppressed_before)
        {
            pump_alive = manager.pump_once(now_us());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        // The POSITIVE half: the request actually reached the boundary and the boundary refused it.
        SMOKE_CHECK(shell::cef::popups_suppressed() > suppressed_before,
                    "a REAL renderer window.open reached OnBeforePopup and was suppressed");
        // The NEGATIVE half, which is the security property itself: no unmanaged window exists.
        SMOKE_CHECK(shell::cef::browsers_created() == browsers_before,
                    "window.open created NO browser — every window is one the Shell manages");
        SMOKE_CHECK(manager.window_count() == 2,
                    "window.open added no window to the registry either");
    }

    // --- DoD: destroy MID-PROCESS, then create again (the CE #319 hazard class) -------------------
    {
        const WindowSurfaces* destroyed_surfaces = created_surfaces;
        const shell::WindowDestroyResult destroyed = manager.destroy_window(second.id);
        SMOKE_CHECK(destroyed.ok(), "the second window was destroyed mid-process");
        SMOKE_CHECK(manager.window_count() == 1, "only the primary is left");
        SMOKE_CHECK(manager.window(second.id) == nullptr, "the destroyed id resolves to nothing");
        // Its session was RETIRED, not freed: CEF finishes tearing a closed browser down inside
        // CefShutdown, still dispatching to the client that holds this window's router.
        SMOKE_CHECK(manager.retired_session_count() == 1,
                    "the destroyed window's session was retired, not freed");
        (void)destroyed_surfaces; // still allocated — that IS the assertion above

        // The primary must survive its peer's teardown and keep painting. If a mid-process browser
        // teardown corrupted CEF's state, this is where it shows.
        const int frames_before = primary->compositor().stats().view_frames;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        bool still_painting = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (!manager.pump_once(now_us()))
            {
                break;
            }
            if (primary->compositor().stats().view_frames > frames_before)
            {
                still_painting = true;
                break;
            }
            // A resize is the cheapest way to make CEF repaint on demand.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!still_painting)
        {
            // Ask for a repaint explicitly rather than waiting on an idle browser: a damage-driven
            // compositor legitimately produces no new frame when nothing changed.
            shell::ShellEvent resize;
            resize.kind = shell::ShellEventKind::resize;
            resize.size = render::Extent2D{800, 500};
            static_cast<shell::HeadlessWindowBackend&>(primary->backend()).post(resize);
            const auto repaint_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (std::chrono::steady_clock::now() < repaint_deadline)
            {
                if (!manager.pump_once(now_us()))
                {
                    break;
                }
                if (primary->compositor().stats().view_frames > frames_before)
                {
                    still_painting = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        SMOKE_CHECK(still_painting, "window 0 kept painting after its peer was destroyed");

        // ...and the registry can create ANOTHER one afterwards. Repeated create/destroy on the REAL
        // CEF path — the deterministic 25-cycle version lives in editor-shell-test_window_registry.
        created_bridge = nullptr;
        created_surfaces = nullptr;
        shell::WindowSpec again = spec;
        again.title = "Context Editor — window 2";
        const shell::WindowCreateResult third = manager.create_window(again, shell::kPrimaryWindowId);
        SMOKE_CHECK(third.ok(), "a window was created again after a mid-process destroy");
        // An id is never reused: 2, not the destroyed 1.
        SMOKE_CHECK(third.id == 2, "the re-created window got a FRESH id, not the destroyed one");
        SMOKE_CHECK(factory_calls == 2, "the factory was called once per create");
        // Same present-path attach the second window needed above — the re-created window is built
        // through the same factory + `create_window()` path, so it needs it too.
        if (third.ok())
        {
            if (shell::EditorWindow* third_window = manager.window(third.id))
            {
                third_window->compositor().attach_cpu(std::make_unique<present::MemoryBlitter>(), size);
            }
        }
        if (third.ok() && created_surfaces != nullptr)
        {
            SMOKE_CHECK(boot_window(third.id, *created_surfaces, 30),
                        "the re-created window booted its own editor-core instance too");
            SMOKE_CHECK(created_bridge != nullptr && created_bridge->refused() == 0,
                        "the re-created window's bridge refused nothing");
        }
    }

    // --- teardown, in the ONE order that is safe (CE #319) ---------------------------------------
    // manager.shutdown() closes every browser and RETIRES every session; shell::cef::shutdown()
    // finishes CEF's own teardown while every router is still alive; only then does `manager` (and
    // with it the graveyard) unwind, at the end of main.
    manager.shutdown();
    shell::cef::shutdown();
    std::filesystem::remove_all(project, ec);

    if (g_failures != 0)
    {
        std::fprintf(stderr,
                     "[editor-cef-smoke-shell-multiwindow] FAILED with %d assertion failure(s)\n",
                     g_failures);
        return finish(1);
    }
    std::printf("[editor-cef-smoke-shell-multiwindow] PASS: %d browsers created, %d popup(s) "
                "suppressed, create/destroy/create clean\n",
                shell::cef::browsers_created(), shell::cef::popups_suppressed());
    return finish(0);
}
