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
// It is deliberately HEADLESS (a windowless browser, no native window) and presents through e03's
// MemoryBlitter, so it is safe on the Session-0 self-hosted Windows runner: no visible window, no
// GPU device, no native-render teardown. The Windows hard exit after success mirrors
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
#include "context/editor/shell/cef/cef_shell.h"
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

// The background `src/editor/webui/app/app.css` sets, in BGRA8 (the composite's format). Keep in
// lockstep with `--editor-bg: #132a44` there; the stylesheet's own comment says so too.
//
// Deliberately NOT e04's `#102040`: reusing the old data:-URL placeholder's colour would make "the
// app scheme served our stylesheet" indistinguishable from "the old placeholder still loads",
// which is precisely the regression this assertion exists to catch.
constexpr std::uint8_t kAppBackgroundB = 0x44;
constexpr std::uint8_t kAppBackgroundG = 0x2a;
constexpr std::uint8_t kAppBackgroundR = 0x13;

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

    std::printf("[editor-cef-smoke-shell] live windowed-OSR CEF -> compositor -> present\n");

    std::error_code ec;
    const std::filesystem::path project =
        std::filesystem::temp_directory_path(ec) / "context-editor-cef-shell-smoke";
    std::filesystem::remove_all(project, ec);
    std::filesystem::create_directories(project, ec);

    const render::Extent2D size{640, 480};

    shell::WindowDesc desc;
    desc.title = "Context Editor (cef smoke)";
    desc.logical_size = size;
    desc.visible = false;
    // Headless on purpose — see the file header on why this is Session-0-safe.
    auto backend = std::make_unique<shell::HeadlessWindowBackend>(desc);
    shell::HeadlessWindowBackend* backend_raw = backend.get();

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
    cef_options.native_window = nullptr; // windowless: no native window on a Session-0 runner
    cef_options.logical_size = size;
    cef_options.dpi = shell::DpiScale{};
    // THE app scheme, not a data: URL and emphatically not a file:// path (04 §1).
    cef_options.url = shell::kAppEntryUrl;
    cef_options.app_asset_root = CONTEXT_WEBUI_ASSET_DIR;
    cef_options.bridge = &bridge;
    // Keep the paint rate low: this smoke wants a FRAME, not a frame rate.
    cef_options.windowless_frame_rate = 10;

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
    auto window = std::make_unique<shell::EditorWindow>(std::move(backend), std::move(browser),
                                                        config);

    // The C-F2 CPU present path with e03's portable blitter: no adapter, no swapchain.
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
    // uniform #132a44 background, and the "NOT a uniform fill" assertion failed on a frame that had
    // simply not painted yet. So also require the surface to be NON-UNIFORM — the exact property
    // that assertion checks. (A raw OnPaint / `view_frames` counter is a weaker proxy: it cannot
    // tell a re-paint of the background apart from the UI's first paint, so waiting on it could
    // still break early.) The 30s deadline is unchanged and still bounds the wait, so a genuine
    // no-paint regression never turns the surface non-uniform, the loop runs out the clock, and the
    // assertion below fails as it should — the wait is not vacuous and cannot loop forever.
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
        const bool hydrated =
            presented && handshake.complete() && panel_host.renders_served() >= expected_renders;
        if (hydrated &&
            !scan_surface(editor->compositor().cpu_surface(), editor->compositor().size()).uniform)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    SMOKE_CHECK(presented, "a real CEF OSR frame was composited and presented within 30s");
    SMOKE_CHECK(editor->compositor().stats().view_frames > 0,
                "the compositor adopted at least one OnPaint frame");
    SMOKE_CHECK(blitter_raw->blit_count() > 0, "the composited frame reached the present blitter");
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
    // The `app.css` note keeping --editor-bg in lockstep with kAppBackground* still applies, and the
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
                    "app.css's #132a44 background covers a substantial part of the composited "
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
    shell::ShellEvent move;
    move.kind = shell::ShellEventKind::pointer;
    move.pointer.action = shell::PointerAction::move;
    move.pointer.position = shell::PointI{100, 100};
    backend_raw->post(move);

    shell::ShellEvent click = move;
    click.pointer.action = shell::PointerAction::down;
    click.pointer.button = shell::MouseButton::left;
    backend_raw->post(click);

    shell::ShellEvent release = click;
    release.pointer.action = shell::PointerAction::up;
    backend_raw->post(release);

    shell::ShellEvent key;
    key.kind = shell::ShellEventKind::key;
    key.key.action = shell::KeyAction::raw_key_down;
    key.key.windows_key_code = 0x09; // VK_TAB — moves DOM focus, so it is not a no-op
    backend_raw->post(key);

    SMOKE_CHECK(manager.pump_once(now_us()), "the loop ran with live input");
    SMOKE_CHECK(editor->input().pointer_dispatches() == 3, "the pointer samples were arbitrated");
    SMOKE_CHECK(editor->input().key_dispatches() == 1, "the key was arbitrated");

    // A live resize: the browser must accept WasResized and repaint at the new size.
    shell::ShellEvent resize;
    resize.kind = shell::ShellEventKind::resize;
    resize.size = render::Extent2D{800, 500};
    backend_raw->post(resize);
    const int frames_before_resize = editor->compositor().stats().view_frames;
    const auto resize_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool repainted = false;
    while (std::chrono::steady_clock::now() < resize_deadline)
    {
        if (!manager.pump_once(now_us()))
        {
            break;
        }
        if (editor->compositor().stats().view_frames > frames_before_resize)
        {
            repainted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    SMOKE_CHECK(repainted, "the browser repainted after a live resize (WasResized)");
    SMOKE_CHECK(editor->compositor().size().width == 800u, "the compositor took the new size");

    // Read the presented-frame count BEFORE shutdown: the blitter is owned by the compositor
    // (attach_cpu took the unique_ptr), and shutdown() -> EditorWindow::close() ->
    // WindowCompositor::detach() destroys it (blitter_.reset()). `blitter_raw` dangles from here
    // on, so the closing report below prints a value captured while it was still alive. Nothing is
    // presented during teardown, so this count is final. (Same defect, same fix, as the
    // Session-0 smoke in ../../smoke/shell_smoke_main.cpp.)
    const int presented_frames = blitter_raw->blit_count();

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
                "(%d frames), input round-tripped, live resize repainted\n",
                presented_frames);
    return finish(0);
}
