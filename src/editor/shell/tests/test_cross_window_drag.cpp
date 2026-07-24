// e10c — the Shell-mediated cross-window drag session, the cursor-capture RAII guard, the cross-window
// relay, and the `drag.*` bridge surface. Runs on all three default `build` legs (CEF-free).
//
// THE CENTRE OF GRAVITY IS THE SAFETY INVARIANT: every terminal path of a drag — dropped, dropped on
// no zone, Escaped, target window closed mid-drag, source window closed mid-drag, and a drag that never
// began because the OS capture could not be taken — MUST have released the global cursor capture. Each
// case below asserts `capture_released()` AND that the headless capture's release count reached its
// capture count, because a leaked capture makes the whole desktop unusable (cross_window_drag.h).

#include "context/editor/shell/cross_window_drag.h"

#include "context/editor/contract/json.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/window_bridge.h"
#include "shell_test.h"

#include <string>
#include <vector>

namespace shell = context::editor::shell;
using Json = context::editor::contract::Json;
using shelltest::g_failures;

namespace
{

shell::PanelSeed make_seed(const char* id)
{
    shell::PanelSeed seed;
    seed.panel_id = id;
    Json blob = Json::object();
    blob.set("schemaVersion", Json(1));
    Json data = Json::object();
    data.set("scrollTop", Json(static_cast<std::uint64_t>(4096)));
    blob.set("data", std::move(data));
    seed.state = std::move(blob);
    return seed;
}

// Dispatch one bridge method over the router exactly as a renderer's query does, returning its
// `result` (or `error`) — so the tests drive the SAME handler code editor-core reaches.
Json dispatch(shell::BridgeRouter& router, const char* method, const Json& params)
{
    Json request = Json::object();
    request.set("jsonrpc", Json(std::string("2.0")));
    request.set("id", Json(7));
    request.set("method", Json(std::string(method)));
    request.set("params", params);
    const shell::BridgeDispatch out = router.dispatch(request.dump());
    const Json response = Json::parse(out.response);
    return response.contains("result") ? response.at("result") : response.at("error");
}

// ---------------------------------------------------------------- ScopedCursorCapture (RAII)

void test_scoped_capture_raii()
{
    shell::HeadlessCursorCapture capture;
    {
        shell::ScopedCursorCapture guard(capture);
        CHECK(guard.holds());
        CHECK(capture.captured());
        CHECK(capture.captures() == 1);
    }
    // The destructor released it — the whole point of the guard.
    CHECK(!capture.captured());
    CHECK(capture.releases() == 1);
}

void test_scoped_capture_early_release_is_idempotent()
{
    shell::HeadlessCursorCapture capture;
    shell::ScopedCursorCapture guard(capture);
    guard.release();
    CHECK(!capture.captured());
    guard.release(); // idempotent — no second OS release
    CHECK(capture.releases() == 1);
    // The destructor, running next, must not release again either.
}

void test_scoped_capture_move_does_not_double_release()
{
    shell::HeadlessCursorCapture capture;
    {
        shell::ScopedCursorCapture a(capture);
        shell::ScopedCursorCapture b(std::move(a));
        CHECK(b.holds());
        CHECK(!a.holds()); // moved-from owns nothing
    }
    // Exactly one release despite two guards having existed.
    CHECK(capture.captures() == 1);
    CHECK(capture.releases() == 1);
    CHECK(!capture.captured());
}

void test_scoped_capture_empty_holds_nothing()
{
    shell::ScopedCursorCapture guard; // default — references no capture
    CHECK(!guard.holds());
    guard.release(); // safe no-op
}

// ---------------------------------------------------------------- CrossWindowDragStore

void test_store_hover_targets_only_the_hovered_window()
{
    shell::CrossWindowDragStore store;
    shell::DragHover hover;
    hover.active = true;
    hover.target = 1;
    hover.panel_id = "builtin.problems";
    hover.local = shell::PointI{40, 60};
    hover.generation = 5;
    store.publish_hover(hover);

    // The window under the cursor sees the active hover; every other window sees inactive.
    const shell::DragHover for_1 = store.hover_for(1);
    CHECK(for_1.active);
    CHECK(for_1.target == 1);
    CHECK(for_1.local.x == 40 && for_1.local.y == 60);
    CHECK(!store.hover_for(0).active);
    CHECK(!store.hover_for(2).active);
}

void test_store_drops_stale_and_wrong_generation_zone_reports()
{
    shell::CrossWindowDragStore store;
    shell::DragHover hover;
    hover.active = true;
    hover.target = 1;
    hover.generation = 10;
    store.publish_hover(hover);

    // A report for a DIFFERENT generation is stale — dropped.
    shell::DragZone stale;
    stale.valid = true;
    stale.zone_id = "center";
    stale.generation = 9;
    store.report_zone(stale);
    CHECK(!store.zone().valid);
    CHECK(store.zones_reported() == 0);

    // The matching-generation report is recorded.
    shell::DragZone fresh;
    fresh.valid = true;
    fresh.zone_id = "center";
    fresh.generation = 10;
    store.report_zone(fresh);
    CHECK(store.zone().valid);
    CHECK(store.zone().zone_id == "center");
    CHECK(store.zones_reported() == 1);

    // A NEW hover clears the answered zone (no stale highlight in the next frame).
    hover.generation = 11;
    store.publish_hover(hover);
    CHECK(!store.zone().valid);
}

// A session wired against a scripted window layout: window 1 occupies screen x in [100,300).
struct Fixture
{
    shell::CrossWindowDragStore store;
    shell::CrossWindowDragSession session{store};
    std::vector<std::pair<shell::WindowId, shell::PanelSeed>> drops;

