// The LIVE HTML5 DRAG-AND-DROP smoke (M9 b1, D11) — ctest `editor-cef-smoke-shell-osrdrag`.
//
// WHAT ONLY THIS CAN PROVE. `editor-shell-test_osr_drag` pins the protocol's state machine and
// `editor-shell-test_shell` pins the owner loop's wiring — both on all three default `build` legs,
// neither with a browser. What NEITHER can reach is the only claim that actually matters to a user:
// that a REAL renderer really starts a drag, that the injections the Shell answers with are really
// ACCEPTED by CEF, and that a real `drop` event really fires in the document. Every one of those
// crosses the process boundary into Chromium, and until they hold, "HTML5 drag-and-drop works" is a
// statement about our own code rather than about the editor.
//
// THE SCENARIO, and why each step is the one it is:
//
//   1. Boot the real editor over the app scheme, exactly as the boot smoke does, and wait for the
//      mounted UI to paint.
//   2. Inject a FULL-WINDOW `draggable` overlay through `IBrowserHost::execute_script`, painted a
//      colour nothing else in the editor uses (MAGENTA). Waiting until the composited frame is
//      mostly magenta is the NON-VACUITY ANCHOR of the whole smoke: it proves the fixture really
//      mounted and really reached the present path, so the "no green yet" assertion below is a
//      statement about the DROP rather than about an overlay that was never there.
//   3. Drive a REAL gesture through the window backend — press, several moves, release. On the
//      Linux leg the ctest registration passes `--real-window`, so those samples go through the X
//      SERVER and come back in through the shipping `translate_x11_event` decoder (e12a-x11-legs);
//      on Windows and macOS they ride the headless queue, which is what keeps this Session-0 safe.
//   4. The renderer's `dragstart` reaches `CefRenderHandler::StartDragging`, which is the exact
//      member that used to return false and abort every drag in the editor.
//   5. The Shell answers with `DragTargetDragEnter` / `DragTargetDragOver`, and CEF answers THOSE
//      with `UpdateDragCursor` — which is the load-bearing positive: our injections cannot produce
//      a drag operation by themselves, so a non-`none` operation here is CEF telling us it
//      processed them inside a live document.
//   6. The release becomes `DragTargetDrop`, the overlay's own `drop` handler repaints it GREEN,
//      and the composited frame turns green. That colour can appear by NO other route.
//
// ⚠ THE FIXTURE IS THE SMOKE'S OWN, NOT DOCKVIEW'S TAB STRIP, and that is deliberate rather than a
// shortcut. What b1 owns is the PROTOCOL; Dockview's tab drag is upstream code that consumes it
// (`panelhost.ts` is unchanged by this task, and the task's own scope says a failure after this
// lands is the protocol's fault, not editor-core's). A smoke that dragged a real tab would assert
// Dockview's drop geometry — a moving target whose pixel positions no test should encode — and
// would fail for reasons that are not this task's, on a blocking gate. Asserting the protocol on a
// fixture the smoke controls proves precisely the thing b1 is responsible for, and the ordinary
// `webui-ts-*` tier plus the boot smoke cover the arrangement above it.
//
// Headless by default and Session-0 safe (windowless browser, the C-F2 CPU present path, the
// Windows hard exit after the verdict), exactly like its ten sibling smokes.

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
#include "context/editor/shell/osr_drag.h"
#include "context/editor/shell/package_grants.h"
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/builtin_panels.h"
#include "context/editor/shell/session_bridge.h"
#include "context/editor/shell/shell.h"
#include "context/editor/shell/smoke/smoke_window.h"
#include "context/editor/shell/themes_bridge.h"
#include "context/editor/shell/user_config.h"
#include "context/editor/shell/welcome.h"
#include "context/editor/shell/window_bridge.h"
#include "context/editor/shell/window_registry.h"

#include <algorithm>
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

