// The owner loop (03 §1) and the D10 authenticated attach: the pump end to end — resize, DPI change,
// focus, the input round-trip through arbitration into the browser, the popup, placement
// persistence, and teardown.

#include "context/editor/shell/shell.h"

#include "context/editor/shell/chrome_facts.h" // a1: the maximized-flip fact the manager reports
#include "context/editor/shell/ui_mirror.h"

#include "shell_test.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace context::editor::shell;
namespace render = context::render;
namespace present = context::render::present;
// `using namespace ...::shell` does not bring the SIBLING namespace into scope, and shell.h's
// declarations name client:: from inside it.
namespace client = context::editor::client;
namespace fs = std::filesystem;

namespace
{

// A window over the headless backend + the scripted browser, presenting into a MemoryBlitter — the
// honest offscreen shell (see window.h / present_blit.h), and the same wiring the Session-0-safe
// smoke uses.
struct Harness
{
    HeadlessWindowBackend* backend = nullptr;
    ScriptedBrowserHost* browser = nullptr;
    present::MemoryBlitter* blitter = nullptr;
    std::unique_ptr<EditorWindow> window;

    explicit Harness(render::Extent2D logical = render::Extent2D{800, 600},
                     std::uint64_t placement_poll_us = 0)
    {
        WindowDesc desc;
        desc.logical_size = logical;
        auto backend_owned = std::make_unique<HeadlessWindowBackend>(desc);
        auto browser_owned = std::make_unique<ScriptedBrowserHost>();
        backend = backend_owned.get();
        browser = browser_owned.get();

        EditorWindowConfig config;
        config.compositor.import_options.force_software = true;
        // 0 (the default) polls every pump so most tests need no clock advance; a case about the
        // poll INTERVAL itself (the a1 chrome-fact boot seed) passes the real one.
        config.placement_poll_us = placement_poll_us;
        window = std::make_unique<EditorWindow>(std::move(backend_owned), std::move(browser_owned),
                                                config);

        auto blitter_owned = std::make_unique<present::MemoryBlitter>();
        blitter = blitter_owned.get();
        window->compositor().attach_cpu(std::move(blitter_owned), backend->client_size());
    }

