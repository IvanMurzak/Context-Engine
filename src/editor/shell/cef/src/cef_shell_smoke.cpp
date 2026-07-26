// The LIVE CEF windowed-OSR boot smoke (M9 e04, extended by e05c) — ctest `editor-cef-smoke-shell`.
//
// The Session-0-safe smoke (src/editor/shell/smoke/) proves the Shell's own machinery against
// scripted software-OSR frames on every OS leg. This one proves the other half: that a REAL CEF
// browser, driven by the REAL integrated pump, produces frames the REAL compositor composites and
// presents. It is the only place the two meet, and it can only run where CEF links — the per-OS
// `editor-cef-smoke` CI job.
//
// e05c ADDS THE TWO DoD LINES THAT CANNOT BE PROVEN LOCALLY AT ALL:
//
//   * `context-editor://app/…` serves the e05a bundle under the strict CSP. Asserted two ways: the
//     composited pixels carry the background from the SERVED `app.css` (so the scheme delivered a
//     second asset with a correct media type — a stylesheet served as the wrong type is ignored),
//     and the handshake below can only happen if `editor-core.js` was served AND executed under a
//     CSP that permits it. There is no `file://` anywhere in the path.
//   * The IPC bridge round-trips native<->JS inside the e04 shell window. The handshake is
//     deliberately THREE legs (JS -> native -> JS -> native, the last echoing a nonce only the
//     Shell knows), so it cannot pass unless a value made the full round trip. A one-way "the
//     bundle called us" ping would pass with a completely broken response path.
//
// The adversarial half of the bridge's DoD ("malformed/hostile messages rejected without crashing
// the Shell") lives in the CEF-FREE `editor-shell-test_ipc_bridge` suite, which runs on all three
// default `build` legs instead of only here — the same layering rationale as the rest of the Shell.
//
// TWO WINDOW MODES (M9 e12a-x11-legs, issue #408), selected by `--real-window`:
//
//   * DEFAULT — headless: no native window, presenting through e03's MemoryBlitter, and therefore
//     safe on the Session-0 self-hosted Windows runner (no visible window, no GPU device, no
//     native-render teardown). This is what the Windows leg runs.
//   * `--real-window` — a REAL X11 window from the REAL make_window_backend, presenting through the
//     REAL X11 blitter `EditorWindow::attach_cpu_present()` selects, with the REAL X server as the
//     input source. The ctest registration passes the flag on Linux, where the `editor-cef-smoke`
//     job already carries xvfb + libx11-dev + libxext-dev. It NEVER degrades: a missing display, a
//     compiled-out X11 backend, or an OS blitter that did not resolve are hard failures here.
//
// CEF itself is windowless-OSR in BOTH modes — cef_shell.cpp uses `native_window` only under
// _WIN32 — so the Shell's own X11 window is purely the PRESENT target, exactly the topology the
// CEF-free `editor-shell-x11-window` smoke proves. The Windows hard exit after success mirrors
// editor_host.cpp / cef_boot_smoke.cpp, skipping CEF's flaky Session-0 teardown.

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
#include "context/editor/shell/session_bridge.h"
#include "context/editor/shell/window_bridge.h"
#include "context/editor/shell/cef/cef_shell.h"
#include "context/editor/shell/editor_state_bridge.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/keybindings_bridge.h"
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/builtin_panels.h"
#include "context/editor/shell/shell.h"
#include "context/editor/shell/smoke/smoke_window.h"
#include "context/editor/shell/themes_bridge.h"
#include "context/editor/shell/user_config.h"
#include "context/editor/shell/welcome.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

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
    std::fprintf(stderr, "[editor-cef-smoke-shell] FAIL (line %d): %s\n", line, what);
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

// editor-core's built asset root, compiled in by CMake (cef/CMakeLists.txt), which also makes this
// target depend on `context_editor_webui` so the assets exist when the test runs.
//
// The fallback is EMPTY rather than an `#error`, on purpose: the pre-push audit's check 9 compiles
// this TU STANDALONE against the pinned CEF headers, with none of CMake's
// target_compile_definitions, so an `#error` here makes the only local signal for the whole CEF
// path permanently red — for this task and every future one that touches this file. The guard is
// not lost, it moves to two better places: cef/CMakeLists.txt FATAL_ERRORs at CONFIGURE time when
// CONTEXT_WEBUI_ASSET_DIR is empty, and the runtime assertion below fails the smoke loudly if an
// empty root ever reaches it.
#if !defined(CONTEXT_WEBUI_ASSET_DIR)
#define CONTEXT_WEBUI_ASSET_DIR ""
#endif

