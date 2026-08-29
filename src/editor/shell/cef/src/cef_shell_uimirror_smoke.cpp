// The LIVE cross-window `editor.ui` MIRROR smoke (M9 e10d-drill2) — ctest
// `editor-cef-smoke-shell-uimirror`. This is the CEF half of INHERITED DRILL 2.
//
// `test_ui_mirror.cpp` proves the Shell relay's machinery (broadcast + per-window queues + the
// `ui.mirror` / `ui.mirror-poll` / `ui.mirror-report` bridge methods) on all three default `build`
// legs with no browser, and `uimirror_broadcast.test.ts` proves the receiving bus's
// `receiveMirrored` echo-suppression in the fake-Shell tier. But e08c's own refine found that its
// two-bus ring drill only ever exercised the POINT-TO-POINT loop breaker; the breaker a BROADCASTING
// transport needs — an envelope arriving back at its OWN `origin` is dropped — was never exercised
// END TO END, across a real Shell hop, between two live editor-core instances with DISTINCT origins.
// This smoke is that end-to-end drill, and it proves the two things a single-window build (or a fake)
// structurally CANNOT:
//
//   1. **CONVERGENCE — a fact published in window A reaches window B through the REAL Shell mirror.**
//      Window 0's LIVE editor-core publishes an `editor.ui.theme-changed` fact on its per-window-origin
//      bus; the ShellUiMirrorSink hands it to `ui.mirror`, the Shell BROADCASTS it to every live
//      window, and window 1's LIVE editor-core drains it on its mirror poll and its bus APPLIES it
//      (foreign origin "0"). Proven because window 1 reports `applied >= 1` over `ui.mirror-report` —
//      a value only a SECOND real bus with a DIFFERENT origin can produce. Cross-origin by construction
//      (e08a): the two windows are two editor-core instances over two wire connections.
//   2. **ECHO SUPPRESSION IN THE BROADCASTING SHAPE — the fact does NOT echo back into window A.**
//      The broadcast delivers window 0 its OWN envelope back (that is the shape that arms the branch —
//      window 0's `ui_mirrors_delivered()` climbs), and window 0's bus DROPS it by origin. Proven
//      because window 0 reports `applied == 0` (it applied none of its own echoes) with
//      `suppressed >= 1` (it dropped them). A one-window build has no window 1 to converge and no
//      second origin to distinguish, so this A-applies-none / B-applies-one asymmetry cannot exist
//      there — which is exactly why e08c deferred the drill to a real second window.
//
// The receiving bus's applied/dropped verdict lives ONLY in the renderer, and the transport counters
// cannot see it: `ui.mirror-poll` hands window 0 its own echo just as it hands window 1 a peer's fact,
// so a delivered count reads them alike. `ui.mirror-report` is the channel that makes that verdict
// observable — the same "drive a JS decision to a C++-observable side effect" shape the settings smoke
// (`UserConfigStore::writes()`) and the drag smoke (`drag_zones_reported()`) use.
//
// Headless throughout (windowless browsers, the C-F2 CPU present path), safe on the Session-0 runner,
// exactly like its sibling smokes. The Windows hard exit after the verdict mirrors them.
//
// ⚠ WHAT THIS LEG DOES NOT DO — stated honestly (09 §3). Windows CI runs Session-0 (no interactive
// desktop), so nothing here rests on a real window being shown, focused, or composited to a visible
// surface: both browsers are windowless and the mirror is a pure IPC/data hop. It proves the real
// cross-window mirror propagation + the echo-suppression branch over the REAL Shell relay between two
// LIVE editor-cores — and nothing more; it does NOT fake a green.

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
#include "context/editor/shell/package_grants.h"
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/builtin_panels.h"
#include "context/editor/shell/session_bridge.h"
#include "context/editor/shell/shell.h"
#include "context/editor/shell/smoke/smoke_window.h"
#include "context/editor/shell/themes_bridge.h"
#include "context/editor/shell/ui_mirror.h"
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
namespace smoke = context::editor::shell::smoke;
namespace render = context::render;

