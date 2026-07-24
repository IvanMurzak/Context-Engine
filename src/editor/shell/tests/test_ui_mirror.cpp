// T1 for the cross-window `editor.ui` MIRROR relay (M9 e10d, design 05 §5, D7 tier 2) — the C++ half
// of INHERITED DRILL 2.
//
// WHAT THIS PROVES, AND WHY A SINGLE-WINDOW BUILD COULD NOT. e08c built the editor-core mirror seam
// (`UiMirrorSink` / `receiveMirrored`) and unit-tested the envelope, but wired NO transport and — its
// refine found — exercised only the point-to-point loop breaker; the loop breaker a BROADCASTING
// transport needs (drop an envelope that returns to its own `origin`) stayed dark. This file drives
// the REAL Shell transport (`UiMirrorStore` + the `ui.mirror` / `ui.mirror-poll` bridge methods) with
// TWO window bridges over ONE store and proves the property that makes that branch fire: a publish
// from window A is broadcast to EVERY window INCLUDING A, so A gets its own envelope back (the frame
// its bus drops by `origin`) AND window B gets it (the frame B applies). A one-window build has no B
// to converge and no second origin to distinguish — the drill is meaningless there, which is exactly
// why e08c deferred it to a real second window. The receiving-bus DROP itself is the TS half
// (`uimirror_broadcast.test.ts`, driving the real `EditorUiBus.receiveMirrored` through this shape).

#include "context/editor/shell/window_bridge.h"

#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/ui_mirror.h"
#include "context/editor/shell/window_registry.h"

#include "shell_test.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace context::editor::shell;
using Json = context::editor::contract::Json;

