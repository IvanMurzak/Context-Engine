// The LIVE settings-driven CEF smoke (M9 e06d) — ctest `editor-cef-smoke-shell-settings`.
//
// This is the T2 half of e06d: it proves the WHOLE theme-persistence chain end to end through the real
// CEF pump. Where `editor-cef-smoke-shell` proves the boot handshake + panel hydration and
// `-shell-palette` proves the command layer, this one boots the SAME live windowless browser and then
// drives a REAL theme change THROUGH THE SETTINGS PANEL, exactly as a user would.
//
// HOW IT DRIVES, AND WHAT IT OBSERVES. editor-core's boot, under the `?ctx-smoke-settings` URL flag
// (boot.ts `runSettingsSmoke`), finds the mounted Settings panel and selects the first offered theme
// that is not the active one — the same code path the `<select>` change handler takes. That applies the
// theme locally AND requests `config.set`. The OBSERVABLES this smoke asserts are therefore on the
// SHELL side, where C-F14 says they must be:
//
//   1. `UserConfigStore::writes() >= 1` — the Shell PERSISTED something, and the Shell is the only
//      thing that can (a fresh boot with no interaction writes nothing; the boot smoke never sees one).
//   2. the theme recorded in the config DOCUMENT ON DISK is a real theme id that is NOT the one the
//      boot URL pinned — so the value that landed came from the user's choice in the panel, not from a
//      default the Shell could have written by itself.
//   3. the recents member the config already held SURVIVED the write — the merge-preserving
//      read-modify-write (the defect e14c's writer had), asserted against a live renderer rather than
//      only in the T1 suite.
//
// That chain cannot be satisfied by accident: it requires the bundle to load, the panel roster to carry
// `builtin.settings` as a `local` panel, PanelHost to mount it through the local-factory seam, the kit
// to have built an operable picker, and the `config.set` envelope to round-trip.
//
// CAUSE REPORTING (DoD). `verbose_logging` is on so Chromium's own renderer/console errors interleave
// onto stderr, the shared CefShell client's OnLoadError + OnConsoleMessage report a page that never
// loads, and every milestone below is a flushed `trace()` line — the LAST one before a gap in the CI
// log is where a hang died. A scenario that silently selected nothing runs the 30s clock out and the
// `writes()` assertion fails as it should.
//
// It boots exactly as cef_shell_palette_smoke.cpp does — a windowless browser presenting through e03's
// MemoryBlitter, hard-exiting on Windows after the verdict to skip CEF's flaky Session-0 teardown — so
// it is structurally identical to that proven-green single-boot smoke, and it can only run where CEF
// links: the per-OS `editor-cef-smoke` CI job (Windows/Linux; macOS's .app packaging is e12's).

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
    std::fprintf(stderr, "[editor-cef-smoke-shell-settings] FAIL (line %d): %s\n", line, what);
    ++g_failures;
}

#define SMOKE_CHECK(cond, what) check((cond), (what), __LINE__)

// Flushed progress trace — the only failure signal for a hang inside the live CEF pump is a stalled
// heartbeat, and CEF does not link on the GCC dev host so this is not locally reproducible.
void trace(const char* label, const char* msg)
{
    std::fprintf(stderr, "[editor-cef-smoke-shell-settings] [%s] %s\n", label, msg);
    std::fflush(stderr);
}

std::uint64_t now_us()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// editor-core's built asset root, compiled in by CMake (cef/CMakeLists.txt). Empty fallback (not an
// #error) so the pre-push audit's check 9 can compile this TU standalone against the pinned CEF
// headers; the runtime guard below fails the smoke loudly if an empty root ever reaches it.
#if !defined(CONTEXT_WEBUI_ASSET_DIR)
#define CONTEXT_WEBUI_ASSET_DIR ""
#endif

// The theme this smoke PINS at boot (`?ctx-smoke-theme`), so the "the user changed it" assertion has a
// known starting point on every host regardless of the runner's `prefers-color-scheme`. The scenario
// then picks a DIFFERENT one, which is what makes the recorded value evidence of a choice.
constexpr const char* kPinnedThemeId = "builtin.dark";

