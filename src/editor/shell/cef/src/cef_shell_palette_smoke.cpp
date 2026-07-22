// The LIVE command-driven CEF smoke (M9 e07d) — ctest `editor-cef-smoke-shell-palette`.
//
// This is the T2 half of e07d: it proves the WHOLE D8 command layer works end-to-end through the real
// CEF pump. Where `editor-cef-smoke-shell` proves the boot handshake + panel hydration, this one boots
// the SAME live windowless browser and then drives a scenario PURELY through the palette / command
// dispatch — the "the palette surface ≡ the scriptable surface" story (design 10), exactly as an agent
// or a T2 CI assertion would drive the editor.
//
// HOW IT DRIVES, AND WHAT IT OBSERVES. editor-core's boot, under the `?ctx-smoke-palette` URL flag
// (boot.ts `runPaletteSmoke`), runs a scripted scenario over the REAL palette + registry: OPEN the
// palette via its command, FILTER by a fuzzy query, then EXECUTE the top match — `view.panel.close`.
// That command's handler closes a docked panel, which fires a Dockview layout change, which
// LayoutPersistence PUBLISHES over `editor.state.publish`. So the OBSERVABLE this smoke asserts is
// `EditorStateBridge::states_published() >= 1`: a layout publish that can ONLY have happened because a
// palette-driven command executed through the live renderer (a fresh boot with no interaction publishes
// nothing — the `editor-cef-smoke-shell` boot smoke never sees a publish). It is the same
// `states_published()` observable the e05d4 restore smoke's arranging session already relies on, so no
// new bridge surface is added: this smoke installs exactly the bridges cef_shell_smoke.cpp does.
//
// CAUSE REPORTING (DoD). Failures must name a CAUSE, not just a verdict. `verbose_logging` is on so
// Chromium's own renderer/console errors interleave onto stderr, the shared CefShell client's
// OnLoadError + OnConsoleMessage report a page that never loads, and every milestone below is a flushed
// `trace()` line — the LAST one before a gap in the CI log is where a hang died. A palette scenario that
// silently executed nothing runs the 30s clock out and the `states_published()` assertion fails as it
// should (the wait is not vacuous and cannot loop forever).
//
// It boots exactly as cef_shell_smoke.cpp does — a windowless browser presenting through e03's
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
#include "context/editor/shell/cef/cef_shell.h"
#include "context/editor/shell/editor_state_bridge.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/keybindings_bridge.h"
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/builtin_panels.h"
#include "context/editor/shell/shell.h"

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
    std::fprintf(stderr, "[editor-cef-smoke-shell-palette] FAIL (line %d): %s\n", line, what);
    ++g_failures;
}

#define SMOKE_CHECK(cond, what) check((cond), (what), __LINE__)

