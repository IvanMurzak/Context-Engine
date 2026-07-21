// The LIVE boot -> dock -> restart -> restore smoke (M9 e05d4) — ctest `editor-cef-smoke-shell-restore`.
//
// This is the half of the e05 group that no single-boot test can reach: that the arrangement
// editor-core builds SURVIVES A RESTART. `editor-cef-smoke-shell` proves ONE boot hydrates panels
// over the real pump; this proves TWO boots against ONE project — the first persists a real docking
// arrangement, the second (a fresh browser + a fresh editor-state store loaded from the file the
// first wrote) reads that arrangement back and REAPPLIES it. Completing it closes the e05 group.
//
// WHY A SECOND BROWSER RATHER THAN A SECOND PROCESS. CEF initialises ONCE per process
// (`make_cef_browser_host` guards CefInitialize on `g_initialized`), so a true fork-and-restart is
// not expressible in one ctest exe. Destroying the first window's browser (CEF stays initialised)
// and creating a second is exactly design 03 §7's crash-recovery drill — "CEF respawns the browser;
// editor-core reloads; layout + panel state restore from the session store" — and every CEF
// operation here (CreateBrowserSync, a browser close + pump, the store flush) is one
// `editor-cef-smoke-shell` already exercises on every run; this just sequences them twice. To keep
// the restart HONEST rather than in-memory, session 2 builds a FRESH WindowManager whose store
// LOADS `.editor/editor-state.json` from disk, and the smoke asserts that file's bytes match what
// session 1 wrote — the full round trip through the file, not a shared in-memory struct.
//
// WHAT PROVES THE RESTORE ACTUALLY HAPPENED. `editor.state.get` being served (`state_reads`) proves
// editor-core READ the blob, not that it APPLIED it. The load-bearing signal is the e05d4
// `editor.layout.restored` report: editor-core sends `layoutRestored:false` on a fresh boot (nothing
// to restore) and `layoutRestored:true` only when `LayoutPersistence.restore` reapplied a persisted
// arrangement — so the boolean is FALSE on session 1 and TRUE on session 2, which is the property
// that distinguishes a restore from a fresh boot and which no persistence-path counter can express.
//
// Like `cef_shell_smoke.cpp`, it is deliberately HEADLESS (a windowless browser, no native window),
// presents through e03's MemoryBlitter, and hard-exits on Windows after the verdict to skip CEF's
// flaky Session-0 teardown. CAUSE reporting (`OnLoadError` + `OnConsoleMessage`) is built into the
// shared CefShell client this smoke boots through, so a page that never loads names its own failure
// on stderr rather than presenting as an undiagnosable stall.

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
#include "context/editor/shell/editor_state.h"
#include "context/editor/shell/editor_state_bridge.h"
#include "context/editor/shell/ipc_bridge.h"
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
namespace contract = context::editor::contract;