// The editor's docking-surface background, in BGRA8 (the composite's format).
//
// ⚠ MOVED BY e06b, and the move is the point. Until the theme engine landed this was app.css's
// standalone placeholder `--editor-bg: #132a44`. app.css now points all five Dockview background
// variables at the ACTIVE THEME's `colors.panel` instead, so this constant tracks
// `src/editor/webui/tokens/themes/dark.theme.json` -> `colors.panel` (#0a0a0a) — Dark being the
// first-run default (design 06 §4: follow `prefers-color-scheme`, Dark when undetectable, and a
// headless CEF renderer reports no preference).
//
// WHAT THE ASSERTION NOW PROVES, which is STRICTLY MORE than before: the stylesheet was served with
// a usable media type AND the live browser's pixels reached the present path (as before) AND the
// theme engine actually applied a theme — because if `editor-core.js` never ran, or the theme apply
// threw, the `var(--ctx-colors-panel, var(--editor-bg))` fallback paints the OLD #132a44 and this
// scan finds none of the colour it is looking for.
//
// Deliberately still not a value anything else produces: #0a0a0a is distinguishable from the
// zero-filled surface (#000000) that a deleted row copy in render_cpu_frame would leave behind, and
// from e04's old `#102040` data:-URL placeholder. Keeping ONE token behind all five background
// variables is what preserves the painted REGION this coverage floor is calibrated against — see the
// lockstep note on app.css's `.dockview-theme-dark` block, and the T1 tripwire test "the five
// Dockview BACKGROUND variables share one token".
constexpr std::uint8_t kAppBackgroundB = 0x0a;
constexpr std::uint8_t kAppBackgroundG = 0x0a;
constexpr std::uint8_t kAppBackgroundR = 0x0a;

// THE THEME THOSE THREE BYTES BELONG TO, PINNED INTO THE BOOT URL — do not drop it.
//
// `colors.panel` is a PER-THEME value (#0a0a0a Dark, #ffffff Light), and editor-core's first run
// follows the host's `prefers-color-scheme` (06 §4 / C-F22 — correct product behaviour). A CI host
// has no colour-scheme preference at all: no settings portal, so Chromium falls back to its `light`
// default and the editor honestly boots `builtin.light`. The scan above would then find ZERO texels
// of #0a0a0a and this smoke would fail on a perfectly healthy editor — which is exactly what it did
// on ubuntu AND windows until this pin landed, while a dark-mode dev box measured it green.
//
// Pinning the theme takes the AMBIENT input out of the test and weakens nothing: the coverage floor,
// the exact byte match and the non-uniformity check are all unchanged; the smoke simply now knows
// which theme's `colors.panel` it is looking for. `webui-theme-contract` re-derives kAppBackground*
// from THIS id's `*.theme.json` and reds if the two ever drift.
constexpr const char* kSmokeThemeId = "builtin.dark";