// Flushed progress trace — the only failure signal for a hang inside the live CEF pump is a stalled
// heartbeat, and CEF does not link on the GCC dev host so this is not locally reproducible. Mirrors
// cef_shell_restore_smoke.cpp's trace() discipline.
void trace(const char* label, const char* msg)
{
    std::fprintf(stderr, "[editor-cef-smoke-shell-palette] [%s] %s\n", label, msg);
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
// headers; the runtime guard below fails the smoke loudly if an empty root ever reaches it (the same
// rationale cef_shell_smoke.cpp documents at length).
#if !defined(CONTEXT_WEBUI_ASSET_DIR)
#define CONTEXT_WEBUI_ASSET_DIR ""
#endif

// The app.css background, in BGRA8 — kept in lockstep with `--editor-bg: #132a44` and mirroring
// cef_shell_smoke.cpp (the same served stylesheet paints the same background here).
constexpr std::uint8_t kAppBackgroundB = 0x44;
constexpr std::uint8_t kAppBackgroundG = 0x2a;
constexpr std::uint8_t kAppBackgroundR = 0x13;

// The composed-surface scan cef_shell_smoke.cpp documents: the wait loop polls for EXACTLY the property
// the "did the UI paint?" assertion checks, so it can neither break one poll too early (the CE #319
// race) nor pass vacuously.
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

    std::printf("[editor-cef-smoke-shell-palette] live command-driven palette scenario over the real "
                "CEF pump\n");
    std::fflush(stdout);

    const std::filesystem::path asset_root = CONTEXT_WEBUI_ASSET_DIR;
    if (asset_root.empty())
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-palette] FAIL: CONTEXT_WEBUI_ASSET_DIR was not "
                             "compiled in (the webui asset root is unwired)\n");
        return finish(1);
    }

    std::error_code ec;
    const std::filesystem::path project =
        std::filesystem::temp_directory_path(ec) / "context-editor-cef-palette-smoke";
    std::filesystem::remove_all(project, ec);
    std::filesystem::create_directories(project, ec);

    const render::Extent2D size{640, 480};

    shell::WindowDesc desc;
    desc.title = "Context Editor (palette smoke)";
    desc.logical_size = size;
    desc.visible = false;
    auto backend = std::make_unique<shell::HeadlessWindowBackend>(desc);

    // --- the privileged bridges (identical set to cef_shell_smoke.cpp) ---------------------------
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

    shell::cef::CefShellOptions cef_options;
    cef_options.native_window = nullptr; // windowless: no native window on a Session-0 runner
    cef_options.logical_size = size;
    cef_options.dpi = shell::DpiScale{};
    // THE app scheme (04 §1), carrying the palette-smoke flag so editor-core's boot drives the scripted
    // OPEN -> FILTER -> EXECUTE scenario over the real palette + registry.
    cef_options.url = std::string(shell::kAppEntryUrl) + "?ctx-smoke-palette=1";
    cef_options.app_asset_root = asset_root;
    cef_options.bridge = &bridge;
    cef_options.windowless_frame_rate = 10;
    // Full verbose CEF logging so a page/console failure names its own cause on stderr (DoD).
    cef_options.verbose_logging = true;

    SMOKE_CHECK(!cef_options.app_asset_root.empty(),
                "CONTEXT_WEBUI_ASSET_DIR was compiled in (the webui asset root is wired)");
    std::printf("[editor-cef-smoke-shell-palette] serving %s from %s\n", cef_options.url.c_str(),
                cef_options.app_asset_root.string().c_str());
    std::fflush(stdout);

    std::string error;
    std::unique_ptr<shell::IBrowserHost> browser =
        shell::cef::make_cef_browser_host(cef_options, error);
    if (browser == nullptr)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-palette] FAIL: the browser did not start: %s\n",
                     error.c_str());
        return finish(1);
    }
    trace("palette", "browser started (CefInitialize + CreateBrowserSync OK)");

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

    // --- the editor-state + region-map surface --------------------------------------------------
    // editor-core's LayoutPersistence calls editor.state.get on boot and editor.state.publish /
    // editor.regions.publish on every layout change — including the one the palette-driven
    // `view.panel.close` triggers, which is THIS smoke's observable. Wire it exactly as
    // cef_shell_smoke.cpp / editor_main.cpp do; `manager` outlives the handlers this install captures.
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

    // --- the keybindings read surface (e07c) ----------------------------------------------------
    // editor-core's boot calls keybindings.get; serve it (present:false on the empty path) so the live
    // boot is not refused. Same deterministic-absent binding as cef_shell_smoke.cpp.
    shell::KeybindingsBridge keybindings_bridge;
    keybindings_bridge.bind_path(std::filesystem::path{});
    SMOKE_CHECK(keybindings_bridge.install(bridge), "the keybindings.get bridge surface installed");

    // Drive the pump until the browser has hydrated (painted a non-uniform UI + handshake complete +
    // panels rendered — the CE #319 / e05d2 wait discipline) AND the palette-driven command has
    // PUBLISHED a layout change (states_published >= 1). The 30s deadline bounds it: a palette scenario
    // that executed nothing runs the clock out and the assertion below fails rather than hanging.
    const std::size_t expected_renders = shell::panels::hostable_panel_ids().size();
    const auto loop_start = std::chrono::steady_clock::now();
    const auto deadline = loop_start + std::chrono::seconds(30);
    bool presented = false;
    bool traced_presented = false;
    bool traced_handshake = false;
    bool traced_panels = false;
    bool traced_published = false;
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
            trace("palette", "milestone: first CEF OSR frame composited + presented");
        }
        if (!traced_handshake && handshake.complete())
        {
            traced_handshake = true;
            trace("palette", "milestone: bridge handshake complete");
        }
        if (!traced_panels && panel_host.renders_served() >= expected_renders)
        {
            traced_panels = true;
            trace("palette", "milestone: all hostable panels rendered");
        }
        if (!traced_published && editor_state_bridge.states_published() >= 1)
        {
            traced_published = true;
            trace("palette", "milestone: a palette-driven command published a layout change "
                             "(editor.state.publish)");
        }
        bool hydrated = false;
        if (presented && handshake.complete() &&
            panel_host.renders_served() >= expected_renders)
        {
            const SurfaceScan scan =
                scan_surface(editor->compositor().cpu_surface(), editor->compositor().size());
            hydrated = !scan.uniform && scan.scanned > 0 &&
                       scan.background_texels > scan.scanned / 10u;
        }
        if (hydrated && editor_state_bridge.states_published() >= 1)
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
                         "[editor-cef-smoke-shell-palette] heartbeat t=%lldms presented=%d "
                         "handshake=%d renders=%llu/%llu published=%llu\n",
                         static_cast<long long>(elapsed_ms), presented ? 1 : 0,
                         handshake.complete() ? 1 : 0,
                         static_cast<unsigned long long>(panel_host.renders_served()),
                         static_cast<unsigned long long>(expected_renders),
                         static_cast<unsigned long long>(editor_state_bridge.states_published()));
            std::fflush(stderr);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // --- the boot invariants (the app is genuinely up — same as the boot smoke) ------------------
    SMOKE_CHECK(presented, "a real CEF OSR frame was composited and presented within 30s");
    SMOKE_CHECK(handshake.complete(),
                "the IPC bridge round-tripped native<->JS: editor-core echoed the handshake nonce");
    SMOKE_CHECK(panel_host.renders_served() >= expected_renders,
                "every hostable panel hydrated over the bridge — the app layer ran");
    {
        const SurfaceScan scan =
            scan_surface(editor->compositor().cpu_surface(), editor->compositor().size());
        SMOKE_CHECK(scan.scanned > 0 && scan.background_texels > scan.scanned / 10u,
                    "app.css's #132a44 background covers a substantial part of the composited frame");
        SMOKE_CHECK(!scan.uniform, "a real docking UI was painted on top of the background");
    }

    // --- the e07d DoD assertion: a command was driven THROUGH THE PALETTE, end to end ------------
    //
    // THIS IS THE PROOF THE WHOLE D8 COMMAND LAYER WORKS OVER THE REAL PUMP. states_published() is
    // incremented ONLY by an editor.state.publish arriving over the router, which the boot-time
    // `runPaletteSmoke` triggers by OPENING the palette, FILTERING, and EXECUTING `view.panel.close`
    // through the ONE registry — a fresh boot with no interaction publishes nothing (the boot smoke
    // never sees this), so a non-zero value here can only be the palette-driven command having run in
    // the live renderer.
    SMOKE_CHECK(editor_state_bridge.states_published() >= 1,
                "a palette-driven command (view.panel.close, executed via the command palette) fired a "
                "layout change that published over editor.state.publish — the D8 command layer works "
                "end to end through the real CEF pump");
    // The scenario's dispatch path rides the same trusted bridge as the boot handshake; nothing it did
    // should have been refused.
    SMOKE_CHECK(bridge.refused() == 0, "the live palette scenario produced no envelope refusals");
    SMOKE_CHECK(bridge.secrets_blocked() == 0,
                "no handler attempted to return a protected credential during the scenario");

    manager.shutdown();
    shell::cef::shutdown();
    std::filesystem::remove_all(project, ec);

    if (g_failures != 0)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-palette] FAILED with %d assertion failure(s)\n",
                     g_failures);
        return finish(1);
    }
    std::printf("[editor-cef-smoke-shell-palette] PASS: the command palette drove a command end to end "
                "through the live CEF pump (a layout change published over editor.state.publish)\n");
    return finish(0);
}