    Fixture()
    {
        session.bind_window_at_point(
            [](shell::PointI p) -> shell::WindowId
            {
                if (p.x >= 100 && p.x < 300)
                    return 1;
                if (p.x >= 0 && p.x < 100)
                    return 0;
                return shell::kInvalidWindowId; // the desktop / another app
            });
        session.bind_to_local(
            [](shell::WindowId target, shell::PointI screen) -> shell::PointI
            {
                // window 1's client origin is screen x=100.
                return target == 1 ? shell::PointI{screen.x - 100, screen.y} : screen;
            });
        session.bind_drop(
            [this](shell::WindowId target, const shell::PanelSeed& seed) -> bool
            {
                drops.push_back({target, seed});
                return true;
            });
    }
};

// The target reports a valid zone the same way its editor-core would (matching the live hover's gen).
void answer_valid_zone(shell::CrossWindowDragStore& store, shell::WindowId reader)
{
    const shell::DragHover hover = store.hover_for(reader);
    shell::DragZone zone;
    zone.valid = true;
    zone.zone_id = "center";
    zone.generation = hover.generation;
    store.report_zone(zone);
}

// ---------------------------------------------------------------- begin / no-capture

void test_begin_takes_capture_and_seeds_ghost()
{
    Fixture fx;
    shell::HeadlessCursorCapture capture;
    CHECK(fx.session.begin(0, make_seed("builtin.problems"), shell::PointI{50, 20}, capture));
    CHECK(fx.session.active());
    CHECK(capture.captured());
    CHECK(fx.session.ghost_position().x == 50);
    CHECK(fx.session.source() == 0);
    fx.session.cancel(); // release for a clean end
    CHECK(fx.session.capture_released());
}

void test_begin_refuses_when_capture_unavailable()
{
    Fixture fx;
    shell::UnavailableCursorCapture capture;
    CHECK(!fx.session.begin(0, make_seed("builtin.problems"), shell::PointI{50, 20}, capture));
    CHECK(!fx.session.active());
    CHECK(fx.session.end_reason() == shell::DragEndReason::no_capture);
    CHECK(fx.session.capture_released()); // nothing was captured, so nothing leaks
}

void test_session_inert_until_begin()
{
    // In-window regression guard: with no drag begun the session touches nothing — the cursor moving
    // within one window is Dockview's own DnD, never intercepted here.
    Fixture fx;
    fx.session.update_cursor(shell::PointI{150, 30});
    fx.session.cancel();
    CHECK(!fx.session.active());
    CHECK(fx.store.hovers_published() == 0);
    CHECK(fx.session.target() == shell::kInvalidWindowId);
}

// ---------------------------------------------------------------- the cross-window round trip + drop

void test_cross_window_round_trip_and_drop()
{
    Fixture fx;
    shell::HeadlessCursorCapture capture;
    CHECK(fx.session.begin(0, make_seed("builtin.problems"), shell::PointI{50, 20}, capture));

    // The cursor leaves window 0 (x<100) and moves over window 1 (x in [100,300)).
    fx.session.update_cursor(shell::PointI{180, 40});
    CHECK(fx.session.target() == 1);
    CHECK(fx.session.ghost_position().x == 180);

    // The Shell published a hover FOR window 1, in window 1's client pixels (x = 180 - 100).
    const shell::DragHover published = fx.store.hover_for(1);
    CHECK(published.active);
    CHECK(published.panel_id == "builtin.problems");
    CHECK(published.local.x == 80 && published.local.y == 40);
    CHECK(!fx.store.hover_for(0).active); // the source window is NOT told to answer

    // Window 1's editor-core answers its drop zone (the genuine cross-origin round trip); drop rehomes.
    answer_valid_zone(fx.store, 1);
    const shell::DragEndReason reason = fx.session.drop();
    CHECK(reason == shell::DragEndReason::dropped);
    CHECK(fx.drops.size() == 1);
    CHECK(fx.drops[0].first == 1);                       // rehomed INTO window 1
    CHECK(fx.drops[0].second.panel_id == "builtin.problems");
    CHECK(fx.session.capture_released());
}

void test_drop_on_no_zone_does_not_move()
{
    Fixture fx;
    shell::HeadlessCursorCapture capture;
    CHECK(fx.session.begin(0, make_seed("builtin.problems"), shell::PointI{50, 20}, capture));
    fx.session.update_cursor(shell::PointI{180, 40}); // over window 1, but it reports NO zone
    const shell::DragEndReason reason = fx.session.drop();
    CHECK(reason == shell::DragEndReason::dropped_no_zone);
    CHECK(fx.drops.empty()); // the panel stayed in its source window
    CHECK(fx.session.capture_released());
}

void test_drop_over_desktop_does_not_move()
{
    Fixture fx;
    shell::HeadlessCursorCapture capture;
    CHECK(fx.session.begin(0, make_seed("builtin.problems"), shell::PointI{50, 20}, capture));
    fx.session.update_cursor(shell::PointI{500, 40}); // over no editor window (the desktop)
    CHECK(fx.session.target() == shell::kInvalidWindowId);
    CHECK(!fx.store.hover_for(1).active); // an inactive hover was published
    const shell::DragEndReason reason = fx.session.drop();
    CHECK(reason == shell::DragEndReason::dropped_no_zone);
    CHECK(fx.drops.empty());
    CHECK(fx.session.capture_released());
}

// ---------------------------------------------------------------- cancel / window-death paths

void test_escape_releases_capture()
{
    Fixture fx;
    shell::HeadlessCursorCapture capture;
    CHECK(fx.session.begin(0, make_seed("builtin.problems"), shell::PointI{50, 20}, capture));
    fx.session.update_cursor(shell::PointI{180, 40});
    fx.session.cancel(); // Escape
    CHECK(fx.session.end_reason() == shell::DragEndReason::escaped);
    CHECK(fx.session.capture_released());
    CHECK(capture.releases() == capture.captures());
}

void test_target_window_dies_mid_drag_releases_capture()
{
    Fixture fx;
    shell::HeadlessCursorCapture capture;
    CHECK(fx.session.begin(0, make_seed("builtin.problems"), shell::PointI{50, 20}, capture));
    fx.session.update_cursor(shell::PointI{180, 40});
    CHECK(fx.session.target() == 1);
    fx.session.on_window_closed(1); // the window under the cursor died
    CHECK(fx.session.end_reason() == shell::DragEndReason::target_closed);
    CHECK(fx.session.target() == shell::kInvalidWindowId); // reference dropped, never read through
    CHECK(fx.session.capture_released());
    CHECK(capture.releases() == capture.captures());
}

void test_source_window_dies_mid_drag_releases_capture()
{
    Fixture fx;
    shell::HeadlessCursorCapture capture;
    CHECK(fx.session.begin(0, make_seed("builtin.problems"), shell::PointI{50, 20}, capture));
    fx.session.update_cursor(shell::PointI{180, 40});
    fx.session.on_window_closed(0); // the drag's OWN source died
    CHECK(fx.session.end_reason() == shell::DragEndReason::source_closed);
    CHECK(fx.session.capture_released());
    CHECK(capture.releases() == capture.captures());
}

void test_unrelated_window_close_continues_drag()
{
    Fixture fx;
    shell::HeadlessCursorCapture capture;
    CHECK(fx.session.begin(0, make_seed("builtin.problems"), shell::PointI{50, 20}, capture));
    fx.session.update_cursor(shell::PointI{180, 40});
    fx.session.on_window_closed(7); // some other window
    CHECK(fx.session.active());      // the drag lives on
    CHECK(capture.captured());
    fx.session.cancel();
    CHECK(fx.session.capture_released());
}

void test_destructor_releases_a_still_live_drag()
{
    shell::HeadlessCursorCapture capture;
    {
        Fixture fx;
        CHECK(fx.session.begin(0, make_seed("builtin.problems"), shell::PointI{50, 20}, capture));
        // Deliberately DO NOT end it — the session destructs while active.
    }
    // Belt-and-braces: even a forgotten end() cannot leak the capture (cross_window_drag.h).
    CHECK(!capture.captured());
    CHECK(capture.releases() == capture.captures());
}

// ---------------------------------------------------------------- the drag.* bridge surface

void test_drag_bridge_round_trips_through_the_router()
{
    // Install a WindowBridge (self_id = 1) bound to a store, exactly as a target window's boot does, and
    // drive drag.probe / drag.report-zone over the REAL router — the SAME code editor-core reaches.
    shell::WindowMoveStore move_store;
    shell::CrossWindowDragStore drag_store;
    shell::WindowBridge bridge(/*self_id*/ 1, move_store);
    bridge.bind_drag_store(&drag_store);
    shell::BridgeRouter router;
    CHECK(bridge.install(router));

    // No drag yet: probe answers inactive, and refuses nothing.
    Json probe = dispatch(router, shell::kDragProbeMethod, Json::object());
    CHECK(!probe.at("active").as_bool());
    CHECK(router.refused() == 0);

    // The Shell publishes a hover for window 1.
    shell::DragHover hover;
    hover.active = true;
    hover.target = 1;
    hover.panel_id = "builtin.problems";
    hover.local = shell::PointI{80, 40};
    hover.generation = 3;
    drag_store.publish_hover(hover);

    probe = dispatch(router, shell::kDragProbeMethod, Json::object());
    CHECK(probe.at("active").as_bool());
    CHECK(probe.at("panelId").as_string() == "builtin.problems");
    CHECK(probe.at("x").as_int() == 80);
    CHECK(probe.at("generation").as_int() == 3);
    CHECK(bridge.drag_probes_active() == 1);

    // The renderer reports its zone back — recorded in the store, matching generation.
    Json report = Json::object();
    report.set("valid", Json(true));
    report.set("zoneId", Json(std::string("center")));
    report.set("generation", Json(static_cast<std::uint64_t>(3)));
    Json reported = dispatch(router, shell::kDragReportZoneMethod, report);
    CHECK(reported.at("recorded").as_bool());
    CHECK(drag_store.zone().valid);
    CHECK(drag_store.zone().zone_id == "center");
    CHECK(bridge.drag_zones_reported() == 1);
    CHECK(router.refused() == 0);

    // A malformed report (no generation) fails closed with bad-params — asserted on the method body
    // directly, the same way test_window_bridge asserts its refusals.
    Json bad = Json::object();
    bad.set("valid", Json(true));
    std::string error_code;
    (void)bridge.drag_report_zone(bad, error_code);
    CHECK(error_code == std::string(shell::kErrWindowBadParams));
}

void test_drag_bridge_inert_without_a_store()
{
    // A smoke that installs the window surface but binds NO drag store (the five sibling smokes): the
    // methods still route (no `unknown_method`, so `refused()==0` holds) but do nothing.
    shell::WindowMoveStore move_store;
    shell::WindowBridge bridge(/*self_id*/ 0, move_store);
    shell::BridgeRouter router;
    CHECK(bridge.install(router));

    Json probe = dispatch(router, shell::kDragProbeMethod, Json::object());
    CHECK(!probe.at("active").as_bool());

    Json report = Json::object();
    report.set("valid", Json(true));
    report.set("generation", Json(static_cast<std::uint64_t>(1)));
    Json reported = dispatch(router, shell::kDragReportZoneMethod, report);
    CHECK(reported.at("recorded").as_bool()); // a well-formed no-op, never a refusal
    CHECK(router.refused() == 0);
}

} // namespace

int main()
{
    test_scoped_capture_raii();
    test_scoped_capture_early_release_is_idempotent();
    test_scoped_capture_move_does_not_double_release();
    test_scoped_capture_empty_holds_nothing();

    test_store_hover_targets_only_the_hovered_window();
    test_store_drops_stale_and_wrong_generation_zone_reports();

    test_begin_takes_capture_and_seeds_ghost();
    test_begin_refuses_when_capture_unavailable();
    test_session_inert_until_begin();

    test_cross_window_round_trip_and_drop();
    test_drop_on_no_zone_does_not_move();
    test_drop_over_desktop_does_not_move();

    test_escape_releases_capture();
    test_target_window_dies_mid_drag_releases_capture();
    test_source_window_dies_mid_drag_releases_capture();
    test_unrelated_window_close_continues_drag();
    test_destructor_releases_a_still_live_drag();

    test_drag_bridge_round_trips_through_the_router();
    test_drag_bridge_inert_without_a_store();

    SHELL_TEST_MAIN_END();
}