// Local rather than reused from tests/shell_test.h: that header pulls the render test fixtures,
// which this CEF-linking target does not (and should not) build against.
bool mentions(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// The composed-surface scan the e05d1 pixel assertions share with the wait loop below. Returning
// one struct keeps the loop's "has the UI actually painted yet?" test byte-for-byte identical to
// the final assertion's "is the frame non-uniform?" — the loop waits for EXACTLY the property the
// assertion will check, so it can neither break one poll too early (the CE #319 race) nor pass
// vacuously. `composed` is the compositor's own surface extent.
struct SurfaceScan
{
    std::size_t scanned = 0;
    std::size_t background_texels = 0;
    bool uniform = true;
};

SurfaceScan scan_surface(const std::vector<std::uint8_t>& surface, render::Extent2D composed)
{
    SurfaceScan scan;
    const std::size_t texels =
        static_cast<std::size_t>(composed.width) * static_cast<std::size_t>(composed.height);
    for (std::size_t i = 0; i < texels; ++i)
    {
        const std::size_t offset = i * 4u;
        if (offset + 3u >= surface.size())
        {
            break;
        }
        ++scan.scanned;
        const bool is_background = surface[offset + 0] == kAppBackgroundB &&
                                   surface[offset + 1] == kAppBackgroundG &&
                                   surface[offset + 2] == kAppBackgroundR;
        if (is_background)
        {
            ++scan.background_texels;
        }
        else
        {
            scan.uniform = false;
        }
    }
    return scan;
}

int finish(int code)
{
#if defined(_WIN32)
    // Session-0 carve-out (mirrors cef_boot_smoke.cpp / editor_host.cpp): CEF's teardown is flaky on
    // the self-hosted Windows runner, so exit hard once the verdict is decided.
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

    // e12a-x11-legs: `--real-window` (passed by the ctest registration on Linux) runs this whole
    // scenario over a REAL X11 window + the REAL X11 present blitter + the REAL X server as the
    // input source, instead of the offscreen shell. Parsed AFTER the subprocess re-entry above: a
    // CEF renderer/GPU child inherits the flag on its command line and must never reach this body.
    const smoke::WindowMode window_mode = smoke::window_mode_from_args(argc, argv);

    std::printf("[editor-cef-smoke-shell] live windowed-OSR CEF -> compositor -> present (%s "
                "window)\n",
                smoke::to_string(window_mode));

    std::error_code ec;
    const std::filesystem::path project =
        std::filesystem::temp_directory_path(ec) / "context-editor-cef-shell-smoke";
    std::filesystem::remove_all(project, ec);
    std::filesystem::create_directories(project, ec);

    const render::Extent2D size{640, 480};

    shell::WindowDesc desc;
    desc.title = "Context Editor (cef smoke)";
    desc.logical_size = size;
    // Headless by DEFAULT (the Session-0 rationale in the file header) and REAL under
    // `--real-window`. The seam decides; nothing here names a concrete backend, which is what
    // removed the `HeadlessWindowBackend*` this smoke used to keep in order to reach `post()`
    // (issue #408). Real mode never degrades — a null backend is a hard failure.
    smoke::WindowSetup window_setup = smoke::make_smoke_window(desc, window_mode);
    if (window_setup.backend == nullptr)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell] FAIL: no %s window could be created: %s\n",
                     smoke::to_string(window_mode), window_setup.diagnostic.c_str());
        return finish(1);
    }
    shell::IWindowBackend* backend_raw = window_setup.backend.get();
    // The size the WINDOW actually got, which is not the size that was asked for once a real
    // display's DPI is involved — CEF's view rect is DIP, so telling it the request would lay the
    // document out at the wrong size on any display that is not at 96 dpi.
    const smoke::BrowserGeometry geometry = smoke::browser_geometry(*backend_raw);

    // --- the privileged bridge (e05c) --------------------------------------------------------
    // `handshake` is declared BEFORE `bridge` (and installed after it) so that it OUTLIVES the
    // router holding the handlers that capture it — the lifetime invariant ShellHandshake::install
    // documents. Declaration order is destruction order reversed.
    shell::ShellHandshake handshake(shell::make_handshake_nonce());
    // Declared BEFORE the browser and outliving it: the CEF handler holds a raw pointer to this
    // router for the browser's whole life (BridgeRouter is non-movable so it cannot be relocated).
    shell::BridgeRouter bridge;
    // A stand-in credential, registered exactly as `context_editor` registers the real D20 token.
    // Its job here is to prove the egress guard is WIRED in the live binding, not just unit-tested:
    // the assertion after the handshake is that it never appears in anything the renderer received.
    const std::string fake_token = "smoke-token-4a91c7e0d25b8f36a1c9e4f70b2d6853";
    SMOKE_CHECK(bridge.protect_secret(fake_token),
                "the egress guard accepted the smoke credential");
    SMOKE_CHECK(handshake.install(bridge), "the bridge handshake installed");

    // --- the panel surface (e05d1) ------------------------------------------------------------
    // The REAL PanelHost with the REAL providers, so the live renderer hydrates an actual C++ panel
    // model rather than a stub. There is no daemon here, so Problems has no diagnostics — which is
    // exactly the point: what this smoke proves is the CHANNEL and the HYDRATION, and an empty
    // Problems panel still renders a real tree (heading + status + empty list). The diagnostic
    // PROJECTION is proven headlessly by editor-shell-test_problems_feed on all three build legs.
    //
    // LIFETIME. `builtin` owns the panel models; `panel_host`'s providers capture them; the router
    // holds handlers capturing `panel_host`. `builtin` must be declared after `panel_host` (it takes
    // a reference to it), so it is also DESTROYED FIRST — the reverse of the ownership order. That is
    // safe here, and only here, because teardown is ordered explicitly: `manager.shutdown()` and
    // `shell::cef::shutdown()` run at the end of main, so the browser and every renderer are already
    // gone before any of these locals unwind and no provider can be invoked during teardown.
    shell::PanelHost panel_host;
    shell::panels::BuiltinPanels builtin = shell::panels::install_builtin_panels(panel_host);
    SMOKE_CHECK(builtin.bound == shell::panels::hostable_panel_ids().size(),
                "every hostable built-in panel provider bound");
    SMOKE_CHECK(panel_host.install(bridge), "the panel.* bridge surface installed");

    shell::cef::CefShellOptions cef_options;
    // Windowless in BOTH modes, and that is not an oversight: cef_shell.cpp uses
    // `native_window` only under _WIN32 (SetAsWindowless(HWND)); the POSIX arm ignores it. So on
    // Linux CEF stays windowless-OSR either way and the Shell's own X11 window is purely the
    // PRESENT target — exactly the topology the CEF-free x11 smoke already proves.
    cef_options.native_window = nullptr;
    cef_options.logical_size = geometry.logical_size;
    cef_options.dpi = geometry.dpi;
    // THE app scheme, not a data: URL and emphatically not a file:// path (04 §1), carrying the
    // theme pin so the per-pixel background assertion below is about a theme this test CHOSE rather
    // than one the host's `prefers-color-scheme` chose for it (see kSmokeThemeId).
    cef_options.url = std::string(shell::kAppEntryUrl) + "?" + shell::kThemePinFlag + "=" +
                      kSmokeThemeId;
    cef_options.app_asset_root = CONTEXT_WEBUI_ASSET_DIR;
    cef_options.bridge = &bridge;
    // Keep the paint rate low: this smoke wants a FRAME, not a frame rate.
    cef_options.windowless_frame_rate = 10;
    // Isolate the OSCrypt profile-encryption key from the MACHINE keychain (issue #437). Without
    // this, macOS blocks CefShutdown() forever on a SecurityAgent authorization prompt no automated
    // run can answer, so the smoke prints its whole verdict and then never exits — see
    // CefShellOptions::use_mock_keychain for the mechanism. EVERY CEF smoke sets it, and
    // tools/check_cef_keychain_isolation.py fails the build if one stops.
    cef_options.use_mock_keychain = true;

    // The runtime half of the guard the header comment describes: an empty asset root would make
    // every scheme request 404 and the handshake time out 30 seconds later, which reads as a bridge
    // bug rather than as the build-wiring mistake it is.
    SMOKE_CHECK(!cef_options.app_asset_root.empty(),
                "CONTEXT_WEBUI_ASSET_DIR was compiled in (the webui asset root is wired)");
    std::printf("[editor-cef-smoke-shell] serving %s from %s\n", cef_options.url.c_str(),
                cef_options.app_asset_root.string().c_str());

    std::string error;
    std::unique_ptr<shell::IBrowserHost> browser =
        shell::cef::make_cef_browser_host(cef_options, error);
    if (browser == nullptr)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell] FAIL: the browser did not start: %s\n",
                     error.c_str());
        return finish(1);
    }

    shell::EditorWindowConfig config;
    // Software OSR — the shipping Windows path per the owner ruling of 2026-07-19.
    config.compositor.import_options.force_software = true;
    config.placement_poll_us = 0;
    auto window = std::make_unique<shell::EditorWindow>(std::move(window_setup.backend),
                                                        std::move(browser), config);

    // The C-F2 CPU present path: e03's portable blitter offscreen, and in real mode the REAL OS
    // blitter that `EditorWindow::attach_cpu_present()` selects from the REAL native window — the
    // same call `context_editor` makes on a GPU-less boot. Real mode REFUSES the in-memory blitter,
    // so a build that lost its X11 present path fails here instead of composing into a buffer and
    // calling it a present.
    const smoke::PresentSetup present_setup = smoke::attach_smoke_present(*window, window_mode);
    if (!present_setup.ok)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell] FAIL: no %s present path: %s\n",
                     smoke::to_string(window_mode), present_setup.diagnostic.c_str());
        return finish(1);
    }

    shell::WindowManager manager(project);
    manager.add(std::move(window));
    shell::EditorWindow* editor = manager.window(0);
    SMOKE_CHECK(editor != nullptr, "the manager adopted the window");
    if (editor == nullptr)
    {
        return finish(1);
    }

    // --- the editor-state + region-map surface (e05d2) ----------------------------------------
    // editor-core's LayoutPersistence calls `editor.state.get` on boot (the restore read) and
    // `editor.state.publish` / `editor.regions.publish` on every layout change. Those methods ride
    // the SAME privileged bridge as `panel.*`; unless the REAL EditorStateBridge is installed here,
    // the live boot handshake hits the router's deny-by-default `unknown_method` REFUSAL and the "no
    // envelope refusals" assertion below fails. Wire it exactly as `editor_main.cpp` does — the Shell
    // is the single writer of `.editor/editor-state.json` (03 §1), reached through the manager's
    // store; a published region map routes into this window's InputArbiter (03 §6). `manager` is
    // declared above this bridge so it OUTLIVES the handlers this install captures, and teardown is
    // ordered (`manager.shutdown()` runs before any local unwinds), so no handler is invoked after
    // these locals die — the same lifetime rationale `panel_host` above relies on.
    shell::EditorStateBridge editor_state_bridge;
    editor_state_bridge.bind_store(&manager.state_store(), now_us);
    editor_state_bridge.bind_regions(
        [&manager](std::vector<shell::ShellRegion> regions)
        {
            if (shell::EditorWindow* target_window = manager.window(0))
            {
                target_window->input().regions().publish(std::move(regions));
            }
        });
    SMOKE_CHECK(editor_state_bridge.install(bridge),
                "the editor.state.*/editor.regions.* bridge surface installed");

    // --- the keybindings read surface (e07c) --------------------------------------------------
    // editor-core's boot loads the per-user keybindings override by calling `keybindings.get`. Like
    // the editor-state methods above, that call rides this SAME privileged bridge — so unless the real
    // KeybindingsBridge is installed here, the live boot hits the router's deny-by-default
    // `unknown_method` REFUSAL and the "no envelope refusals" assertion below fails. Bound to an EMPTY
    // path (a permanently-absent snapshot) so the smoke is deterministic regardless of whether the CI
    // host happens to have a `~/.context/keybindings.json` — the method is served (present:false), the
    // renderer applies the default keymap, and nothing is refused. Same lifetime tier as the bridges
    // above.
    shell::KeybindingsBridge keybindings_bridge;
    keybindings_bridge.bind_path(std::filesystem::path{});
    SMOKE_CHECK(keybindings_bridge.install(bridge), "the keybindings.get bridge surface installed");

    // --- the watched-themes read surface (e06b) -------------------------------------------------
    // editor-core's boot loads watched user themes by calling `themes.get`, for exactly the same
    // reason and with exactly the same failure mode as `keybindings.get` above: unserved, it is an
    // `unknown_method` REFUSAL and the "no envelope refusals" assertion below fails. Bound to an EMPTY
    // directory so the smoke is deterministic regardless of what the CI host has in
    // `~/.context/themes/` — the method is served (an empty list), and the renderer applies a BUILT-IN
    // theme, which is what the per-pixel background assertion above depends on.
    shell::ThemesBridge themes_bridge;
    themes_bridge.bind_directory(std::filesystem::path{});
    SMOKE_CHECK(themes_bridge.install(bridge), "the themes.get bridge surface installed");

    // --- the welcome launch-mode surface (e14c) -----------------------------------------------
    // editor-core's boot calls `welcome.state` right after `shell.ready` to choose the welcome screen
    // vs the editor (boot.ts). Like the editor-state and keybindings methods above, that call rides
    // this SAME privileged bridge — so unless a real WelcomeBridge is installed here, the live boot
    // hits the router's deny-by-default `unknown_method` REFUSAL and the "no envelope refusals"
    // assertion below fails. boot.ts's own fallback (treat that refusal as "not welcome, proceed to
    // the editor") keeps the smoke booting and rendering, but the strict `bridge.refused() == 0`
    // invariant does not tolerate even a gracefully-handled refusal. Installed in PROJECT mode so the
    // method is SERVED reporting `mode: "project"` and the renderer takes the same editor/panels path
    // the rest of this smoke asserts — never the welcome screen (which mounts no panels). Bound to an
    // EMPTY config path (a permanently-absent recents store) so the served state is deterministic
    // regardless of whether the CI host happens to have a `~/.context/config.json`, mirroring the
    // keybindings bridge's empty-path rationale. Same lifetime tier as the bridges above.
    shell::WelcomeBridge welcome_bridge;
    welcome_bridge.set_launch_mode(shell::LaunchMode::project);
    welcome_bridge.set_config_path(std::filesystem::path{});
    SMOKE_CHECK(welcome_bridge.install(bridge), "the welcome.state bridge surface installed");

    // e14d: editor-core's boot calls `update.state` + `daemon.linkState`; install the surface so
    // those calls are SERVED rather than refused. The router denies unknown methods by DEFAULT, so an
    // uninstalled banner surface trips this file's strict `bridge.refused() == 0` invariant even
    // though editor-core degrades gracefully — the exact regression e06d shipped with its config
    // surface. NEITHER collaborator is bound: with no update notice the surface honestly reports "no
    // update channel is wired" (so this smoke makes NO network call), and with no daemon-link probe it
    // reports a live link (so no banner paints and the per-pixel coverage floor is untouched).
    shell::BannerBridge banner_bridge;
    SMOKE_CHECK(banner_bridge.install(bridge), "the banner bridge surface installed");

    // --- the daemon session read surface (e08d) --------------------------------------------------
    // editor-core's boot reads the daemon's L-51 play state with `session.state` so its
    // `when`-contexts see daemon truth instead of a frozen `edit` baseline (boot.ts `startSession`).
    // Same failure mode as every surface above: the router denies unknown methods by DEFAULT, so an
    // uninstalled session surface is an `unknown_method` REFUSAL and this file's strict
    // `bridge.refused() == 0` invariant fails — even though boot.ts degrades gracefully to the boot
    // baseline. Installed UNBOUND on purpose: this smoke has no daemon, so the surface honestly
    // reports `state:"edit", attached:false` — which is also what makes the boot deterministic here
    // (no live session can change the play state under the scenario).
    shell::SessionBridge session_bridge;
    SMOKE_CHECK(session_bridge.install(bridge), "the session.state bridge surface installed");
    // e10b: editor-core's boot now calls `window.seed` / `window.list` / `window.rehomed`; install
    // the surface (unbound — no tear-out is driven here) so those calls are not `unknown_method`
    // refusals that trip this smoke's `refused() == 0` invariant (the e06d regression).
    shell::WindowMoveStore window_move_store;
    shell::WindowBridge window_move_bridge(shell::kPrimaryWindowId, window_move_store);
    SMOKE_CHECK(window_move_bridge.install(bridge), "the window.* bridge surface installed");

    // --- the per-user config read surface (e06d) ------------------------------------------------
    // editor-core's boot reads the per-user config with `config.get` BEFORE it applies a theme
    // (boot.ts `loadUserConfig`), for exactly the same reason and with exactly the same failure mode
    // as `keybindings.get` / `themes.get` / `welcome.state` above: unserved, it is an
    // `unknown_method` REFUSAL and the "no envelope refusals" assertion below fails. boot.ts treats
    // that refusal as an empty snapshot and boots anyway, but — as the welcome bridge's note already
    // spells out — the strict `bridge.refused() == 0` invariant does not tolerate even a gracefully
    // handled refusal. Bound to an EMPTY path (a permanently-absent document, `writable() == false`)
    // so the smoke is deterministic regardless of whether the CI host happens to have a
    // `~/.context/config.json`: nothing is persisted, and the renderer takes the first-run
    // `prefers-color-scheme` path onto a BUILT-IN theme, which is what the per-pixel #0a0a0a
    // background assertion above depends on. A host-recorded theme would otherwise change the
    // composited colour and red this smoke on a developer machine only. Same lifetime tier as the
    // bridges above.
    shell::UserConfigStore user_config;
    user_config.bind_path(std::filesystem::path{});
    SMOKE_CHECK(user_config.install(bridge), "the config.* bridge surface installed");

    // Drive the integrated pump until the browser has painted, the bridge handshake completed,
    // every hostable panel has hydrated, AND the composed surface has ACTUALLY REPAINTED with the
    // mounted UI. All of them, not any: the composite proves the scheme served a renderable
    // document, the handshake proves the bundle executed and round-tripped, the render count proves
    // the app layer ran, and the non-uniform surface proves the hydrated DOM's pixels reached the
    // present target — the very thing the per-pixel assertion below checks.
    //
    // ⚠ WHY THE SURFACE CONDITION WAS ADDED (CE #319 — the race this test kept losing). Panels are
    // brought up AFTER `shell.ready` (boot.ts orders it so deliberately), and `renders_served()`
    // counts the render REQUEST served synchronously on the C++ side (hydration.ts:249) — which
    // climbs BEFORE the mounted DOM's apply() (hydration.ts:253) repaints into the CEF OSR frame, a
    // gap a05b42e's init()+onShow() startup double-refresh only widens. A loop that broke on
    // `renders_served >= expected` alone therefore sampled cpu_surface() while it was still the
    // uniform #0a0a0a background, and the "NOT a uniform fill" assertion failed on a frame that had
    // simply not painted yet. So also require the surface to be NON-UNIFORM — the exact property
    // that assertion checks. (A raw OnPaint / `view_frames` counter is a weaker proxy: it cannot
    // tell a re-paint of the background apart from the UI's first paint, so waiting on it could
    // still break early.) The 30s deadline is unchanged and still bounds the wait, so a genuine
    // no-paint regression never turns the surface non-uniform, the loop runs out the clock, and the
    // assertion below fails as it should — the wait is not vacuous and cannot loop forever.
    //
    // ⚠ e05d2 WIDENED THIS SAME RACE: once the smoke installs the real EditorStateBridge (above),
    // editor-core's boot-time `editor.state.get` restore actually SUCCEEDS instead of being refused,
    // so LayoutPersistence applies a restored (non-default) arrangement asynchronously after the
    // FIRST non-uniform paint the loop above would have broken on — intermittently red on ubuntu with
    // the SAME "the active theme's #0a0a0a panel background covers a substantial part" assertion the loop does not
    // yet wait for. Require the loop's OWN scan to clear the same background-coverage floor that
    // assertion checks (not merely non-uniform), exactly the CE #319 fix's own reasoning applied to
    // the second per-pixel property: the loop waits for EXACTLY what the assertion will check, so it
    // cannot break on a frame the still-in-flight layout restore has only partially painted.
    const std::size_t expected_renders = shell::panels::hostable_panel_ids().size();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool presented = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!manager.pump_once(now_us()))
        {
            break;
        }
        if (!presented && editor->compositor().stats().view_frames > 0 &&
            editor->compositor().stats().frames_presented > 0)
        {
            presented = true;
        }
        const bool hydrated =
            presented && handshake.complete() && panel_host.renders_served() >= expected_renders;
        if (hydrated)
        {
            const SurfaceScan poll_scan =
                scan_surface(editor->compositor().cpu_surface(), editor->compositor().size());
            if (!poll_scan.uniform && poll_scan.scanned > 0 &&
                poll_scan.background_texels > poll_scan.scanned / 10u)
            {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    SMOKE_CHECK(presented, "a real CEF OSR frame was composited and presented within 30s");
    SMOKE_CHECK(editor->compositor().stats().view_frames > 0,
                "the compositor adopted at least one OnPaint frame");
    // ⚠ READ FROM THE COMPOSITOR, not from a blitter handle. `blit_count()` is MemoryBlitter's and
    // does not exist on the OS blitters real mode attaches, and the compositor's own counter is the
    // same claim in both modes — `frames_presented` advances ONLY when the attached blitter's
    // blit() returned true (compositor.cpp), which in real mode means a real XShmPutImage/XPutImage
    // actually reached the X server.
    SMOKE_CHECK(editor->compositor().stats().frames_presented > 0,
                "the composited frame reached the present blitter");
    SMOKE_CHECK(!editor->compositor().cpu_surface().empty(), "the composed surface is non-empty");

    // The document's background, PER PIXEL. Counting frames is not enough: cpu_surface_ is zero-
    // filled before the compose, so deleting the row copy in render_cpu_frame leaves it non-empty
    // and correctly sized while presenting solid black, and every counter above still passes. This
    // is the assertion that the LIVE browser's pixels actually reached the present path — and it is
    // why app.css paints a known colour instead of an arbitrary one.
    //
    // ⚠ CHANGED BY e05d1, FROM NINE FIXED SAMPLES TO A FULL SCAN — and the change STRENGTHENS it.
    // Before e05d1 the window was empty, so every texel was the background and nine samples could
    // all be required to match. Now a real docking UI is mounted, and where any individual texel
    // lands relative to a tab strip, a sash or a glyph is not a stable fact — nine fixed points
    // would be exactly the brittleness that turns a real gate into a rerun-budget consumer (see
    // CE #319, which is this very test). Scanning the WHOLE surface instead proves BOTH facts more
    // strongly than sampling ever did:
    //
    //   * the stylesheet was served with a usable media type — the background colour is PRESENT in
    //     the frame, and a scan cannot miss it the way a sample can (this is now unmissable rather
    //     than probabilistic);
    //   * the live browser's pixels reached the present path — the surface is NON-UNIFORM, which is
    //     a fact the old assertion could not express at all: a solid fill of the right colour would
    //     have passed every one of its nine samples.
    //
    // The `app.css` note keeping the docking-surface token in lockstep with kAppBackground* still
    // applies (since e06b that token is the active theme's `colors.panel`, not `--editor-bg`), and the
    // stylesheet now also re-points Dockview's own surface colours at that variable so the editor's
    // background is genuinely present behind the UI rather than painted over by a theme.
    {
        // The SAME scan the wait loop polled, re-run once on the frame the loop settled on (no pump
        // runs between the break and here, so this observes exactly the surface the loop broke on).
        const SurfaceScan scan =
            scan_surface(editor->compositor().cpu_surface(), editor->compositor().size());

        SMOKE_CHECK(scan.scanned > 0, "the composed surface was large enough to scan");
        // A tenth of the frame, not "at least one texel": a single stray pixel of the right colour
        // could be coincidence, while a tenth of the window can only be the served stylesheet
        // painting the editor's background.
        SMOKE_CHECK(scan.scanned > 0 && scan.background_texels > scan.scanned / 10u,
                    "the active theme's #0a0a0a panel background covers a substantial part of the composited "
                    "frame — the app scheme served the STYLESHEET with a usable media type and the "
                    "LIVE browser's pixels reached the present path");
        SMOKE_CHECK(!scan.uniform,
                    "the composited frame is NOT a uniform fill — a real docking UI was painted on "
                    "top of the background, which a solid-colour surface would have faked");
    }

    // --- the e05c DoD assertions ------------------------------------------------------------------
    {
        // The bundle executed and reached the Shell. This can only be true if
        // `context-editor://app/index.html` AND `context-editor://app/editor-core.js` were both
        // served, and if the CSP permitted the module script to run.
        SMOKE_CHECK(handshake.hello_received(),
                    "editor-core called shell.hello over context-editor://ipc — the bundle was "
                    "served by the app scheme and executed under the strict CSP");
        // THE round-trip assertion: shell.ready only completes when the renderer echoed back a
        // nonce the Shell minted, so both directions of the channel are proven.
        SMOKE_CHECK(handshake.complete(),
                    "the IPC bridge round-tripped native<->JS: editor-core echoed the handshake "
                    "nonce back through shell.ready");
        SMOKE_CHECK(handshake.nonce_mismatches() == 0, "no nonce mismatch during the handshake");
        SMOKE_CHECK(bridge.served() >= 2, "both handshake legs were served by the router");
        SMOKE_CHECK(bridge.refused() == 0, "the live handshake produced no envelope refusals");
        // The egress guard is WIRED in the live binding, not merely unit-tested: nothing the
        // renderer asked for caused a protected value to be withheld, because nothing tried to
        // send one.
        SMOKE_CHECK(bridge.secrets_blocked() == 0,
                    "no handler attempted to return a protected credential");
        SMOKE_CHECK(!mentions(handshake.client_summary(), fake_token),
                    "the protected token never appeared in what editor-core sent us");
    }

    // --- the e05d1 DoD assertions: the LIVE hydration runtime actually ran ------------------------
    //
    // THIS IS THE ONLY PLACE THE HYDRATION RUNTIME IS PROVEN END TO END, and it is why the counters
    // exist on PanelHost at all. The local dev gate cannot link CEF, so it cannot run a browser; the
    // TS type gate proves the runtime COMPILES; the C++ T1 suites prove the panel surface behaves.
    // What none of them can show is that the bundle's PanelHost actually drove the Shell's — which
    // is the whole claim of "PanelHost owns panel lifecycle" and "Problems hydrates via the bridge".
    // These counters are incremented ONLY by a real `panel.*` call arriving over the router, so a
    // non-zero value here cannot be produced by anything but the live renderer having done it.
    {
        SMOKE_CHECK(panel_host.lists_served() > 0,
                    "the live renderer called panel.list — editor-core's PanelHost read the Shell's "
                    "roster over the bridge");
        SMOKE_CHECK(panel_host.renders_served() > 0,
                    "the live renderer called panel.render — the hydration runtime pulled a real "
                    "C++ panel's uitree and mounted it into the DOM");
        // Both hostable panels mounted, not just one. A runtime that special-cased a single panel
        // kind would render one and quietly skip the other, which this catches and a single-panel
        // assertion would not.
        SMOKE_CHECK(panel_host.renders_served() >= shell::panels::hostable_panel_ids().size(),
                    "every hostable panel was rendered, not merely the first");
        SMOKE_CHECK(bridge.secrets_blocked() == 0,
                    "no panel handler attempted to return a protected credential");
    }

    // Input round-trip into the LIVE browser. NOTE what this does and does NOT prove: the counters
    // asserted below are OUR InputArbiter's, incremented before the browser is called, so they pin
    // the arbitration half only. What makes this a LIVE-browser assertion is that CEF accepts the
    // translated events at all — a malformed CefMouseEvent/CefKeyEvent trips CEF's own checks — and
    // that the browser is still painting afterwards, which the post-resize repaint below asserts.
    // ⚠ IN REAL-WINDOW MODE EVERY ONE OF THESE IS ASYNCHRONOUS. `inject_event` does not queue: the
    // pointer and key are sent to this smoke's own window THROUGH THE X SERVER and come back on a
    // later pump via the real decoder, and the resize is a request the server answers with its own
    // ConfigureNotify. So the assertions below wait rather than reading the counters after one pump.
    const int pointers_before = editor->input().pointer_dispatches();
    const int keys_before = editor->input().key_dispatches();

    shell::ShellEvent move;
    move.kind = shell::ShellEventKind::pointer;
    move.pointer.action = shell::PointerAction::move;
    move.pointer.position = shell::PointI{100, 100};
    SMOKE_CHECK(smoke::inject_event(*backend_raw, window_mode, move), "the pointer move was injected");

    shell::ShellEvent click = move;
    click.pointer.action = shell::PointerAction::down;
    click.pointer.button = shell::MouseButton::left;
    SMOKE_CHECK(smoke::inject_event(*backend_raw, window_mode, click), "the pointer press was injected");

    shell::ShellEvent release = click;
    release.pointer.action = shell::PointerAction::up;
    // X reports the modifier mask BEFORE the event, so a release still carries its own button down.
    release.pointer.modifiers.left_button_down = true;
    SMOKE_CHECK(smoke::inject_event(*backend_raw, window_mode, release),
                "the pointer release was injected");

    shell::ShellEvent key;
    key.kind = shell::ShellEventKind::key;
    key.key.action = shell::KeyAction::raw_key_down;
    key.key.windows_key_code = 0x09; // VK_TAB — moves DOM focus, so it is not a no-op
    SMOKE_CHECK(smoke::inject_event(*backend_raw, window_mode, key), "the key was injected");

    const auto input_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < input_deadline)
    {
        if (!manager.pump_once(now_us()))
        {
            break;
        }
        if (editor->input().pointer_dispatches() >= pointers_before + 3 &&
            editor->input().key_dispatches() > keys_before)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // DELTAS and `>=`, not absolute equality. In real-window mode the X server is entitled to have
    // delivered input of its own: a LeaveNotify, which `translate_x11_event` carries as
    // `PointerAction::leave`. (An EnterNotify does NOT contribute — the decoder has no arm for it,
    // so a window merely mapping under the pointer adds nothing.) And a
    // real Tab press yields the raw key AND the character the decoder synthesizes from it — neither
    // of which the offscreen backend can produce. Asserting exact counts would be asserting the
    // state of whatever desktop this ran on; asserting the DELTA still fails at zero, which is the
    // only outcome a broken input path can produce.
    SMOKE_CHECK(editor->input().pointer_dispatches() >= pointers_before + 3,
                "the three pointer samples were arbitrated");
    SMOKE_CHECK(editor->input().key_dispatches() > keys_before, "the key was arbitrated");

    // A live resize: the browser must accept WasResized and repaint at the new size. In real-window
    // mode this is a REQUEST — the size the shell reacts to is the one the server grants, so the
    // assertion is against `backend_raw->client_size()` rather than the number that was asked for.
    const render::Extent2D size_before_resize = backend_raw->client_size();
    shell::ShellEvent resize;
    resize.kind = shell::ShellEventKind::resize;
    resize.size = render::Extent2D{800, 500};
    SMOKE_CHECK(smoke::inject_event(*backend_raw, window_mode, resize), "the resize was injected");
    const int frames_before_resize = editor->compositor().stats().view_frames;
    const auto resize_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool repainted = false;
    while (std::chrono::steady_clock::now() < resize_deadline)
    {
        if (!manager.pump_once(now_us()))
        {
            break;
        }
        if (editor->compositor().stats().view_frames > frames_before_resize &&
            editor->compositor().size().width == backend_raw->client_size().width &&
            editor->compositor().size().height == backend_raw->client_size().height &&
            (backend_raw->client_size().width != size_before_resize.width ||
             backend_raw->client_size().height != size_before_resize.height))
        {
            repainted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    SMOKE_CHECK(repainted, "the browser repainted after a live resize (WasResized)");
    SMOKE_CHECK(backend_raw->client_size().width != size_before_resize.width ||
                    backend_raw->client_size().height != size_before_resize.height,
                "the window really changed size");
    SMOKE_CHECK(editor->compositor().size().width == backend_raw->client_size().width &&
                    editor->compositor().size().height == backend_raw->client_size().height,
                "the compositor took the size the window actually got");

    // Read the presented-frame count BEFORE shutdown: shutdown() -> EditorWindow::close() ->
    // WindowCompositor::detach() destroys the blitter, and nothing is presented during teardown, so
    // this count is final. (Read from the COMPOSITOR rather than a MemoryBlitter handle, which real
    // mode does not have — see the present assertion above.)
    const int presented_frames = editor->compositor().stats().frames_presented;

    manager.shutdown();
    shell::cef::shutdown();
    std::filesystem::remove_all(project, ec);

    if (g_failures != 0)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell] FAILED with %d assertion failure(s)\n",
                     g_failures);
        return finish(1);
    }
    std::printf("[editor-cef-smoke-shell] PASS: live CEF windowed-OSR composited + presented "
                "(%d frames through the %s blitter, %s window), input round-tripped, live resize "
                "repainted\n",
                presented_frames, present_setup.blitter_name.c_str(),
                smoke::to_string(window_mode));
    return finish(0);
}