namespace
{

int g_failures = 0;

void check(bool condition, const char* what, int line)
{
    if (condition)
    {
        return;
    }
    std::fprintf(stderr, "[editor-cef-smoke-shell-osrdrag] FAIL (line %d): %s\n", line, what);
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

// Empty fallback (not #error), so the pre-push audit's check 9 can compile this TU standalone
// against the pinned CEF headers with none of CMake's defines — the sibling smokes' rationale,
// verbatim.
#if !defined(CONTEXT_WEBUI_ASSET_DIR)
#define CONTEXT_WEBUI_ASSET_DIR ""
#endif

// Pinned so the boot is deterministic regardless of the host's `prefers-color-scheme` — the same
// reason the boot smoke pins it, and doubly so here where the assertions are per-pixel.
constexpr const char* kSmokeThemeId = "builtin.dark";

// The two fixture colours, as RGB. Chosen so that NEITHER can be produced by the editor itself: the
// built-in themes are greys and desaturated blues, and a zero-filled or black surface is neither.
//
// MAGENTA is the "the fixture is mounted" anchor; GREEN is the "a real `drop` event fired" verdict.
// Keeping them maximally far apart in every channel means no anti-aliased edge between the overlay
// and anything under it can be mistaken for either.
constexpr std::uint8_t kOverlayR = 0xFF;
constexpr std::uint8_t kOverlayG = 0x00;
constexpr std::uint8_t kOverlayB = 0xFF;
constexpr std::uint8_t kDroppedR = 0x00;
constexpr std::uint8_t kDroppedG = 0xFF;
constexpr std::uint8_t kDroppedB = 0x00;

// How many texels of a given colour the composed surface holds. The surface is premultiplied BGRA8
// (browser.h), so the byte order here is B, G, R.
struct ColourScan
{
    std::size_t scanned = 0;
    std::size_t matched = 0;
};

ColourScan scan_colour(const std::vector<std::uint8_t>& surface, render::Extent2D composed,
                       std::uint8_t b, std::uint8_t g, std::uint8_t r)
{
    ColourScan scan;
    // CLAMPED ONCE rather than bounds-checked per texel: the surface is the only thing that can be
    // short (a frame composed at a smaller size than `composed` claims), and it is short by the same
    // amount for every texel — so re-deriving the limit 307,200 times answers one question with one
    // answer. `scanned` still reports what was actually read, which is what every caller compares
    // against.
    const std::size_t texels =
        std::min(static_cast<std::size_t>(composed.width) * static_cast<std::size_t>(composed.height),
                 surface.size() / 4u);
    scan.scanned = texels;
    for (std::size_t i = 0; i < texels; ++i)
    {
        const std::size_t offset = i * 4u;
        if (surface[offset + 0] == b && surface[offset + 1] == g && surface[offset + 2] == r)
        {
            ++scan.matched;
        }
    }
    return scan;
}

// THE FIXTURE. A full-window `draggable` overlay with the three handlers an HTML5 drag needs, and
// nothing else.
//
// ⚠ `dragover` MUST call `preventDefault()`, and that is not boilerplate: without it the element is
// not a drop target at all, the browser reports `DRAG_OPERATION_NONE` forever, and no `drop` event
// is ever dispatched — so the smoke would fail with every one of OUR paths working. It is the one
// line of this fixture whose absence would look like a defect in the code under test.
//
// The overlay is `position:fixed; inset:0` so the gesture can land anywhere in the client area
// without this smoke having to know where any editor element is. `pointer-events:auto` is explicit
// because the editor's own chrome sets `pointer-events` on several layers.
constexpr const char* kFixtureScript = R"JS(
(function () {
  var el = document.createElement('div');
  el.id = 'ctx-osrdrag-fixture';
  el.setAttribute('draggable', 'true');
  el.style.cssText =
    'position:fixed;inset:0;z-index:2147483000;background:#ff00ff;pointer-events:auto';
  el.addEventListener('dragstart', function (e) {
    e.dataTransfer.effectAllowed = 'copy';
    e.dataTransfer.setData('text/plain', 'ctx-osrdrag');
  });
  el.addEventListener('dragover', function (e) {
    e.preventDefault();
    e.dataTransfer.dropEffect = 'copy';
  });
  el.addEventListener('drop', function (e) {
    e.preventDefault();
    el.style.background = '#00ff00';
  });
  document.body.appendChild(el);
})();
)JS";

// One pointer sample, with the button state a drag is always in. `down`/`move` hold the left button;
// `up` releases it — Chromium reads the button flags off the event's modifiers (`to_cef_modifiers`),
// and a move with no button held ends whatever drag it was in the middle of.
shell::ShellEvent drag_sample(shell::PointerAction action, shell::PointI position)
{
    shell::ShellEvent event;
    event.kind = shell::ShellEventKind::pointer;
    event.pointer.action = action;
    event.pointer.position = position;
    event.pointer.modifiers.left_button_down = action != shell::PointerAction::up;
    if (action == shell::PointerAction::down || action == shell::PointerAction::up)
    {
        event.pointer.button = shell::MouseButton::left;
    }
    return event;
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

    // e12a-x11-legs: `--real-window` (passed by the ctest registration on Linux) runs this whole
    // scenario over a REAL X11 window, presenting through the REAL X11 blitter, with the X SERVER
    // as the input source — which is what makes the Linux leg's gesture a genuine one rather than a
    // queue this smoke wrote itself. Parsed AFTER the subprocess re-entry: a CEF child inherits the
    // flag on its command line and must never reach this body.
    const smoke::WindowMode window_mode = smoke::window_mode_from_args(argc, argv);
    std::printf("[editor-cef-smoke-shell-osrdrag] live HTML5 drag through the OSR protocol (%s "
                "window)\n",
                smoke::to_string(window_mode));

    std::error_code ec;
    const std::filesystem::path project =
        std::filesystem::temp_directory_path(ec) / "context-editor-cef-osrdrag-smoke";
    std::filesystem::remove_all(project, ec);
    std::filesystem::create_directories(project, ec);

    shell::WindowDesc desc;
    desc.title = "Context Editor (osr drag smoke)";
    desc.logical_size = render::Extent2D{640, 480};
    smoke::WindowSetup window_setup = smoke::make_smoke_window(desc, window_mode);
    if (window_setup.backend == nullptr)
    {
        std::fprintf(stderr,
                     "[editor-cef-smoke-shell-osrdrag] FAIL: no %s window could be created: %s\n",
                     smoke::to_string(window_mode), window_setup.diagnostic.c_str());
        return finish(1);
    }
    shell::IWindowBackend* backend_raw = window_setup.backend.get();
    const smoke::BrowserGeometry geometry = smoke::browser_geometry(*backend_raw);

    // --- the bridge surfaces editor-core's boot calls --------------------------------------------
    // The same set the boot smoke installs, for the same reason: the router denies unknown methods
    // by DEFAULT, so an uninstalled surface is a refusal — and while boot.ts degrades gracefully on
    // each one, a boot that spends its time on fallbacks is not the boot this smoke wants to drive a
    // gesture through. `handshake` is declared before `bridge` so it OUTLIVES the router holding the
    // handlers that capture it.
    shell::ShellHandshake handshake(shell::make_handshake_nonce());
    shell::BridgeRouter bridge;
    SMOKE_CHECK(handshake.install(bridge), "the bridge handshake installed");

    shell::PanelHost panel_host;
    shell::panels::BuiltinPanels builtin = shell::panels::install_builtin_panels(panel_host);
    SMOKE_CHECK(builtin.bound == shell::panels::hostable_panel_ids().size(),
                "every hostable built-in panel provider bound");
    SMOKE_CHECK(panel_host.install(bridge), "the panel.* bridge surface installed");

    shell::cef::CefShellOptions cef_options;
    cef_options.native_window = nullptr;
    cef_options.logical_size = geometry.logical_size;
    cef_options.dpi = geometry.dpi;
    cef_options.url =
        std::string(shell::kAppEntryUrl) + "?" + shell::kThemePinFlag + "=" + kSmokeThemeId;
    cef_options.app_asset_root = CONTEXT_WEBUI_ASSET_DIR;
    cef_options.bridge = &bridge;
    cef_options.windowless_frame_rate = 10;
    // Isolate the OSCrypt key from the MACHINE keychain (issue #437) — without it macOS blocks
    // CefShutdown() forever on a SecurityAgent prompt no automated run can answer. EVERY CEF smoke
    // sets it and tools/check_cef_keychain_isolation.py fails the build if one stops.
    cef_options.use_mock_keychain = true;
    SMOKE_CHECK(!cef_options.app_asset_root.empty(),
                "CONTEXT_WEBUI_ASSET_DIR was compiled in (the webui asset root is wired)");

    std::string error;
    std::unique_ptr<shell::IBrowserHost> browser =
        shell::cef::make_cef_browser_host(cef_options, error);
    if (browser == nullptr)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-osrdrag] FAIL: the browser did not start: %s\n",
                     error.c_str());
        return finish(1);
    }

    shell::EditorWindowConfig config;
    config.compositor.import_options.force_software = true;
    config.placement_poll_us = 0;
    auto window = std::make_unique<shell::EditorWindow>(std::move(window_setup.backend),
                                                        std::move(browser), config);

    const smoke::PresentSetup present_setup = smoke::attach_smoke_present(*window, window_mode);
    if (!present_setup.ok)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-osrdrag] FAIL: no %s present path: %s\n",
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
    SMOKE_CHECK(editor_state_bridge.install(bridge), "the editor.state.* surface installed");

    shell::KeybindingsBridge keybindings_bridge;
    keybindings_bridge.bind_path(std::filesystem::path{});
    SMOKE_CHECK(keybindings_bridge.install(bridge), "the keybindings.get surface installed");

    shell::ThemesBridge themes_bridge;
    themes_bridge.bind_directory(std::filesystem::path{});
    SMOKE_CHECK(themes_bridge.install(bridge), "the themes.get surface installed");

    shell::WelcomeBridge welcome_bridge;
    welcome_bridge.set_launch_mode(shell::LaunchMode::project);
    welcome_bridge.set_config_path(std::filesystem::path{});
    SMOKE_CHECK(welcome_bridge.install(bridge), "the welcome.state surface installed");

    shell::BannerBridge banner_bridge;
    SMOKE_CHECK(banner_bridge.install(bridge), "the banner surface installed");

    shell::SessionBridge session_bridge;
    SMOKE_CHECK(session_bridge.install(bridge), "the session.state surface installed");

    shell::WindowMoveStore window_move_store;
    shell::WindowBridge window_move_bridge(shell::kPrimaryWindowId, window_move_store);
    SMOKE_CHECK(window_move_bridge.install(bridge), "the window.* surface installed");

    shell::UserConfigStore user_config;
    user_config.bind_path(std::filesystem::path{});
    SMOKE_CHECK(user_config.install(bridge), "the config.* surface installed");

    shell::PackageStoreScan package_scan;
    shell::PackageGrantHost package_grants(package_scan, std::filesystem::path{});
    SMOKE_CHECK(package_grants.install(bridge), "the package.grants.* surface installed");

    // --- 1. boot ---------------------------------------------------------------------------------
    const auto boot_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool presented = false;
    while (std::chrono::steady_clock::now() < boot_deadline)
    {
        if (!manager.pump_once(now_us()))
        {
            break;
        }
        if (editor->compositor().stats().view_frames > 0 &&
            editor->compositor().stats().frames_presented > 0 && handshake.complete())
        {
            presented = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    SMOKE_CHECK(presented, "the editor booted, handshook and presented a real CEF frame within 30s");
    if (!presented)
    {
        manager.shutdown();
        shell::cef::shutdown();
        return finish(1);
    }

    // --- 2. mount the fixture, and PROVE it painted ----------------------------------------------
    editor->browser().execute_script(kFixtureScript);

    const auto fixture_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool fixture_painted = false;
    while (std::chrono::steady_clock::now() < fixture_deadline)
    {
        if (!manager.pump_once(now_us()))
        {
            break;
        }
        const ColourScan scan = scan_colour(editor->compositor().cpu_surface(),
                                            editor->compositor().size(), kOverlayB, kOverlayG,
                                            kOverlayR);
        // HALF the frame, not one texel: the overlay covers the whole client area, so anything less
        // means it did not really mount — and a coincidental magenta texel is exactly the kind of
        // accident a one-texel floor would accept.
        if (scan.scanned > 0 && scan.matched > scan.scanned / 2u)
        {
            fixture_painted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    SMOKE_CHECK(fixture_painted,
                "the injected draggable overlay mounted and its pixels reached the present path — "
                "the anchor that makes every assertion below a statement about the DRAG");
    if (!fixture_painted)
    {
        manager.shutdown();
        shell::cef::shutdown();
        return finish(1);
    }

    // THE NEGATIVE HALF, taken BEFORE the gesture: the drop colour is nowhere in the frame yet. A
    // "green appeared" verdict means nothing without it — the overlay could have been born green.
    {
        const ColourScan before = scan_colour(editor->compositor().cpu_surface(),
                                              editor->compositor().size(), kDroppedB, kDroppedG,
                                              kDroppedR);
        SMOKE_CHECK(before.matched == 0,
                    "the drop colour is ABSENT before the gesture — so its appearance below can "
                    "only be the `drop` event");
    }

    // --- 3-5. drive a real gesture and watch CEF answer -------------------------------------------
    //
    // The press, then moves 60 physical pixels apart. Chromium starts a drag only once the pointer
    // has travelled past its own threshold with the button held, so a single move would prove
    // nothing; the loop keeps stepping until `StartDragging` has fired or the clock runs out.
    const render::Extent2D client = backend_raw->client_size();
    const std::int32_t start_x = static_cast<std::int32_t>(client.width / 4u);
    const std::int32_t start_y = static_cast<std::int32_t>(client.height / 2u);

    SMOKE_CHECK(smoke::inject_event(*backend_raw, window_mode,
                                    drag_sample(shell::PointerAction::down,
                                               shell::PointI{start_x, start_y})),
                "the press was delivered");

    const auto drag_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::int32_t x = start_x;
    // The FIRST move is asserted rather than fire-and-forgotten: `inject_event` returns false for a
    // delivery it could not make (a real-mode build with no X11 headers, a shape the arm refuses),
    // and without this the smoke would spend its whole 20 s injecting nothing and then blame
    // `StartDragging` for a gesture that never left the harness.
    bool first_move_delivered = false;
    while (std::chrono::steady_clock::now() < drag_deadline)
    {
        if (!manager.pump_once(now_us()))
        {
            break;
        }
        if (editor->drag().drags_begun() > 0 && editor->drag().operation() != shell::DragOperation::none)
        {
            break;
        }
        if (x + 60 < static_cast<std::int32_t>(client.width) - 20)
        {
            x += 60;
        }
        else
        {
            // Walk back and forth rather than stopping: `UpdateDragCursor` fires on a CHANGE of the
            // view's answer, so a pointer parked on one spot can leave the loop waiting for an event
            // that has no reason to be sent again.
            x = start_x;
        }
        const bool delivered = smoke::inject_event(
            *backend_raw, window_mode,
            drag_sample(shell::PointerAction::move, shell::PointI{x, start_y}));
        first_move_delivered = first_move_delivered || delivered;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    SMOKE_CHECK(first_move_delivered, "at least one drag move was delivered to the window backend");

    // 4. THE MEMBER THAT USED TO REFUSE. `StartDragging`'s default returns false, which the header
    // defines as "abort the drag operation" — so before b1 this counter could not leave zero no
    // matter what gesture arrived.
    SMOKE_CHECK(editor->drag().drags_begun() == 1,
                "the live renderer's dragstart reached CefRenderHandler::StartDragging and the "
                "Shell ACCEPTED it — the member whose unimplemented default aborted every drag");

    // 5. AND CEF ANSWERED. `UpdateDragCursor` is sent by the browser in response to the
    // `DragTargetDragEnter`/`DragTargetDragOver` the Shell injected; nothing on our side of the
    // process boundary can produce it, so a real operation here is CEF reporting that it processed
    // our injections inside a live document. `copy` specifically, because the fixture's `dragover`
    // sets `dropEffect = 'copy'` — an assertion on the VALUE, not merely on non-emptiness.
    SMOKE_CHECK(editor->drag().operation() == shell::DragOperation::copy,
                "CEF answered our DragTarget* injections with UpdateDragCursor(COPY) — the fixture's "
                "own dropEffect, round-tripped through a live renderer");

    // --- 6. the drop ------------------------------------------------------------------------------
    SMOKE_CHECK(smoke::inject_event(*backend_raw, window_mode,
                                    drag_sample(shell::PointerAction::up,
                                               shell::PointI{x, start_y})),
                "the release was delivered");

    const auto drop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool dropped = false;
    while (std::chrono::steady_clock::now() < drop_deadline)
    {
        if (!manager.pump_once(now_us()))
        {
            break;
        }
        const ColourScan scan = scan_colour(editor->compositor().cpu_surface(),
                                            editor->compositor().size(), kDroppedB, kDroppedG,
                                            kDroppedR);
        if (scan.scanned > 0 && scan.matched > scan.scanned / 2u)
        {
            dropped = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    SMOKE_CHECK(dropped,
                "the overlay repainted GREEN — a real `drop` event was dispatched in the live "
                "document, which only DragTargetDrop after a genuine StartDragging can produce");
    SMOKE_CHECK(editor->drag().end_reason() == shell::OsrDragEndReason::dropped,
                "the Shell recorded the drag as DROPPED");
    // The pair, not either alone: a drag that begins and never ends leaves the renderer's drop
    // targets armed forever, and only the second number can see that.
    SMOKE_CHECK(editor->drag().drags_begun() == editor->drag().drags_ended(),
                "every drag that began also ended — none dangled");
    SMOKE_CHECK(!editor->drag().active(), "no drag is live after the drop");

    // THE FAMILY INVARIANT every smoke that stands up the full boot surface asserts (`-shell`,
    // `-palette`, `-settings`, `-drag`, `-iframe`). The router denies unknown methods BY DEFAULT, so
    // a surface this smoke forgot to install — or one editor-core starts calling and nothing here
    // provides — shows up as a refusal and nowhere else: the boot still "succeeds", the fixture
    // still paints, and the gate stays green while editor-core is being refused. Without this line
    // the newest blocking smoke is quietly narrower than its siblings.
    SMOKE_CHECK(bridge.refused() == 0,
                "window 0's bridge refused nothing across the whole drag scenario");

    std::printf("[editor-cef-smoke-shell-osrdrag] begun=%llu ended=%llu reason=%s refused=%llu\n",
                static_cast<unsigned long long>(editor->drag().drags_begun()),
                static_cast<unsigned long long>(editor->drag().drags_ended()),
                shell::to_string(editor->drag().end_reason()),
                static_cast<unsigned long long>(bridge.refused()));

    // Teardown in the documented order: the manager's windows first, then CEF, while every bridge
    // local is still in scope (cef_shell.h § the LIFETIME INVARIANT / CE #319).
    manager.shutdown();
    shell::cef::shutdown();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-osrdrag] %d assertion(s) FAILED\n",
                     g_failures);
        return finish(1);
    }
    std::printf("[editor-cef-smoke-shell-osrdrag] OK\n");
    return finish(0);
}