    void queue_view_frame(render::Extent2D coded, std::uint8_t b, std::uint8_t g, std::uint8_t r)
    {
        browser->queue_solid_frame(BrowserLayer::view, coded,
                                   render::Rect2D{render::Origin2D{}, coded}, b, g, r, 255);
    }
};

// ------------------------------------------------------------------- the D10 authenticated attach

void test_attach_guard_refuses_an_unauthenticated_attach()
{
    // Token enforcement has been on since e02 and the Shell has NO unauthenticated path. Checking
    // here rather than letting the daemon refuse turns "there is no token on this machine" into its
    // own message instead of an `attach.denied` that reads like a wrong password.
    const client::AttachOptions options = make_shell_attach_options();
    std::string reason;
    CHECK(!guard_shell_attach(options, "", reason));
    CHECK(!reason.empty());
    CHECK(shelltest::mentions(reason, "token"));
    CHECK(shelltest::mentions(reason, "e02"));

    // A token DISCOVERED from .editor/instance.json is the normal path: Client::attach falls back
    // to it, so an empty options.token is correct rather than a bug.
    CHECK(guard_shell_attach(options, "discovered-token", reason));
    CHECK(reason.empty());

    // An explicitly pinned token also passes, with nothing discovered.
    const client::AttachOptions pinned = make_shell_attach_options("explicit-token");
    CHECK(guard_shell_attach(pinned, "", reason));
}

void test_shell_attach_options_ask_for_the_shell_scope()
{
    const client::AttachOptions options = make_shell_attach_options();
    // Named once (kShellScope) so a caller cannot quietly widen them.
    CHECK(options.scope == std::string(kShellScope));
    CHECK(options.scope == std::string("read,write,session"));
    CHECK(options.token.empty()); // discovery fills it
    CHECK(!options.capabilities.empty());
}

void test_attach_to_a_project_with_no_daemon_is_reported_not_fatal()
{
    const fs::path root = shelltest::make_temp_project("context-shell-loop", "nodaemon");
    // No .editor/instance.json: discovery finds nothing. The editor opens read-only (03 §7) rather
    // than refusing to start — a shell that would not start without a daemon could not be used to
    // diagnose why the daemon would not start.
    const DaemonAttach attach = attach_to_project(root, 50);
    CHECK(!attach.attached);
    CHECK(attach.client == nullptr);
    CHECK(!attach.error.empty());
    shelltest::cleanup(root);
}

// ---------------------------------------------------------------------------- the owner loop

void test_pump_syncs_the_browser_size_in_dip_on_the_first_iteration()
{
    Harness harness(render::Extent2D{800, 600});
    // A 2x monitor: the browser's view rect is DIP, so reporting the physical size would lay the
    // document out at twice the intended size.
    ShellEvent dpi;
    dpi.kind = ShellEventKind::dpi_changed;
    dpi.dpi = DpiScale{192};
    harness.backend->post(dpi);
    ShellEvent resize;
    resize.kind = ShellEventKind::resize;
    resize.size = render::Extent2D{1600, 1200}; // physical
    harness.backend->post(resize);

    CHECK(harness.window->pump_once(1000));
    CHECK(shelltest::extent_eq(harness.browser->last_logical_size(), render::Extent2D{800, 600}));
    CHECK(harness.browser->last_dpi().dpi == 192u);
    // The resize protocol drives BOTH halves: the compositor reconfigures AND the browser is told
    // (WasResized). Doing only the first leaves the browser painting at the old size.
    CHECK(shelltest::extent_eq(harness.window->compositor().size(), render::Extent2D{1600, 1200}));
    CHECK(harness.browser->resize_count() >= 2);
}

// a1: the OTHER half of the OSR geometry contract. `resize()` tells the browser how big its view is;
// this tells it WHERE that view is, which an off-screen browser cannot ask the OS for — and without
// it CEF answers `GetScreenPoint` with view coordinates and every native menu opens at the wrong
// place (docs/shell.md § 16, owner item #5).
void test_pump_pushes_the_client_origin_and_a_move_updates_it()
{
    // THE REAL 250 ms POLL INTERVAL, deliberately — not the tests' usual 0. With the poll never due
    // inside this test's timeline, every push here is ATTRIBUTABLE to the event path: a version that
    // pushed only from the placement poll (the backstop, exercised by the next test) would leave the
    // browser mis-positioned for up to a quarter second after a window drag, and this test is what
    // says so rather than passing on the poll's coattails.
    Harness harness(render::Extent2D{800, 600}, 250'000);
    // Model the frameless Win32 client: it is INSET from the window rect
    // (`win32_frameless_client_insets` — 12 px at 150%, 8 px at 100%), so the window origin and the
    // client origin are different points, and only the client one is correct here.
    harness.backend->set_client_inset(PointI{12, 0});

    // The first pump seeds it, alongside the view size — a browser is positioned before it paints.
    CHECK(harness.window->pump_once(1000));
    CHECK(harness.browser->last_client_origin() == (PointI{12, 0}));
    CHECK(harness.browser->client_origin_pushes() == 1);

    // A move, 1 ms later: the client origin travels with the window, on the EVENT.
    ShellEvent moved;
    moved.kind = ShellEventKind::moved;
    moved.position = PointI{1000, 500};
    harness.backend->post(moved);
    CHECK(harness.window->pump_once(2000));
    CHECK(harness.browser->last_client_origin() == (PointI{1012, 500}));
    // The WINDOW rect origin is a different point, and pushing it would put every menu 12 px off.
    CHECK(harness.browser->last_client_origin() != (PointI{1000, 500}));
    CHECK(harness.browser->client_origin_pushes() == 2);

    // An IDLE pump pushes NOTHING. The negative half: a per-iteration re-push would be invisible in
    // the value (it never changes) while driving a CEF callback on every frame.
    CHECK(harness.window->pump_once(3000));
    CHECK(harness.browser->client_origin_pushes() == 2);
    CHECK(harness.browser->last_client_origin() == (PointI{1012, 500}));

    // A DPI change re-pushes even with the window where it is: the mapping is scale-dependent on
    // Windows/Linux, so a stale origin after a monitor change is a stale menu position.
    ShellEvent dpi;
    dpi.kind = ShellEventKind::dpi_changed;
    dpi.dpi = DpiScale{144};
    harness.backend->post(dpi);
    CHECK(harness.window->pump_once(4000));
    CHECK(harness.browser->client_origin_pushes() == 3);
}

void test_a_placement_change_with_no_move_event_still_repositions_the_browser()
{
    // THE BACKSTOP, and it is not redundant: a window can move without the Shell seeing a `moved`
    // event — a WM-driven move, a maximize/restore, or simply a backend that reports geometry by
    // polling rather than by event (the Cocoa one does exactly that, window.h § the five Cocoa
    // shapes). The placement poll already re-reads the backend for persistence; the origin rides
    // the same observation, re-read from the backend rather than derived from the placement — which
    // is the one case a derivation would get wrong (a maximized window's persisted rect is its
    // RESTORE rect, not where it is).
    Harness harness(render::Extent2D{800, 600}); // poll every pump
    harness.backend->set_client_inset(PointI{12, 0});
    CHECK(harness.window->pump_once(1000));
    CHECK(harness.browser->client_origin_pushes() == 1);

    // Move the window behind the pump's back: no ShellEvent is queued, so only the poll can notice.
    harness.backend->apply_placement(WindowPlacement{"", 640, 360, 800, 600, false});
    CHECK(harness.window->pump_once(2000));
    CHECK(harness.browser->last_client_origin() == (PointI{652, 360}));
    CHECK(harness.browser->client_origin_pushes() == 2);

    // And the poll does not re-push a placement that did not change.
    CHECK(harness.window->pump_once(3000));
    CHECK(harness.browser->client_origin_pushes() == 2);
}

void test_input_round_trip_reaches_the_browser()
{
    Harness harness;
    // No regions published: everything is browser chrome.
    ShellEvent move;
    move.kind = ShellEventKind::pointer;
    move.pointer.action = PointerAction::move;
    move.pointer.position = PointI{120, 90};
    harness.backend->post(move);

    ShellEvent down = move;
    down.pointer.action = PointerAction::down;
    down.pointer.button = MouseButton::left;
    harness.backend->post(down);

    ShellEvent up = down;
    up.pointer.action = PointerAction::up;
    harness.backend->post(up);

    ShellEvent wheel;
    wheel.kind = ShellEventKind::pointer;
    wheel.pointer.action = PointerAction::wheel;
    wheel.pointer.wheel_delta_y = -120;
    harness.backend->post(wheel);

    ShellEvent key;
    key.kind = ShellEventKind::key;
    key.key.action = KeyAction::raw_key_down;
    key.key.windows_key_code = 'A';
    harness.backend->post(key);

    ShellEvent character;
    character.kind = ShellEventKind::key;
    character.key.action = KeyAction::character;
    character.key.character = U'A';
    harness.backend->post(character);

    CHECK(harness.window->pump_once(1000));
    // Mouse + wheel + keyboard all round-tripped.
    CHECK(harness.browser->pointers().size() == 4u);
    CHECK(harness.browser->pointers()[3].wheel_delta_y == -120);
    CHECK(harness.browser->keys().size() == 2u);
    CHECK(harness.window->input().pointer_dispatches() == 4);
    CHECK(harness.window->input().key_dispatches() == 2);
}

void test_a_viewport_region_takes_input_away_from_the_browser()
{
    Harness harness;
    harness.window->input().regions().publish(
        {ShellRegion{"scene", shelltest::rect(0, 0, 400, 300), RegionKind::viewport}});

    ShellEvent in_viewport;
    in_viewport.kind = ShellEventKind::pointer;
    in_viewport.pointer.action = PointerAction::move;
    in_viewport.pointer.position = PointI{50, 50};
    harness.backend->post(in_viewport);

    ShellEvent in_chrome = in_viewport;
    in_chrome.pointer.position = PointI{500, 400};
    harness.backend->post(in_chrome);

    CHECK(harness.window->pump_once(1000));
    // Only the chrome sample reached the browser; the viewport one took the native path (whose
    // consumer — camera/picking/gizmos over the bridge — arrives with e11).
    CHECK(harness.browser->pointers().size() == 1u);
    CHECK(harness.browser->pointers()[0].position == (PointI{500, 400}));
    CHECK(harness.window->input().pointer_dispatches() == 2);
}

void test_caption_samples_are_suppressed_and_control_samples_reach_the_browser()
{
    // b1 (ROADMAP risk 3): a caption sample must never half-reach the browser — on Windows the NC
    // hit-test consumes it before client routing, and this arbitration IS the suppression on every
    // backend without an NC path (this headless one included). The CONTROL rects are the opposite:
    // web-drawn buttons, so their samples MUST reach the browser.
    Harness harness;
    harness.window->input().regions().publish(
        {ShellRegion{"chrome.caption", shelltest::rect(0, 0, 700, 38), RegionKind::caption},
         ShellRegion{"chrome.caption-close", shelltest::rect(700, 0, 46, 38),
                     RegionKind::caption_close}});

    ShellEvent caption_press;
    caption_press.kind = ShellEventKind::pointer;
    caption_press.pointer.action = PointerAction::down;
    caption_press.pointer.button = MouseButton::left;
    caption_press.pointer.position = PointI{300, 20};
    harness.backend->post(caption_press);
    ShellEvent caption_release = caption_press;
    caption_release.pointer.action = PointerAction::up;
    harness.backend->post(caption_release);

    ShellEvent close_press = caption_press;
    close_press.pointer.position = PointI{720, 20};
    harness.backend->post(close_press);
    ShellEvent close_release = close_press;
    close_release.pointer.action = PointerAction::up;
    harness.backend->post(close_release);

    CHECK(harness.window->pump_once(1000));
    // All four samples were arbitrated; only the CONTROL pair reached the browser.
    CHECK(harness.window->input().pointer_dispatches() == 4);
    CHECK(harness.browser->pointers().size() == 2u);
    CHECK(harness.browser->pointers()[0].position == (PointI{720, 20}));
    CHECK(harness.browser->pointers()[1].position == (PointI{720, 20}));
}

void test_pump_pushes_republished_chrome_regions_down_to_the_backend()
{
    // b1: the published region map reaches the OS backend — the Windows NC hit-test's feed — on
    // the pump after a publish. Generation-gated and wholesale (shell.cpp § the chrome-region
    // push), observable on every leg through the headless backend's recorder.
    Harness harness;
    CHECK(harness.window->pump_once(1));
    CHECK(harness.backend->chrome_region_pushes() == 0); // nothing published yet: no push

    harness.window->input().regions().publish(
        {ShellRegion{"chrome.caption", shelltest::rect(0, 0, 700, 38), RegionKind::caption}});
    CHECK(harness.window->pump_once(2));
    CHECK(harness.backend->chrome_region_pushes() == 1);
    CHECK(harness.backend->chrome_regions().size() == 1u);
    CHECK(harness.backend->chrome_regions().front().id == "chrome.caption");

    // No republish, no push — the one-integer generation gate.
    CHECK(harness.window->pump_once(3));
    CHECK(harness.backend->chrome_region_pushes() == 1);

    // A wholesale EMPTY republish is pushed too: "no drag duty any more" is a fact the backend
    // must also see, or a stale caption rect keeps hit-testing after a mode change.
    harness.window->input().regions().publish({});
    CHECK(harness.window->pump_once(4));
    CHECK(harness.backend->chrome_region_pushes() == 2);
    CHECK(harness.backend->chrome_regions().empty());
}

void test_focus_events_reach_the_browser_and_drop_a_live_drag()
{
    Harness harness;
    // a1: `focused()` starts false — the honest answer for a window the OS has said nothing about
    // (chrome.state's `focused` reads this).
    CHECK(!harness.window->focused());
    harness.window->input().regions().publish(
        {ShellRegion{"scene", shelltest::rect(0, 0, 400, 300), RegionKind::viewport}});

    ShellEvent down;
    down.kind = ShellEventKind::pointer;
    down.pointer.action = PointerAction::down;
    down.pointer.button = MouseButton::left;
    down.pointer.position = PointI{50, 50};
    harness.backend->post(down);
    CHECK(harness.window->pump_once(1000));
    CHECK(harness.window->input().has_pointer_capture());

    ShellEvent focus_lost;
    focus_lost.kind = ShellEventKind::focus_lost;
    harness.backend->post(focus_lost);
    CHECK(harness.window->pump_once(2000));
    CHECK(!harness.browser->focused());
    CHECK(!harness.window->focused()); // a1: the chrome.state source tracks the same event
    // The pointer-up that would have released the drag is going to a DIFFERENT window now, so the
    // capture is dropped — otherwise the next click here still routes to where the drag started.
    CHECK(!harness.window->input().has_pointer_capture());

    ShellEvent focus_gained;
    focus_gained.kind = ShellEventKind::focus_gained;
    harness.backend->post(focus_gained);
    CHECK(harness.window->pump_once(3000));
    CHECK(harness.browser->focused());
    CHECK(harness.window->focused());
}

void test_a_browser_paint_presents_and_an_idle_pump_does_not()
{
    Harness harness;
    // The first pump presents (a freshly attached window has never drawn).
    harness.queue_view_frame(render::Extent2D{200, 150}, 9, 8, 7);
    CHECK(harness.window->pump_once(1000));
    CHECK(harness.blitter->blit_count() == 1);

    // Idle: damage-driven redraw skips the frame entirely.
    CHECK(harness.window->pump_once(2000));
    CHECK(harness.blitter->blit_count() == 1);
    CHECK(harness.window->compositor().stats().frames_skipped_no_damage >= 1);

    // A new paint damages it again.
    harness.queue_view_frame(render::Extent2D{200, 150}, 1, 2, 3);
    CHECK(harness.window->pump_once(3000));
    CHECK(harness.blitter->blit_count() == 2);
}

void test_a_popup_composites_through_the_loop()
{
    Harness harness;
    harness.queue_view_frame(render::Extent2D{200, 150}, 0, 0, 0);
    // The rect arrives before the first popup paint — the real CEF sequence.
    harness.browser->queue_popup_state(true, shelltest::rect(20, 20, 40, 30));
    harness.browser->queue_solid_frame(BrowserLayer::popup, render::Extent2D{40, 30},
                                       shelltest::rect(0, 0, 40, 30), 200, 150, 100, 255);
    CHECK(harness.window->pump_once(1000));
    CHECK(harness.window->compositor().popup_visible());
    CHECK(harness.window->compositor().stats().popup_draws == 1);

    // The composed surface is WINDOW-sized (compositor.h § the 1:1 rule), so its row stride is the
    // window's width — not the 200-wide view frame's, which lands 1:1 at the origin inside it.
    const std::vector<std::uint8_t>& surface = harness.window->compositor().cpu_surface();
    const std::size_t stride = static_cast<std::size_t>(harness.window->compositor().size().width);
    CHECK(surface.size() == stride * harness.window->compositor().size().height * 4u);
    const std::size_t inside = (static_cast<std::size_t>(20) * stride + 20) * 4;
    CHECK(surface[inside + 0] == 200);
    CHECK(surface[inside + 1] == 150);
    CHECK(surface[inside + 2] == 100);
}

void test_close_ends_the_loop()
{
    Harness harness;
    ShellEvent close;
    close.kind = ShellEventKind::close_requested;
    harness.backend->post(close);
    CHECK(!harness.window->pump_once(1000));
    CHECK(!harness.window->alive());
    // Pumping a dead window is a no-op, not a crash.
    CHECK(!harness.window->pump_once(2000));
}

// ---------------------------------------------------------------------------- the WindowManager

void test_manager_persists_placement_and_restores_it()
{
    const fs::path root = shelltest::make_temp_project("context-shell-loop", "placement");
    {
        WindowManager manager(root);
        Harness harness;
        HeadlessWindowBackend* backend = harness.backend;
        EditorWindow& window = manager.add(std::move(harness.window));

        // Move the window: the manager records it, debounced.
        backend->apply_placement(WindowPlacement{"\\\\.\\DISPLAY1", 300, 200, 900, 700, false});
        CHECK(manager.pump_once(1'000));
        CHECK(manager.state_store().dirty());
        CHECK(manager.state_store().write_count() == 0); // still inside the quiet period

        CHECK(manager.pump_once(1'000'000)); // past the debounce
        CHECK(manager.state_store().write_count() == 1);
        CHECK(&window == manager.window(0));

        manager.shutdown();
        CHECK(manager.window_count() == 0u);
    }

    // A NEW manager restores it — placement round-trips through .editor/editor-state.json, the file
    // the Shell is the single writer of (03 §1).
    {
        WindowManager manager(root);
        CHECK(manager.state_store().state().windows.size() == 1u);
        CHECK(manager.state_store().state().windows[0].x == 300);
        CHECK(manager.state_store().state().windows[0].width == 900u);

        Harness harness;
        HeadlessWindowBackend* backend = harness.backend;
        manager.add(std::move(harness.window));
        // add() applies the remembered placement before the first frame.
        CHECK(backend->placement().x == 300);
        CHECK(shelltest::extent_eq(backend->client_size(), render::Extent2D{900, 700}));
    }

    shelltest::cleanup(root);
}

void test_manager_drops_a_closed_window_and_ends_when_none_are_left()
{
    const fs::path root = shelltest::make_temp_project("context-shell-loop", "drop");
    WindowManager manager(root);
    Harness harness;
    HeadlessWindowBackend* backend = harness.backend;
    manager.add(std::move(harness.window));
    CHECK(manager.window_count() == 1u);

    ShellEvent close;
    close.kind = ShellEventKind::close_requested;
    backend->post(close);
    // The last window closing is the loop's termination condition.
    CHECK(!manager.pump_once(1000));
    CHECK(manager.window_count() == 0u);
    shelltest::cleanup(root);
}

void test_a_maximized_flip_reaches_the_mirror_relay_and_an_idle_pump_does_not()
{
    // The a1 DoD line, end to end (target design 02 §1): a placement FLIP the 250 ms poll observes
    // becomes ONE `editor.ui.chrome` envelope in the affected window's mirror queue — through the
    // REAL detector (poll_placement -> WindowManager::pump_once) and the REAL relay
    // (ChromeFactRelay -> UiMirrorStore), exactly as editor_main wires them. The headless backend's
    // `set_maximized` is the honest state lever (window.h), standing in for Win+Up / the WM.
    const fs::path root = shelltest::make_temp_project("context-shell-loop", "chromefact");
    WindowManager manager(root);
    UiMirrorStore mirror;
    ChromeFactRelay relay;
    relay.bind_store(&mirror);
    manager.on_chrome_maximized([&relay](WindowId id, bool maximized)
                                { (void)relay.publish_maximized(id, maximized); });

    Harness harness;
    HeadlessWindowBackend* backend = harness.backend;
    manager.add(std::move(harness.window));
    const WindowId id = manager.last_minted_id();

    // NEGATIVE HALF FIRST: pumps with no flip publish NOTHING — adoption seeded the baseline, so
    // boot is quiet and an unchanged state stays quiet. (Without this half, a relay that spammed a
    // fact per pump would pass every positive assertion below.)
    CHECK(manager.pump_once(1'000));
    CHECK(manager.pump_once(2'000));
    CHECK(relay.published() == 0);
    CHECK(mirror.pending(id) == 0);

    // THE FLIP: the OS-truth bit changes; the NEXT poll observes it; exactly one fact arrives, in
    // THIS window's queue, carrying the new state.
    backend->set_maximized(true);
    CHECK(manager.pump_once(3'000));
    CHECK(relay.published() == 1);
    CHECK(mirror.pending(id) == 1);
    {
        const std::vector<context::editor::contract::Json> facts = mirror.take(id);
        CHECK(facts.size() == 1);
        CHECK(facts[0].at("topic").as_string() == "editor.ui.chrome");
        CHECK(facts[0].at("origin").as_string() == "shell");
        CHECK(facts[0].at("payload").at("windowId").as_int() ==
              static_cast<std::int64_t>(id));
        CHECK(facts[0].at("payload").at("maximized").as_bool());
    }

    // Steady state is quiet again — the sink reports CHANGES, not the state per pump.
    CHECK(manager.pump_once(4'000));
    CHECK(relay.published() == 1);

    // And the RESTORE is its own fact, `maximized:false`.
    backend->set_maximized(false);
    CHECK(manager.pump_once(5'000));
    CHECK(relay.published() == 2);
    {
        const std::vector<context::editor::contract::Json> facts = mirror.take(id);
        CHECK(facts.size() == 1);
        CHECK(facts[0].at("payload").at("maximized").as_bool() == false);
    }

    manager.shutdown();
    shelltest::cleanup(root);
}

void test_a_window_restored_maximized_fires_no_boot_fact()
{
    // The adoption baseline is seeded from the backend's REAL placement (shell.cpp add_session), so
    // a window that comes back maximized from `.editor/editor-state.json` reports no flip at boot —
    // `chrome.state` carries the initial state; the fact channel carries CHANGES only. Without the
    // seed, every restored-maximized boot would open with a spurious `maximized:true` fact.
    const fs::path root = shelltest::make_temp_project("context-shell-loop", "chromeseed");
    WindowManager manager(root);
    UiMirrorStore mirror;
    ChromeFactRelay relay;
    relay.bind_store(&mirror);
    manager.on_chrome_maximized([&relay](WindowId id, bool maximized)
                                { (void)relay.publish_maximized(id, maximized); });

    // The REAL poll interval, deliberately (not the Harness's poll-every-pump 0): the hazard this
    // test exists for is the window between adoption and the first poll, where the EditorWindow's
    // cached placement predates the restore — with interval 0 the cache re-syncs on the very first
    // pump and a baseline seeded from the wrong source could never be caught.
    Harness harness(render::Extent2D{800, 600}, 250'000);
    // Maximized BEFORE adoption — the restored-placement shape (add() applies a remembered
    // placement before the first frame; here the backend simply already carries the state).
    harness.backend->set_maximized(true);
    manager.add(std::move(harness.window));

    // Pumps INSIDE the first poll interval, then one past it: quiet throughout — no fact minted
    // from the adoption itself, neither before nor at the first real poll.
    CHECK(manager.pump_once(1'000));
    CHECK(manager.pump_once(2'000));
    CHECK(relay.published() == 0);
    CHECK(manager.pump_once(251'000)); // the first real poll runs here
    CHECK(relay.published() == 0);

    // The un-maximize IS a change, and still fires exactly once — the baseline was the true state,
    // not a default false that would have eaten this flip as "no change".
    harness.backend->set_maximized(false);
    CHECK(manager.pump_once(502'000)); // past the next poll deadline, so the flip is observed
    CHECK(relay.published() == 1);
    CHECK(mirror.take(manager.last_minted_id()).at(0).at("payload").at("maximized").as_bool() ==
          false);

    manager.shutdown();
    shelltest::cleanup(root);
}

void test_shutdown_flushes_pending_state_and_is_idempotent()
{
    const fs::path root = shelltest::make_temp_project("context-shell-loop", "shutdown");
    WindowManager manager(root);
    Harness harness;
    HeadlessWindowBackend* backend = harness.backend;
    manager.add(std::move(harness.window));
    backend->apply_placement(WindowPlacement{"", 11, 22, 640, 480, false});
    CHECK(manager.pump_once(1000));
    CHECK(manager.state_store().dirty());

    // Waiting out the quiet period on the way down would just lose the last change the user made.
    manager.shutdown();
    CHECK(manager.state_store().write_count() == 1);
    CHECK(fs::exists(editor_state_path(root)));
    manager.shutdown(); // idempotent
    CHECK(manager.state_store().write_count() == 1);
    shelltest::cleanup(root);
}

} // namespace

// ------------------------------------------------------------------------------- PumpSchedule
//
// The integrated pump's policy (03 §1) — the design's central rejection of the spike's
// multi-threaded+mutex model. It lives in the portable core precisely so it can be asserted here:
// its real caller is the CEF binding, the one translation unit the local gate cannot build, where
// nothing would have exercised it. A fake clock, so nothing here is wall-clock dependent.
void test_pump_schedule_runs_when_work_is_due()
{
    PumpSchedule schedule;

    // Nothing scheduled yet: the FLOOR pumps anyway. This is what keeps the browser live if CEF's
    // schedule callback is never delivered — without it a missed schedule parks the browser forever.
    CHECK(!schedule.has_scheduled_work());
    CHECK(schedule.should_pump(1'000));

    // Scheduled and NOT yet due: skip. (Pumping regardless would make the schedule meaningless and
    // burn the owner thread on every loop iteration.)
    schedule.schedule(/*delay_ms*/ 50, /*now_ms*/ 1'000);
    CHECK(schedule.has_scheduled_work());
    CHECK(schedule.due_ms() == 1'050);
    CHECK(!schedule.should_pump(1'000));
    CHECK(!schedule.should_pump(1'049));

    // Due: pump, and CONSUME the schedule — so the next call falls through to the floor rather than
    // re-firing the same deadline forever.
    CHECK(schedule.should_pump(1'050));
    CHECK(!schedule.has_scheduled_work());
    CHECK(schedule.should_pump(1'051));

    // Exactly-due and past-due both fire; a later schedule replaces the earlier deadline.
    schedule.schedule(10, 2'000);
    CHECK(schedule.should_pump(9'999));
    CHECK(!schedule.has_scheduled_work());

    // A negative delay means "as soon as possible", not a deadline in the past that never arrives.
    schedule.schedule(-5, 3'000);
    CHECK(schedule.due_ms() == 3'000);
    CHECK(schedule.should_pump(3'000));

    // Re-scheduling while one is pending moves the deadline rather than stacking.
    schedule.schedule(100, 4'000);
    schedule.schedule(10, 4'000);
    CHECK(schedule.due_ms() == 4'010);
    CHECK(!schedule.should_pump(4'005));
    CHECK(schedule.should_pump(4'010));
}

int main()
{
    test_pump_schedule_runs_when_work_is_due();
    test_attach_guard_refuses_an_unauthenticated_attach();
    test_shell_attach_options_ask_for_the_shell_scope();
    test_attach_to_a_project_with_no_daemon_is_reported_not_fatal();
    test_pump_syncs_the_browser_size_in_dip_on_the_first_iteration();
    test_pump_pushes_the_client_origin_and_a_move_updates_it();
    test_a_placement_change_with_no_move_event_still_repositions_the_browser();
    test_input_round_trip_reaches_the_browser();
    test_a_viewport_region_takes_input_away_from_the_browser();
    test_caption_samples_are_suppressed_and_control_samples_reach_the_browser();
    test_pump_pushes_republished_chrome_regions_down_to_the_backend();
    test_focus_events_reach_the_browser_and_drop_a_live_drag();
    test_a_browser_paint_presents_and_an_idle_pump_does_not();
    test_a_popup_composites_through_the_loop();
    test_close_ends_the_loop();
    test_manager_persists_placement_and_restores_it();
    test_manager_drops_a_closed_window_and_ends_when_none_are_left();
    test_a_maximized_flip_reaches_the_mirror_relay_and_an_idle_pump_does_not();
    test_a_window_restored_maximized_fires_no_boot_fact();
    test_shutdown_flushes_pending_state_and_is_idempotent();
    SHELL_TEST_MAIN_END();
}