namespace
{

int g_failures = 0;

void check(bool condition, const char* what, int line)
{
    if (condition)
    {
        return;
    }
    std::fprintf(stderr, "[editor-cef-smoke-shell-restore] FAIL (line %d): %s\n", line, what);
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

// The app.css background, in BGRA8 — kept in lockstep with `--editor-bg: #132a44` and mirroring
// cef_shell_smoke.cpp (the same served stylesheet paints the same background here).
constexpr std::uint8_t kAppBackgroundB = 0x44;
constexpr std::uint8_t kAppBackgroundG = 0x2a;
constexpr std::uint8_t kAppBackgroundR = 0x13;

// The composed-surface scan cef_shell_smoke.cpp documents: the wait loop polls for EXACTLY the
// property the "did the UI paint?" assertion checks, so it can neither break one poll too early (the
// CE #319 race) nor pass vacuously.
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
    // Windows runner, so exit hard once the verdict is decided. Reached ONLY after both sessions and
    // shell::cef::shutdown() — never between the two boots.
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
#else
    return code;
#endif
}

// What one session proved, read out of its bridge/store before teardown so the caller can assert on
// it once the session's non-movable objects are gone.
struct SessionOutcome
{
    bool browser_started = false;
    bool hydrated = false; // painted + handshake complete + panels rendered + a non-uniform surface
    std::size_t states_published = 0;
    std::size_t state_reads = 0;
    std::size_t restore_reports = 0;
    bool layout_restored = false;
    int store_write_count = 0;
    // The store's layout blob at session START (what its WindowManager loaded from disk) and END
    // (after the flush). Deep copies, so they outlive the store.
    contract::Json loaded_layout;
    contract::Json persisted_layout;
};

struct SessionConfig
{
    const char* label;
    bool arrange;        // append ?ctx-smoke-arrange and wait for the resulting publish
    bool expect_restore; // wait for the restore read + report before finishing
};

// Boot one live editor-core over the app scheme, drive the integrated pump until it has hydrated
// (and, per the config, published an arrangement or reported a restore), then tear the browser down
// (CEF stays initialised) and return what the bridge saw. ALL non-movable objects (router, host,
// bridge, manager) live and die inside this function, in an order that keeps the browser destroyed
// before the router it points at (see the teardown note at the bottom).
SessionOutcome run_session(const std::filesystem::path& project,
                           const std::filesystem::path& asset_root, render::Extent2D size,
                           const SessionConfig& cfg)
{
    SessionOutcome out;

    // handshake BEFORE bridge (it must outlive the router that captures it); manager BEFORE the
    // editor-state bridge (whose sink captures &manager) — the same declaration discipline
    // cef_shell_smoke.cpp / editor_main.cpp document.
    shell::ShellHandshake handshake(shell::make_handshake_nonce());
    shell::BridgeRouter bridge;

    shell::PanelHost panel_host;
    shell::panels::BuiltinPanels builtin = shell::panels::install_builtin_panels(panel_host);
    (void)builtin;

    shell::WindowManager manager(project);
    // What the constructor loaded from `.editor/editor-state.json` — null on a fresh project, the
    // persisted arrangement after a prior session wrote one. Captured before the browser can change it.
    out.loaded_layout = manager.state_store().state().layout;

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

    if (!handshake.install(bridge) || !panel_host.install(bridge) ||
        !editor_state_bridge.install(bridge))
    {
        std::fprintf(stderr, "[%s] FAIL: a bridge surface refused to install\n", cfg.label);
        return out;
    }

    shell::WindowDesc desc;
    desc.title = "Context Editor (restore smoke)";
    desc.logical_size = size;
    desc.visible = false;
    auto backend = std::make_unique<shell::HeadlessWindowBackend>(desc);

    shell::cef::CefShellOptions cef_options;
    cef_options.native_window = nullptr; // windowless: no native window on a Session-0 runner
    cef_options.logical_size = size;
    cef_options.dpi = shell::DpiScale{};
    // THE app scheme (04 §1). Session 1 carries the smoke-arrange flag so its FIRST arrangement is a
    // real, non-default one; session 2 boots the plain entry URL and restores whatever is on disk.
    std::string url = shell::kAppEntryUrl;
    if (cfg.arrange)
    {
        url += "?ctx-smoke-arrange=1";
    }
    cef_options.url = url;
    cef_options.app_asset_root = asset_root;
    cef_options.bridge = &bridge;
    cef_options.windowless_frame_rate = 10;

    std::string error;
    std::unique_ptr<shell::IBrowserHost> browser =
        shell::cef::make_cef_browser_host(cef_options, error);
    if (browser == nullptr)
    {
        std::fprintf(stderr, "[%s] FAIL: the browser did not start: %s\n", cfg.label, error.c_str());
        return out;
    }
    out.browser_started = true;

    shell::EditorWindowConfig config;
    config.compositor.import_options.force_software = true; // software OSR — the shipping Windows path
    config.placement_poll_us = 0;
    auto window = std::make_unique<shell::EditorWindow>(std::move(backend), std::move(browser),
                                                        config);

    auto blitter = std::make_unique<present::MemoryBlitter>();
    present::MemoryBlitter* blitter_raw = blitter.get();
    window->compositor().attach_cpu(std::move(blitter), size);

    manager.add(std::move(window));
    shell::EditorWindow* editor = manager.window(0);
    if (editor == nullptr)
    {
        std::fprintf(stderr, "[%s] FAIL: the manager adopted no window\n", cfg.label);
        return out;
    }

    // Drive the pump until the browser has painted a non-uniform UI (the CE #319 / e05d2 wait
    // discipline — wait for EXACTLY the property the assertion checks) AND the session's own
    // completion signal has arrived: a published arrangement for the arranging session, a restore
    // read + report for the restoring one. The 30s deadline bounds it, so a genuine no-restore
    // regression runs out the clock and the caller's assertion fails rather than hanging.
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
            blitter_raw->blit_count() > 0)
        {
            presented = true;
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
        if (hydrated)
        {
            out.hydrated = true;
            const bool arranged =
                !cfg.arrange || editor_state_bridge.states_published() >= 1;
            const bool restored =
                !cfg.expect_restore ||
                (editor_state_bridge.state_reads() >= 1 &&
                 editor_state_bridge.restore_reports() >= 1);
            if (arranged && restored)
            {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Force the debounced store write so the on-disk file reflects the last published arrangement
    // (a no-op when nothing is dirty), then read the counters + blob out before teardown.
    (void)manager.state_store().flush_now();
    out.states_published = editor_state_bridge.states_published();
    out.state_reads = editor_state_bridge.state_reads();
    out.restore_reports = editor_state_bridge.restore_reports();
    out.layout_restored = editor_state_bridge.layout_restored();
    out.store_write_count = manager.state_store().write_count();
    out.persisted_layout = manager.state_store().state().layout;

    // Close + destroy the window (and its browser) NOW, while `bridge` — which the browser points at
    // — is still alive: manager.shutdown() clears its windows, whose EditorWindow destructors run the
    // browser's CloseBrowser + pump. `editor_state_bridge` (declared after `manager`) is destroyed
    // before it at return, so its region sink's `&manager` capture never dangles; nothing is pumped
    // after this, so no bridge handler is invoked during the unwind. CEF stays INITIALISED — only the
    // caller's final shutdown() tears it down.
    manager.shutdown();
    return out;
}

// editor-core's built asset root, compiled in by CMake (see cef/CMakeLists.txt). Empty fallback (not
// an #error) so the pre-push audit's check 9 can compile this TU standalone against the pinned CEF
// headers; the runtime guard below fails the smoke loudly if an empty root ever reaches it.
#if !defined(CONTEXT_WEBUI_ASSET_DIR)
#define CONTEXT_WEBUI_ASSET_DIR ""
#endif

} // namespace

int main(int argc, char** argv)
{
    // Subprocess re-entry FIRST: CEF's renderer/GPU/utility processes re-exec this binary.
    const int subprocess_exit = shell::cef::execute_subprocess(argc, argv);
    if (subprocess_exit >= 0)
    {
        return subprocess_exit;
    }

    std::printf("[editor-cef-smoke-shell-restore] live boot -> dock -> restart -> restore\n");

    const std::filesystem::path asset_root = CONTEXT_WEBUI_ASSET_DIR;
    if (asset_root.empty())
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-restore] FAIL: CONTEXT_WEBUI_ASSET_DIR was not "
                             "compiled in (the webui asset root is unwired)\n");
        return finish(1);
    }

    std::error_code ec;
    const std::filesystem::path project =
        std::filesystem::temp_directory_path(ec) / "context-editor-cef-restore-smoke";
    std::filesystem::remove_all(project, ec);
    std::filesystem::create_directories(project, ec);

    const render::Extent2D size{640, 480};

    // --- SESSION 1: boot from the app scheme, mount panels, drive a dock change, persist -----------
    const SessionOutcome first =
        run_session(project, asset_root, size, SessionConfig{"session1", true, false});
    SMOKE_CHECK(first.browser_started, "session 1: a real windowless CEF browser started");
    SMOKE_CHECK(first.hydrated,
                "session 1: booted from context-editor://, the handshake completed, and the panels "
                "hydrated into a non-uniform composited frame");
    SMOKE_CHECK(!first.loaded_layout.is_object(),
                "session 1 started with NO persisted arrangement — a fresh project");
    SMOKE_CHECK(first.restore_reports >= 1,
                "session 1: editor-core reported its (empty) restore outcome");
    SMOKE_CHECK(!first.layout_restored,
                "session 1 restored NOTHING (fresh project) — layoutRestored:false, the false half "
                "of the signal that distinguishes a restore from a fresh boot");
    SMOKE_CHECK(first.states_published >= 1,
                "session 1: the scripted dock change published an arrangement over "
                "editor.state.publish");
    SMOKE_CHECK(first.store_write_count >= 1,
                "session 1: the Shell wrote the arrangement to the editor-state file");
    SMOKE_CHECK(first.persisted_layout.is_object() &&
                    first.persisted_layout.at("panels").is_object(),
                "the persisted arrangement is a real Dockview layout (a toJSON with a panels object), "
                "not a null/empty stub");

    // The on-disk file exists (the disk half of the round trip, independent of the in-memory store).
    const std::filesystem::path state_file = shell::editor_state_path(project);
    SMOKE_CHECK(std::filesystem::exists(state_file),
                "the editor-state file exists on disk after session 1");

    // --- RESTART -> SESSION 2: a fresh browser + a fresh store loaded from disk, then restore ------
    const SessionOutcome second =
        run_session(project, asset_root, size, SessionConfig{"session2", false, true});
    SMOKE_CHECK(second.browser_started,
                "session 2: a fresh browser started in the same CEF (the restart)");
    SMOKE_CHECK(second.hydrated, "session 2: booted and hydrated its panels");
    SMOKE_CHECK(second.loaded_layout.is_object(),
                "session 2's fresh store LOADED a persisted arrangement from disk — the restart read "
                "the file session 1 wrote");
    SMOKE_CHECK(second.loaded_layout.dump() == first.persisted_layout.dump(),
                "the arrangement session 2 loaded from disk is byte-identical to what session 1 "
                "persisted — the full round trip through the editor-state file");
    SMOKE_CHECK(second.state_reads >= 1,
                "session 2: editor-core read the persisted arrangement over editor.state.get");
    SMOKE_CHECK(second.restore_reports >= 1, "session 2: editor-core reported its restore outcome");
    SMOKE_CHECK(second.layout_restored,
                "session 2 REAPPLIED the persisted arrangement — layoutRestored:true. False on a "
                "fresh boot, so this is the end-to-end proof the arrangement came back");

    shell::cef::shutdown();
    std::filesystem::remove_all(project, ec);

    if (g_failures != 0)
    {
        std::fprintf(stderr,
                     "[editor-cef-smoke-shell-restore] FAILED with %d assertion failure(s)\n",
                     g_failures);
        return finish(1);
    }
    std::printf("[editor-cef-smoke-shell-restore] PASS: a live arrangement was persisted, survived a "
                "browser restart, and was restored from the editor-state file\n");
    return finish(0);
}
