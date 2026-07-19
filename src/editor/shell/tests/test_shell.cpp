// The owner loop (03 §1) and the D10 authenticated attach: the pump end to end — resize, DPI change,
// focus, the input round-trip through arbitration into the browser, the popup, placement
// persistence, and teardown.

#include "context/editor/shell/shell.h"

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

fs::path make_temp_project(const char* tag)
{
    static int counter = 0;
    std::error_code ec;
    fs::path root = fs::temp_directory_path(ec) /
                    ("context-shell-loop-" + std::string(tag) + "-" + std::to_string(++counter));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

// Named cleanup(), not remove_all(): the latter is found by ADL on fs::path and collides with
// std::filesystem::remove_all.
void cleanup(const fs::path& path)
{
    std::error_code ec;
    fs::remove_all(path, ec);
}

// A window over the headless backend + the scripted browser, presenting into a MemoryBlitter — the
// honest offscreen shell (see window.h / present_blit.h), and the same wiring the Session-0-safe
// smoke uses.
struct Harness
{
    HeadlessWindowBackend* backend = nullptr;
    ScriptedBrowserHost* browser = nullptr;
    present::MemoryBlitter* blitter = nullptr;
    std::unique_ptr<EditorWindow> window;

    explicit Harness(render::Extent2D logical = render::Extent2D{800, 600})
    {
        WindowDesc desc;
        desc.logical_size = logical;
        auto backend_owned = std::make_unique<HeadlessWindowBackend>(desc);
        auto browser_owned = std::make_unique<ScriptedBrowserHost>();
        backend = backend_owned.get();
        browser = browser_owned.get();

        EditorWindowConfig config;
        config.compositor.import_options.force_software = true;
        config.placement_poll_us = 0; // poll every pump so the tests need no clock advance
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
    const fs::path root = make_temp_project("nodaemon");
    // No .editor/instance.json: discovery finds nothing. The editor opens read-only (03 §7) rather
    // than refusing to start — a shell that would not start without a daemon could not be used to
    // diagnose why the daemon would not start.
    const DaemonAttach attach = attach_to_project(root, 50);
    CHECK(!attach.attached);
    CHECK(attach.client == nullptr);
    CHECK(!attach.error.empty());
    cleanup(root);
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

void test_focus_events_reach_the_browser_and_drop_a_live_drag()
{
    Harness harness;
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
    // The pointer-up that would have released the drag is going to a DIFFERENT window now, so the
    // capture is dropped — otherwise the next click here still routes to where the drag started.
    CHECK(!harness.window->input().has_pointer_capture());

    ShellEvent focus_gained;
    focus_gained.kind = ShellEventKind::focus_gained;
    harness.backend->post(focus_gained);
    CHECK(harness.window->pump_once(3000));
    CHECK(harness.browser->focused());
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

    const std::vector<std::uint8_t>& surface = harness.window->compositor().cpu_surface();
    const std::size_t inside = (static_cast<std::size_t>(20) * 200 + 20) * 4;
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
    const fs::path root = make_temp_project("placement");
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

    cleanup(root);
}

void test_manager_drops_a_closed_window_and_ends_when_none_are_left()
{
    const fs::path root = make_temp_project("drop");
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
    cleanup(root);
}

void test_shutdown_flushes_pending_state_and_is_idempotent()
{
    const fs::path root = make_temp_project("shutdown");
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
    cleanup(root);
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
    test_input_round_trip_reaches_the_browser();
    test_a_viewport_region_takes_input_away_from_the_browser();
    test_focus_events_reach_the_browser_and_drop_a_live_drag();
    test_a_browser_paint_presents_and_an_idle_pump_does_not();
    test_a_popup_composites_through_the_loop();
    test_close_ends_the_loop();
    test_manager_persists_placement_and_restores_it();
    test_manager_drops_a_closed_window_and_ends_when_none_are_left();
    test_shutdown_flushes_pending_state_and_is_idempotent();
    SHELL_TEST_MAIN_END();
}