namespace
{

// One `editor.ui` envelope as editor-core hands it to `ui.mirror` ({seq, topic, origin, payload}).
Json envelope(std::uint64_t seq, const char* topic, const char* origin, Json payload)
{
    Json e = Json::object();
    e.set("seq", Json(seq));
    e.set("topic", Json(std::string(topic)));
    e.set("origin", Json(std::string(origin)));
    e.set("payload", std::move(payload));
    return e;
}

Json theme_payload(const char* variant)
{
    Json p = Json::object();
    p.set("variant", Json(std::string(variant)));
    return p;
}

// Bind a bridge to report the two-window set {0, 1} — what `window.list` reports to each window.
void bind_two_windows(WindowBridge& bridge)
{
    bridge.bind_windows([]() -> std::vector<WindowId> { return {0u, 1u}; });
}

// --------------------------------------------------------- 1. the broadcast reaches EVERY window

void test_a_publish_broadcasts_to_every_window_including_the_sender()
{
    WindowMoveStore moves0;
    WindowMoveStore moves1;
    UiMirrorStore mirror;

    WindowBridge window0(0u, moves0);
    WindowBridge window1(1u, moves1);
    bind_two_windows(window0);
    bind_two_windows(window1);
    window0.bind_ui_mirror_store(&mirror);
    window1.bind_ui_mirror_store(&mirror);

    // Window A (origin "0") publishes a theme-changed fact.
    std::string error;
    const Json result = window0.ui_mirror(
        envelope(7, "editor.ui.theme-changed", "0", theme_payload("dark")), error);
    CHECK(error.empty());
    CHECK(result.at("mirrored").as_bool());
    // Broadcast to BOTH windows — the sender included. That "self included" is the whole point: it is
    // what delivers A its own envelope back so its bus can drop it (the branch e08c could not reach).
    CHECK(result.at("windows").as_int() == 2);
    CHECK(window0.ui_mirrors_published() == 1u);

    // Window B (origin "1") drains it — the fact it will APPLY (foreign origin).
    const Json b_batch = window1.ui_mirror_poll();
    CHECK(b_batch.at("events").size() == 1u);
    CHECK(b_batch.at("events").at(0).at("topic").as_string() == "editor.ui.theme-changed");
    CHECK(b_batch.at("events").at(0).at("origin").as_string() == "0");
    // The payload is carried VERBATIM — the Shell never interprets tier-2 chrome (D7).
    CHECK(b_batch.at("events").at(0).at("payload").at("variant").as_string() == "dark");
    CHECK(window1.ui_mirrors_delivered() == 1u);

    // Window A (the SENDER) ALSO drains its OWN envelope back — the echo its bus drops by `origin`.
    // Without this delivery the broadcasting echo-suppression branch would never fire.
    const Json a_batch = window0.ui_mirror_poll();
    CHECK(a_batch.at("events").size() == 1u);
    CHECK(a_batch.at("events").at(0).at("origin").as_string() == "0");
    CHECK(window0.ui_mirrors_delivered() == 1u);

    // A second poll on either window is empty — the queue was drained, not merely read.
    CHECK(window1.ui_mirror_poll().at("events").size() == 0u);
    CHECK(window0.ui_mirror_poll().at("events").size() == 0u);
}

// --------------------------------------------------------- 2. ordering + multi-origin interleave

void test_multiple_publishes_are_drained_in_publish_order()
{
    WindowMoveStore moves0;
    WindowMoveStore moves1;
    UiMirrorStore mirror;
    WindowBridge window0(0u, moves0);
    WindowBridge window1(1u, moves1);
    bind_two_windows(window0);
    bind_two_windows(window1);
    window0.bind_ui_mirror_store(&mirror);
    window1.bind_ui_mirror_store(&mirror);

    std::string error;
    // A publishes twice, then B publishes once — three broadcasts, all seen by both windows in order.
    (void)window0.ui_mirror(envelope(1, "editor.ui.focus", "0", Json::object()), error);
    (void)window0.ui_mirror(envelope(2, "editor.ui.layout", "0", Json::object()), error);
    (void)window1.ui_mirror(envelope(1, "editor.ui.palette", "1", Json::object()), error);

    const Json b_batch = window1.ui_mirror_poll();
    CHECK(b_batch.at("events").size() == 3u);
    CHECK(b_batch.at("events").at(0).at("topic").as_string() == "editor.ui.focus");
    CHECK(b_batch.at("events").at(1).at("topic").as_string() == "editor.ui.layout");
    CHECK(b_batch.at("events").at(2).at("topic").as_string() == "editor.ui.palette");
    // The last envelope carries window B's OWN origin — the one B's bus will drop.
    CHECK(b_batch.at("events").at(2).at("origin").as_string() == "1");
}

// --------------------------------------------------------- 3. fail-closed + inert-without-a-store

void test_a_malformed_envelope_fails_closed_and_a_null_store_is_inert()
{
    WindowMoveStore moves;
    UiMirrorStore mirror;
    WindowBridge window(0u, moves);
    window.bind_windows([]() -> std::vector<WindowId> { return {0u}; });
    window.bind_ui_mirror_store(&mirror);

    // An envelope with no topic / no origin is a wiring bug — refused, nothing broadcast.
    std::string error;
    Json bad = Json::object();
    bad.set("payload", Json::object());
    (void)window.ui_mirror(bad, error);
    CHECK(error == kErrWindowBadParams);
    CHECK(window.ui_mirrors_published() == 0u);
    CHECK(mirror.pending(0u) == 0u);

    // A bridge with NO mirror store bound (the five sibling smokes) routes both methods as a
    // well-formed no-op — never `unknown_method`, so those smokes keep `bridge.refused() == 0`.
    WindowMoveStore moves2;
    WindowBridge inert(0u, moves2);
    inert.bind_windows([]() -> std::vector<WindowId> { return {0u}; });
    std::string inert_error;
    const Json r = inert.ui_mirror(envelope(1, "editor.ui.focus", "0", Json::object()), inert_error);
    CHECK(inert_error.empty());
    CHECK(!r.at("mirrored").as_bool());
    CHECK(inert.ui_mirror_poll().at("events").size() == 0u);
}

// --------------------------------------------------------- 4. forget drops a destroyed window's queue

void test_forget_drops_a_destroyed_windows_queue()
{
    UiMirrorStore mirror;
    mirror.enqueue(1u, Json::object());
    mirror.enqueue(1u, Json::object());
    CHECK(mirror.pending(1u) == 2u);
    // A window that goes away mid-app must not leak its queued envelopes until exit.
    mirror.forget(1u);
    CHECK(mirror.pending(1u) == 0u);
    CHECK(mirror.take(1u).empty());
}

// --------------------------------------------------------- 5. the methods bind over a real router

Json dispatch(BridgeRouter& router, const char* method, const Json& params, bool& refused)
{
    Json request = Json::object();
    request.set("jsonrpc", Json("2.0"));
    request.set("id", Json(11));
    request.set("method", Json(std::string(method)));
    request.set("params", params);
    const BridgeDispatch out = router.dispatch(request.dump());
    refused = out.refused();
    return Json::parse(out.response);
}

void test_both_methods_bind_over_a_real_router()
{
    WindowMoveStore moves;
    UiMirrorStore mirror;
    WindowBridge window(0u, moves);
    window.bind_windows([]() -> std::vector<WindowId> { return {0u}; });
    window.bind_ui_mirror_store(&mirror);

    BridgeRouter router;
    // install() binds EVERY window.* + drag.* + ui.* method; a false return is a name collision the
    // caller must not ignore. This is the property that keeps a live smoke's `bridge.refused() == 0`:
    // the methods exist on the router, so editor-core's boot-time calls are routed, not refused.
    CHECK(window.install(router));

    bool refused = true;
    const Json mirrored = dispatch(router, kUiMirrorMethod,
                                   envelope(1, "editor.ui.theme-changed", "0", theme_payload("light")),
                                   refused);
    CHECK(!refused);
    CHECK(mirrored.at("result").at("mirrored").as_bool());

    refused = true;
    const Json polled = dispatch(router, kUiMirrorPollMethod, Json::object(), refused);
    CHECK(!refused);
    CHECK(polled.at("result").at("events").size() == 1u);
}

// -------------------------------------------------- 6. the convergence report (e10d-drill2)

// The one fact the transport counters CANNOT tell apart: `ui.mirror-poll` delivers a window its OWN
// echo too, so a delivered count sees an applied fact and a dropped echo alike. `ui.mirror-report`
// carries the receiving BUS's own APPLIED / SUPPRESSED verdict, which is what the live smoke asserts on
// to prove convergence (window B applied) vs echo suppression (window A dropped). Here: it records the
// running totals last-write-wins, fails closed on a malformed report, and binds over the real router.
Json report_params(std::int64_t applied, std::int64_t suppressed)
{
    Json p = Json::object();
    p.set("origin", Json(std::string("0")));
    p.set("applied", Json(applied));
    p.set("suppressed", Json(suppressed));
    return p;
}

void test_the_convergence_report_records_running_totals_and_fails_closed()
{
    WindowMoveStore moves;
    WindowBridge window(1u, moves);
    window.bind_windows([]() -> std::vector<WindowId> { return {0u, 1u}; });

    // A fresh bridge has reported nothing — the sibling smokes' resting state.
    CHECK(window.ui_mirror_reports() == 0u);
    CHECK(window.ui_mirror_reported_applied() == 0u);
    CHECK(window.ui_mirror_reported_suppressed() == 0u);

    // A well-formed report is recorded and answered.
    std::string error;
    const Json r1 = window.ui_mirror_report(report_params(1, 0), error);
    CHECK(error.empty());
    CHECK(r1.at("recorded").as_bool());
    CHECK(window.ui_mirror_reports() == 1u);
    CHECK(window.ui_mirror_reported_applied() == 1u);
    CHECK(window.ui_mirror_reported_suppressed() == 0u);

    // Last-write-wins: the renderer sends CUMULATIVE totals, so a later report replaces (never adds to)
    // the recorded counts — the value the smoke waits to reach.
    (void)window.ui_mirror_report(report_params(3, 2), error);
    CHECK(error.empty());
    CHECK(window.ui_mirror_reports() == 2u);
    CHECK(window.ui_mirror_reported_applied() == 3u);
    CHECK(window.ui_mirror_reported_suppressed() == 2u);

    // Fail-closed: a missing count, a non-number, or a NEGATIVE total is a wiring bug — refused, and
    // the last good record is left untouched (never a meaningless count the smoke would assert against).
    Json missing = Json::object();
    missing.set("applied", Json(std::int64_t{1}));
    std::string err_missing;
    (void)window.ui_mirror_report(missing, err_missing);
    CHECK(err_missing == kErrWindowBadParams);

    std::string err_negative;
    (void)window.ui_mirror_report(report_params(-1, 0), err_negative);
    CHECK(err_negative == kErrWindowBadParams);
    // The refusals recorded nothing: the totals are still the last GOOD report.
    CHECK(window.ui_mirror_reports() == 2u);
    CHECK(window.ui_mirror_reported_applied() == 3u);
    CHECK(window.ui_mirror_reported_suppressed() == 2u);
}

void test_the_report_binds_over_a_real_router()
{
    WindowMoveStore moves;
    WindowBridge window(0u, moves);
    window.bind_windows([]() -> std::vector<WindowId> { return {0u}; });

    BridgeRouter router;
    // install() binds ui.mirror-report alongside every sibling method, so editor-core's boot-time
    // convergence report is ROUTED (never `unknown_method`) on every window — the property that keeps a
    // live smoke's `bridge.refused() == 0` even when NO mirror store is bound (the report needs none).
    CHECK(window.install(router));

    bool refused = true;
    const Json recorded = dispatch(router, kUiMirrorReportMethod, report_params(2, 1), refused);
    CHECK(!refused);
    CHECK(recorded.at("result").at("recorded").as_bool());
    CHECK(window.ui_mirror_reported_applied() == 2u);
    CHECK(window.ui_mirror_reported_suppressed() == 1u);

    // A malformed report is a router-level error, not a silent record.
    refused = true;
    Json bad = Json::object();
    bad.set("applied", Json(std::string("many"))); // not a number
    const Json err = dispatch(router, kUiMirrorReportMethod, bad, refused);
    CHECK(err.at("error").at("data").at("reason").as_string() == kErrWindowBadParams);
}

} // namespace

int main()
{
    test_a_publish_broadcasts_to_every_window_including_the_sender();
    test_multiple_publishes_are_drained_in_publish_order();
    test_a_malformed_envelope_fails_closed_and_a_null_store_is_inert();
    test_forget_drops_a_destroyed_windows_queue();
    test_both_methods_bind_over_a_real_router();
    test_the_convergence_report_records_running_totals_and_fails_closed();
    test_the_report_binds_over_a_real_router();
    SHELL_TEST_MAIN_END();
}