int finish(int code)
{
#if defined(_WIN32)
    // Session-0 carve-out (mirrors cef_shell_smoke.cpp): CEF's teardown is flaky on the self-hosted
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

    std::printf("[editor-cef-smoke-shell-settings] live theme-switch-via-Settings scenario over the "
                "real CEF pump\n");
    std::fflush(stdout);

    const std::filesystem::path asset_root = CONTEXT_WEBUI_ASSET_DIR;
    if (asset_root.empty())
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-settings] FAIL: CONTEXT_WEBUI_ASSET_DIR was not "
                             "compiled in (the webui asset root is unwired)\n");
        return finish(1);
    }

    std::error_code ec;
    const std::filesystem::path project =
        std::filesystem::temp_directory_path(ec) / "context-editor-cef-settings-smoke";
    std::filesystem::remove_all(project, ec);
    std::filesystem::create_directories(project, ec);

    // A TEMP config, never the developer's / runner's real `~/.context/config.json`: this smoke WRITES
    // through the real store, and a test that mutates the host's actual preferences would be a defect
    // in itself. Seeded with a recents entry so the merge-preservation assertion at the end has
    // something to preserve.
    const std::filesystem::path config_path = project / "config.json";
    {
        Json seed = Json::object();
        seed.set("version", Json(static_cast<std::int64_t>(1)));
        Json recents = Json::array();
        Json entry = Json::object();
        entry.set("path", Json(project.generic_string()));
        entry.set("name", Json(std::string("settings-smoke")));
        entry.set("lastOpenedMs", Json(static_cast<std::int64_t>(1)));
        recents.push_back(std::move(entry));
        seed.set("recents", std::move(recents));
        std::string seed_error;
        if (!shell::write_user_config(config_path, seed, &seed_error))
        {
            std::fprintf(stderr,
                         "[editor-cef-smoke-shell-settings] FAIL: could not seed the temp config: %s\n",
                         seed_error.c_str());
            return finish(1);
        }
    }

    const render::Extent2D size{640, 480};

    shell::WindowDesc desc;
    desc.title = "Context Editor (settings smoke)";
    desc.logical_size = size;
    desc.visible = false;
    auto backend = std::make_unique<shell::HeadlessWindowBackend>(desc);

    // --- the privileged bridges (the boot smoke's set, plus the e06d config surface) --------------
    // handshake BEFORE bridge (outlives the router that captures it); manager BEFORE the editor-state
    // bridge (whose sink captures &manager) — the declaration discipline cef_shell_smoke.cpp documents.
    shell::ShellHandshake handshake(shell::make_handshake_nonce());
    shell::BridgeRouter bridge;

    shell::PanelHost panel_host;
    shell::panels::BuiltinPanels builtin = shell::panels::install_builtin_panels(panel_host);
    SMOKE_CHECK(builtin.bound == shell::panels::hostable_panel_ids().size(),
                "every hostable built-in panel provider bound");
    SMOKE_CHECK(handshake.install(bridge), "the bridge handshake installed");
    SMOKE_CHECK(panel_host.install(bridge), "the panel.* bridge surface installed");

    // THE SUBJECT OF THIS SMOKE: the real store, bound to the temp config, installed on the real
    // router. Nothing in editor-core can write this file; every byte that lands in it below was
    // written by this object (C-F14).
    shell::UserConfigStore user_config;
    user_config.bind_path(config_path);
    SMOKE_CHECK(user_config.install(bridge), "the config.* bridge surface installed");
    SMOKE_CHECK(user_config.writes() == 0, "nothing has been persisted before the renderer runs");

    shell::cef::CefShellOptions cef_options;
    cef_options.native_window = nullptr; // windowless: no native window on a Session-0 runner
    cef_options.logical_size = size;
    cef_options.dpi = shell::DpiScale{};
    // THE app scheme (04 §1), carrying the settings-smoke flag so editor-core's boot drives the
    // scripted theme change through the REAL Settings panel, plus the theme pin that gives the
    // "the user changed it" assertion a known starting point.
    cef_options.url = std::string(shell::kAppEntryUrl) + "?ctx-smoke-settings=1&" +
                      shell::kThemePinFlag + "=" + kPinnedThemeId;
    cef_options.app_asset_root = asset_root;
    cef_options.bridge = &bridge;
    cef_options.windowless_frame_rate = 10;
    // Full verbose CEF logging so a page/console failure names its own cause on stderr (DoD).
    cef_options.verbose_logging = true;

    std::printf("[editor-cef-smoke-shell-settings] serving %s from %s (config at %s)\n",
                cef_options.url.c_str(), cef_options.app_asset_root.string().c_str(),
                config_path.string().c_str());
    std::fflush(stdout);

    std::string error;
    std::unique_ptr<shell::IBrowserHost> browser =
        shell::cef::make_cef_browser_host(cef_options, error);
    if (browser == nullptr)
    {
        std::fprintf(stderr,
                     "[editor-cef-smoke-shell-settings] FAIL: the browser did not start: %s\n",
                     error.c_str());
        return finish(1);
    }
    trace("settings", "browser started (CefInitialize + CreateBrowserSync OK)");

    shell::EditorWindowConfig config;
    config.compositor.import_options.force_software = true; // software OSR — the shipping Windows path
    config.placement_poll_us = 0;
    auto window = std::make_unique<shell::EditorWindow>(std::move(backend), std::move(browser),
                                                       config);

    auto blitter = std::make_unique<present::MemoryBlitter>();
    present::MemoryBlitter* blitter_raw = blitter.get();
    window->compositor().attach_cpu(std::move(blitter), size);

    shell::WindowManager manager(project);
    manager.add(std::move(window));
    shell::EditorWindow* editor = manager.window(0);
    SMOKE_CHECK(editor != nullptr, "the manager adopted the window");
    if (editor == nullptr)
    {
        return finish(1);
    }

    // --- the rest of the boot surface editor-core calls (identical to the sibling smokes) ---------
    // Each of these is called during boot; without them the router's deny-by-default `unknown_method`
    // refusal would trip the strict `bridge.refused() == 0` invariant below.
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

    shell::KeybindingsBridge keybindings_bridge;
    keybindings_bridge.bind_path(std::filesystem::path{});
    SMOKE_CHECK(keybindings_bridge.install(bridge), "the keybindings.get bridge surface installed");

    shell::ThemesBridge themes_bridge;
    themes_bridge.bind_directory(std::filesystem::path{});
    SMOKE_CHECK(themes_bridge.install(bridge), "the themes.get bridge surface installed");

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

    // Drive the pump until the browser has hydrated AND the settings-driven change has been PERSISTED
    // by the Shell. The 30s deadline bounds it: a scenario that selected nothing runs the clock out and
    // the assertions below fail rather than hanging.
    const std::size_t expected_renders = shell::panels::hostable_panel_ids().size();
    const auto loop_start = std::chrono::steady_clock::now();
    const auto deadline = loop_start + std::chrono::seconds(30);
    bool presented = false;
    bool traced_presented = false;
    bool traced_handshake = false;
    bool traced_config_read = false;
    bool traced_written = false;
    auto last_heartbeat = loop_start;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!manager.pump_once(now_us()))
        {
            break;
        }
        if (!presented && editor->compositor().stats().view_frames > 0 &&
            blitter_raw->blit_count() > 0)
        {
            presented = true;
        }
        if (presented && !traced_presented)
        {
            traced_presented = true;
            trace("settings", "milestone: first CEF OSR frame composited + presented");
        }
        if (!traced_handshake && handshake.complete())
        {
            traced_handshake = true;
            trace("settings", "milestone: bridge handshake complete");
        }
        if (!traced_config_read && user_config.reads() >= 1)
        {
            traced_config_read = true;
            trace("settings", "milestone: editor-core read the persisted config (config.get)");
        }
        if (!traced_written && user_config.writes() >= 1)
        {
            traced_written = true;
            trace("settings", "milestone: a Settings-driven theme change was PERSISTED (config.set)");
        }
        if (presented && handshake.complete() && user_config.writes() >= 1)
        {
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_heartbeat >= std::chrono::seconds(2))
        {
            last_heartbeat = now;
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - loop_start).count();
            std::fprintf(stderr,
                         "[editor-cef-smoke-shell-settings] heartbeat t=%lldms presented=%d "
                         "handshake=%d renders=%llu/%llu config_reads=%llu writes=%llu refusals=%llu\n",
                         static_cast<long long>(elapsed_ms), presented ? 1 : 0,
                         handshake.complete() ? 1 : 0,
                         static_cast<unsigned long long>(panel_host.renders_served()),
                         static_cast<unsigned long long>(expected_renders),
                         static_cast<unsigned long long>(user_config.reads()),
                         static_cast<unsigned long long>(user_config.writes()),
                         static_cast<unsigned long long>(user_config.refusals()));
            std::fflush(stderr);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // --- the boot invariants (the app is genuinely up — same as the sibling smokes) ---------------
    SMOKE_CHECK(presented, "a real CEF OSR frame was composited and presented within 30s");
    SMOKE_CHECK(handshake.complete(),
                "the IPC bridge round-tripped native<->JS: editor-core echoed the handshake nonce");
    SMOKE_CHECK(user_config.reads() >= 1,
                "editor-core READ the per-user config at boot (config.get) — the channel is live");

    // --- the e06d DoD assertions: the Shell persisted a choice the RENDERER made ------------------
    SMOKE_CHECK(user_config.writes() >= 1,
                "a theme picked in the live Settings panel was PERSISTED by the Shell over config.set "
                "— the C-F14 chain (panel -> request -> validate -> write) works end to end");
    SMOKE_CHECK(user_config.refusals() == 0,
                "the Settings panel's request was ACCEPTED (a refusal here means it sent a key or a "
                "value the Shell's closed vocabulary rejects)");

    {
        // Read the document back FROM DISK — not from the store's cache — so this asserts what a NEXT
        // launch would actually find.
        const Json persisted = shell::read_user_config(config_path);
        const std::string theme =
            persisted.is_object() && persisted.contains(shell::kConfigThemeKey) &&
                    persisted.at(shell::kConfigThemeKey).is_string()
                ? persisted.at(shell::kConfigThemeKey).as_string()
                : std::string();
        std::fprintf(stderr, "[editor-cef-smoke-shell-settings] persisted theme=\"%s\"\n",
                     theme.c_str());
        std::fflush(stderr);
        SMOKE_CHECK(!theme.empty(), "the config document on disk records a theme id");
        // NOT the pinned boot theme: the scenario deliberately selects a DIFFERENT one, so a value
        // equal to the pin would mean nothing was really chosen (or that something echoed the default).
        SMOKE_CHECK(theme != kPinnedThemeId,
                    "the recorded theme differs from the pinned boot theme — the value came from a "
                    "CHOICE made in the panel, not from a default the Shell could write by itself");
        // The merge-preserving read-modify-write, asserted against a live renderer: the recents entry
        // seeded above must have survived the theme write (e14c's writer would have discarded it).
        SMOKE_CHECK(persisted.is_object() && persisted.contains(shell::kConfigRecentsKey) &&
                        persisted.at(shell::kConfigRecentsKey).size() == 1,
                    "the pre-existing recents list SURVIVED the theme write (the merge-preserving "
                    "single-writer contract, not a whole-document replace)");
    }

    SMOKE_CHECK(bridge.refused() == 0, "the live settings scenario produced no envelope refusals");
    SMOKE_CHECK(bridge.secrets_blocked() == 0,
                "no handler attempted to return a protected credential during the scenario");

    manager.shutdown();
    shell::cef::shutdown();
    std::filesystem::remove_all(project, ec);

    if (g_failures != 0)
    {
        std::fprintf(stderr,
                     "[editor-cef-smoke-shell-settings] FAILED with %d assertion failure(s)\n",
                     g_failures);
        return finish(1);
    }
    std::printf("[editor-cef-smoke-shell-settings] PASS: a theme picked in the live Settings panel "
                "was persisted to the user config by the Shell (C-F14), preserving the rest of the "
                "document\n");
    return finish(0);
}