namespace
{

int g_failures = 0;

void check(bool condition, const char* what, int line)
{
    if (condition)
    {
        return;
    }
    std::fprintf(stderr, "[editor-cef-smoke-shell-uimirror] FAIL (line %d): %s\n", line, what);
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

// Pinned so both windows boot on the same deterministic theme path (no ambient prefers-color-scheme),
// exactly as the sibling smokes pin it. This smoke asserts NO pixels — it is not in check 9's
// THEME_SMOKES scan set — but pinning keeps both windows' boot deterministic on the CI host.
constexpr const char* kSmokeThemeId = "builtin.dark";

// The full boot surface for one window, with the WindowBridge bound to the SHARED cross-window mirror
// store (e10d) AND the live window set — so this window's LIVE editor-core's ShellUiMirrorSink
// broadcasts through the real relay and its poller drains from it. Member order puts `handshake` first
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

    [[nodiscard]] bool install(shell::BridgeRouter& router, shell::WindowManager& manager,
                               shell::WindowMoveStore& store, shell::UiMirrorStore& mirror_store,
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
        // editor-window-chrome d1: install() registers `session.control` (the play-bar strip's
        // transport relay) beside `session.state`; asserted so the ten-smoke rule is mechanised
        // rather than trusted. Unbound, it answers the honest "nothing to drive", never a refusal.
        ok = router.has_method(shell::kSessionControlMethod) && ok;
        package_grants =
            std::make_unique<shell::PackageGrantHost>(package_scan, std::filesystem::path{});
        ok = package_grants->install(router) && ok;

        window_bridge = std::make_unique<shell::WindowBridge>(window_id, store);
        // The LIVE window set drives the broadcast fan-out: `ui.mirror` fans the envelope to every id
        // this reports (plus self), so WITHOUT this a publish would reach only the sender and window 1
        // would never converge. It is the same provider the drag/tearout smokes bind.
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
        // e10d: this window's editor-core mirrors editor.ui facts through the SHARED store.
        window_bridge->bind_ui_mirror_store(&mirror_store);
        ok = window_bridge->install(router) && ok;
        return ok;
    }
};

// Both windows carry the mirror-smoke flag AND the theme pin. Window 0's editor-core re-publishes an
// editor.ui fact on every poll tick under the flag (boot.ts), so the broadcast reaches window 1 once it
// is up and polling; every window drains + applies + reports convergence.
std::string boot_url()
{
    return std::string(shell::kAppEntryUrl) + "?" + shell::kThemePinFlag + "=" + kSmokeThemeId +
           "&ctx-smoke-uimirror";
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

    std::printf("[editor-cef-smoke-shell-uimirror] an editor.ui fact published in window 0 converges in "
                "window 1 through the REAL Shell mirror, and does NOT echo back into window 0\n");

    std::error_code ec;
    const std::filesystem::path project =
        std::filesystem::temp_directory_path(ec) / "context-editor-cef-uimirror-smoke";
    std::filesystem::remove_all(project, ec);
    std::filesystem::create_directories(project, ec);

    const render::Extent2D size{640, 480};

    SMOKE_CHECK(std::string(CONTEXT_WEBUI_ASSET_DIR).empty() == false,
                "CONTEXT_WEBUI_ASSET_DIR was compiled in (the webui asset root is wired)");

    // --- the shared relays + window 0 (the mirror PUBLISHER) -------------------------------------
    shell::WindowMoveStore move_store;
    shell::UiMirrorStore mirror_store;
    shell::BridgeRouter primary_bridge;
    WindowSurfaces primary_surfaces;

    shell::WindowManager manager(project);

    SMOKE_CHECK(primary_surfaces.install(primary_bridge, manager, move_store, mirror_store,
                                         shell::kPrimaryWindowId),
                "every bridge surface installed on window 0");

    {
        shell::WindowDesc desc;
        desc.title = "Context Editor (uimirror smoke, window 0)";
        desc.logical_size = size;
        smoke::WindowSetup window_setup = smoke::make_smoke_window(desc, window_mode);
        if (window_setup.backend == nullptr)
        {
            std::fprintf(stderr, "[editor-cef-smoke-shell-uimirror] FAIL: no %s window 0: %s\n",
                         smoke::to_string(window_mode), window_setup.diagnostic.c_str());
            return finish(1);
        }
        const smoke::BrowserGeometry geometry = smoke::browser_geometry(*window_setup.backend);
        std::string error;
        std::unique_ptr<shell::IBrowserHost> browser =
            shell::cef::make_cef_browser_host(make_cef_options(geometry, &primary_bridge), error);
        if (browser == nullptr)
        {
            std::fprintf(stderr, "[editor-cef-smoke-shell-uimirror] FAIL: window 0's browser did not "
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
            std::fprintf(stderr, "[editor-cef-smoke-shell-uimirror] FAIL: no %s present path for window 0: %s\n",
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

    // The factory: window 1 (the RECEIVER) gets the full live surface, sharing the ONE move relay + the
    // ONE mirror store, so its editor-core drains the broadcast for real. Records the created surfaces
    // so the assertions can read window 1's WindowBridge counters.
    WindowSurfaces* target_surfaces = nullptr;
    manager.bind_window_factory(
        [&](const shell::WindowSpec& spec, shell::WindowSessionParts& parts, std::string& error)
            -> bool
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
            if (!surfaces->install(*window_bridge_router, manager, move_store, mirror_store,
                                   expected_id))
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
            target_surfaces = surfaces.get();
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
    SMOKE_CHECK(primary_bridge.refused() == 0, "window 0's bridge refused nothing at boot");

    // Create + boot window 1 (the RECEIVER).
    shell::WindowSpec spec;
    // e12a-x11-legs: a REAL second window under --real-window, offscreen otherwise. This is
    // the WindowSpec::headless switch itself, which is why the DoD names it: a run that asks
    // for real windows must get one HERE too, not only for window 0.
    spec.headless = window_mode == smoke::WindowMode::headless;
    spec.title = "Context Editor (uimirror smoke, window 1)";
    const shell::WindowCreateResult created = manager.create_window(spec);
    SMOKE_CHECK(created.ok(), "window 1 (the mirror receiver) was created");
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

    // --- drive the drill: pump until window 1 CONVERGES and window 0 SUPPRESSES its own echo -------
    //
    // Window 0's editor-core re-publishes an editor.ui.theme-changed fact on every poll tick (under the
    // smoke flag), so once window 1 is up and polling the broadcast reaches it. We pump until BOTH
    // verdicts hold — window 1's bus reports at least one APPLIED fact (convergence) and window 0's bus
    // reports at least one SUPPRESSED echo (the branch) — WITHOUT the smoke touching either bus.
    const auto converged = [&]() -> bool
    {
        return target_surfaces->window_bridge != nullptr &&
               target_surfaces->window_bridge->ui_mirror_reported_applied() >= 1 &&
               primary_surfaces.window_bridge != nullptr &&
               primary_surfaces.window_bridge->ui_mirror_reported_suppressed() >= 1;
    };
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline && !converged())
        {
            if (!manager.pump_once(now_us()))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // --- DoD line: CONVERGENCE — the fact reached window 1 through the REAL Shell mirror -----------
    SMOKE_CHECK(primary_surfaces.window_bridge != nullptr &&
                    primary_surfaces.window_bridge->ui_mirrors_published() >= 1,
                "window 0's LIVE editor-core published an editor.ui fact through ui.mirror");
    SMOKE_CHECK(target_surfaces->window_bridge != nullptr &&
                    target_surfaces->window_bridge->ui_mirrors_delivered() >= 1,
                "the broadcast reached window 1's queue (the real cross-window Shell hop)");
    SMOKE_CHECK(target_surfaces->window_bridge != nullptr &&
                    target_surfaces->window_bridge->ui_mirror_reported_applied() >= 1,
                "window 1's LIVE bus APPLIED the mirrored fact (foreign origin \"0\") — CONVERGENCE, a "
                "value only a second real bus with a different origin can produce");

    // --- DoD line: ECHO SUPPRESSION — the fact does NOT echo back into window 0 --------------------
    SMOKE_CHECK(primary_surfaces.window_bridge != nullptr &&
                    primary_surfaces.window_bridge->ui_mirrors_delivered() >= 1,
                "the broadcast delivered window 0 its OWN envelope back (the shape that arms the drop)");
    SMOKE_CHECK(primary_surfaces.window_bridge != nullptr &&
                    primary_surfaces.window_bridge->ui_mirror_reported_suppressed() >= 1,
                "window 0's LIVE bus DROPPED its own echo by origin (the broadcasting loop breaker "
                "e08c's ring drill could not exercise)");
    SMOKE_CHECK(primary_surfaces.window_bridge != nullptr &&
                    primary_surfaces.window_bridge->ui_mirror_reported_applied() == 0,
                "window 0 APPLIED none of its own echoes — the fact did NOT echo back into window 0");

    SMOKE_CHECK(primary_bridge.refused() == 0, "window 0's bridge refused nothing across the drill");

    // --- teardown, in the ONE order that is safe (CE #319) ---------------------------------------
    manager.shutdown();
    shell::cef::shutdown();
    std::filesystem::remove_all(project, ec);

    if (g_failures != 0)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-uimirror] FAILED with %d assertion failure(s)\n",
                     g_failures);
        return finish(1);
    }
    std::printf("[editor-cef-smoke-shell-uimirror] PASS: window 0's editor.ui fact converged in window 1 "
                "through the real Shell mirror and did not echo back into window 0\n");
    return finish(0);
}
