// T1 for the window-management bridge surface (M9 e10b): the cross-window relay store, the parse +
// loud-fail contract of each `window.*` method, and the full JSON-RPC binding over a real
// BridgeRouter.
//
// WHAT THIS PROVES AND WHY IT MATTERS. `window.*` is the ONLY path by which editor-core asks for a
// tear-out / a move / a rehome (the browser side has no window registry of its own — 04 §1 / 08 §1),
// and the ONE mechanism (D6) that must serve all three. The properties pinned here are the ones
// whose failure is INVISIBLE at runtime and would only surface as a live-CEF-smoke red one CI round
// away:
//
//   * a tear-out that FAILS answers LOUD — `created:false` + the `WindowCreateOutcome` token + a
//     reason — never a silent success (03 §7). A silent fallback that "works" is a DoD failure.
//   * the RELAY is the same store for tear-out (a boot seed the new window reads once) and for
//     move-to-N / rehome (a queue the target drains on its poll), so the SAME serialize->relay->
//     recreate path serves every move — the divergence D6 exists to prevent stays impossible.
//   * every method fails CLOSED on renderer-controlled garbage (a missing panelId, a non-numeric
//     target) rather than seeding a nameless panel or addressing a wrong window.
//   * the D6 state blob is OPAQUE — copied verbatim, never interpreted — so a value a fresh panel
//     could not have (a typed input, a scroll offset) survives the relay byte-for-byte.

#include "context/editor/shell/window_bridge.h"

#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/window_registry.h"

#include "shell_test.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace context::editor::shell;
using Json = context::editor::contract::Json;

namespace
{

// A D6 state blob a FRESH panel could not have — a typed-in value plus a scroll offset — so a relay
// that dropped the state would be caught by the survivor check, not merely by "a panel came back".
Json fresh_impossible_state()
{
    Json data = Json::object();
    data.set("query", Json(std::string("half-typed search")));
    data.set("scrollTop", Json(4096));
    Json blob = Json::object();
    blob.set("schemaVersion", Json(1));
    blob.set("data", std::move(data));
    return blob;
}

Json dispatch(BridgeRouter& router, const char* method, const Json& params, bool& refused)
{
    Json request = Json::object();
    request.set("jsonrpc", Json("2.0"));
    request.set("id", Json(11));
    request.set("method", Json(std::string(method)));
    request.set("params", params);
    const BridgeDispatch out = router.dispatch(request.dump());
    refused = out.refused();
    const Json response = Json::parse(out.response);
    return response.contains("result") ? response.at("result") : response.at("error");
}

// --- the relay store -----------------------------------------------------------------------------

void the_store_relays_a_boot_seed_once()
{
    WindowMoveStore store;
    CHECK(!store.has_boot_seed(1));
    store.set_boot_seed(1, PanelSeed{"builtin.problems", fresh_impossible_state()});
    CHECK(store.has_boot_seed(1));
    CHECK(store.pending_boot_seeds() == 1);

    const auto taken = store.take_boot_seed(1);
    CHECK(taken.has_value());
    CHECK(taken->panel_id == "builtin.problems");
    // The OPAQUE blob survived the relay byte-for-byte — the value a fresh panel could not have.
    CHECK(taken->state.at("data").at("query").as_string() == "half-typed search");
    CHECK(taken->state.at("data").at("scrollTop").as_int() == 4096);
    // Consumed exactly once: a window is seeded before it boots and reads its seed a single time.
    CHECK(!store.has_boot_seed(1));
    CHECK(!store.take_boot_seed(1).has_value());
}

void the_store_queues_rehomes_in_order()
{
    WindowMoveStore store;
    store.enqueue_rehome(0, PanelSeed{"a", Json{}});
    store.enqueue_rehome(0, PanelSeed{"b", Json{}});
    CHECK(store.pending_rehomes(0) == 2);
    // A move to a DIFFERENT target is isolated — a rehome to window 0 never leaks into window 2.
    store.enqueue_rehome(2, PanelSeed{"c", Json{}});
    CHECK(store.pending_rehomes(2) == 1);

    const std::vector<PanelSeed> drained = store.take_rehomed(0);
    CHECK(drained.size() == 2);
    CHECK(drained[0].panel_id == "a");
    CHECK(drained[1].panel_id == "b");
    CHECK(store.pending_rehomes(0) == 0);
    CHECK(store.take_rehomed(0).empty()); // drained once, then empty

    store.forget(2); // a window going away for good drops its still-queued seeds
    CHECK(store.pending_rehomes(2) == 0);
}

// --- tear-out: the LOUD degradation contract (03 §7) ---------------------------------------------

void a_tear_out_creates_seeds_and_reports_the_new_window()
{
    WindowMoveStore store;
    WindowBridge bridge(kPrimaryWindowId, store);
    // The handler is the app's create-a-window step; here it mints id 1 and stashes the seed exactly
    // as editor_main.cpp's binding does, so the new window's `window.seed` will find it.
    bridge.bind_tear_out(
        [&store](const WindowBridge::TearOut& request) -> WindowMoveResult
        {
            store.set_boot_seed(1, request.seed);
            return WindowMoveResult{true, 1, to_string(WindowCreateOutcome::created), ""};
        });

    Json params = Json::object();
    params.set("panelId", Json(std::string("builtin.inspector")));
    params.set("state", fresh_impossible_state());
    std::string error_code;
    const Json out = bridge.tear_out(params, error_code);
    CHECK(error_code.empty());
    CHECK(out.at("created").as_bool());
    CHECK(out.at("windowId").as_int() == 1);
    CHECK(bridge.tear_outs() == 1);
    // The seed reached the store keyed by the NEW window, opaque state intact.
    CHECK(store.has_boot_seed(1));
    CHECK(store.take_boot_seed(1)->state.at("data").at("scrollTop").as_int() == 4096);
}

void a_failed_tear_out_is_loud_not_silent()
{
    WindowMoveStore store;
    WindowBridge bridge(kPrimaryWindowId, store);
    // The factory failed (no native backend on this platform) — the exact 03 §7 case editor-core must
    // degrade LOUDLY on, to a floating group in the SOURCE window.
    bridge.bind_tear_out(
        [](const WindowBridge::TearOut&) -> WindowMoveResult {
            return WindowMoveResult{false, kInvalidWindowId,
                                    to_string(WindowCreateOutcome::factory_failed),
                                    "no native window backend on this platform"};
        });

    Json params = Json::object();
    params.set("panelId", Json(std::string("builtin.problems")));
    std::string error_code;
    const Json out = bridge.tear_out(params, error_code);
    CHECK(error_code.empty()); // the CALL was well-formed; the FAILURE rides the result, not an error
    CHECK(out.at("created").as_bool() == false);
    CHECK(out.at("outcome").as_string() == std::string(to_string(WindowCreateOutcome::factory_failed)));
    CHECK(!out.at("error").as_string().empty()); // a reason the user can be shown
    // Nothing was seeded — there is no window to seed.
    CHECK(store.pending_boot_seeds() == 0);
}

void no_bound_handler_is_still_a_loud_answer()
{
    WindowMoveStore store;
    WindowBridge bridge(kPrimaryWindowId, store); // no bind_tear_out
    Json params = Json::object();
    params.set("panelId", Json(std::string("builtin.problems")));
    std::string error_code;
    const Json out = bridge.tear_out(params, error_code);
    CHECK(error_code.empty());
    CHECK(out.at("created").as_bool() == false);
    CHECK(out.at("outcome").as_string() == std::string(to_string(WindowCreateOutcome::no_factory)));
}

void a_tear_out_with_no_panel_id_fails_closed()
{
    WindowMoveStore store;
    WindowBridge bridge(kPrimaryWindowId, store);
    Json params = Json::object(); // no panelId
    std::string error_code;
    const Json out = bridge.tear_out(params, error_code);
    CHECK(error_code == std::string(kErrWindowBadParams));
    CHECK(out.is_null());
    CHECK(bridge.tear_outs() == 0); // a malformed request never counts as a tear-out
}

// --- move-to / seed / rehomed: the same relay, the target's two read moments ----------------------

void a_move_to_enqueues_for_the_target_window()
{
    WindowMoveStore store;
    WindowBridge bridge(2, store); // this request comes FROM window 2
    bridge.bind_move_to(
        [&store](const WindowBridge::MoveTo& request) -> WindowMoveResult
        {
            store.enqueue_rehome(request.target, request.seed);
            return WindowMoveResult{true, request.target, "", ""};
        });

    Json params = Json::object();
    params.set("panelId", Json(std::string("builtin.problems")));
    params.set("state", fresh_impossible_state());
    params.set("windowId", Json(0)); // move it to the primary
    std::string error_code;
    const Json out = bridge.move_to(params, error_code);
    CHECK(error_code.empty());
    CHECK(out.at("moved").as_bool());
    CHECK(bridge.moves() == 1);
    // The primary's rehome queue now carries the panel, delivered on its next poll.
    CHECK(store.pending_rehomes(0) == 1);

    // The target reads it exactly as window 0's editor-core does: `window.rehomed` drains the queue.
    WindowBridge primary_side(0, store);
    const Json rehomed = primary_side.rehomed();
    CHECK(rehomed.at("panels").size() == 1);
    CHECK(rehomed.at("panels").at(0).at("panelId").as_string() == "builtin.problems");
    CHECK(rehomed.at("panels").at(0).at("state").at("data").at("query").as_string() ==
          "half-typed search");
    CHECK(store.pending_rehomes(0) == 0); // drained
}

void a_move_to_with_a_non_numeric_target_fails_closed()
{
    WindowMoveStore store;
    WindowBridge bridge(1, store);
    bridge.bind_move_to([](const WindowBridge::MoveTo&) -> WindowMoveResult
                        { return WindowMoveResult{true, 0, "", ""}; });
    Json params = Json::object();
    params.set("panelId", Json(std::string("builtin.problems")));
    // windowId missing entirely — a renderer bug that must not address window 0 by accident.
    std::string error_code;
    const Json out = bridge.move_to(params, error_code);
    CHECK(error_code == std::string(kErrWindowBadParams));
    CHECK(out.is_null());
}

void a_seed_is_delivered_once_then_absent()
{
    WindowMoveStore store;
    store.set_boot_seed(3, PanelSeed{"builtin.inspector", fresh_impossible_state()});
    WindowBridge bridge(3, store); // this IS window 3, reading its own seed

    const Json first = bridge.seed();
    CHECK(first.at("seeded").as_bool());
    CHECK(first.at("panelId").as_string() == "builtin.inspector");
    CHECK(first.at("state").at("data").at("scrollTop").as_int() == 4096);
    CHECK(bridge.seeds_served() == 1);

    // A second boot read (a reload) finds nothing — the seed is consumed once.
    const Json second = bridge.seed();
    CHECK(second.at("seeded").as_bool() == false);

    // An ORDINARY window (no seed) is not an error — it simply reports `seeded:false`.
    WindowBridge ordinary(kPrimaryWindowId, store);
    CHECK(ordinary.seed().at("seeded").as_bool() == false);
}

void list_reports_this_window_and_its_peers()
{
    WindowMoveStore store;
    WindowBridge bridge(1, store);
    bridge.bind_windows([]() -> std::vector<WindowId> { return {0, 1, 2}; });
    const Json out = bridge.list();
    CHECK(out.at("windowId").as_int() == 1);
    CHECK(out.at("windows").size() == 3);
    CHECK(out.at("windows").at(2).as_int() == 2);
}

// --- the full JSON-RPC binding over a real router (deny-by-default, nothing refused) --------------

void every_method_binds_and_serves_over_a_real_router()
{
    WindowMoveStore store;
    store.set_boot_seed(0, PanelSeed{"builtin.problems", fresh_impossible_state()});
    WindowBridge bridge(kPrimaryWindowId, store);
    bridge.bind_windows([]() -> std::vector<WindowId> { return {0}; });
    bridge.bind_tear_out([](const WindowBridge::TearOut&) -> WindowMoveResult
                         { return WindowMoveResult{true, 1, to_string(WindowCreateOutcome::created), ""}; });
    bridge.bind_close([](WindowId) -> WindowMoveResult
                      { return WindowMoveResult{true, 1, to_string(WindowDestroyOutcome::destroyed), ""}; });

    BridgeRouter router;
    CHECK(bridge.install(router));

    bool refused = false;
    // list
    Json out = dispatch(router, kWindowListMethod, Json::object(), refused);
    CHECK(!refused);
    CHECK(out.at("windowId").as_int() == 0);
    // seed — the boot seed we planted for window 0 comes back over the wire, opaque state intact.
    out = dispatch(router, kWindowSeedMethod, Json::object(), refused);
    CHECK(!refused);
    CHECK(out.at("seeded").as_bool());
    CHECK(out.at("state").at("data").at("scrollTop").as_int() == 4096);
    // tear-out
    Json params = Json::object();
    params.set("panelId", Json(std::string("builtin.inspector")));
    out = dispatch(router, kWindowTearOutMethod, params, refused);
    CHECK(!refused);
    CHECK(out.at("created").as_bool());
    // rehomed (empty is a valid, non-refused answer)
    out = dispatch(router, kWindowRehomedMethod, Json::object(), refused);
    CHECK(!refused);
    CHECK(out.at("panels").size() == 0);
    // close
    out = dispatch(router, kWindowCloseMethod, Json::object(), refused);
    CHECK(!refused);
    CHECK(out.at("closed").as_bool());

    // A malformed tear-out is a HANDLER-level bridge error (an error RESPONSE editor-core's
    // ShellBridge.call rejects on), NOT a ROUTER refusal: the envelope was well-formed, so
    // `dispatch.refused()` stays false and the router's `refused()` counter never moves. The
    // window.bad_params reason still reaches editor-core in the error's `data.reason`.
    Json bad = dispatch(router, kWindowTearOutMethod, Json::object(), refused);
    CHECK(!refused);
    CHECK(bad.at("data").at("reason").as_string() == std::string(kErrWindowBadParams));

    // The router refused NOTHING — every envelope was well-formed. This is the deny-by-default
    // invariant every live CEF smoke asserts across N routers (refused()==0); a handler's own error
    // does not break it.
    CHECK(router.refused() == 0);
}

} // namespace

int main()
{
    the_store_relays_a_boot_seed_once();
    the_store_queues_rehomes_in_order();
    a_tear_out_creates_seeds_and_reports_the_new_window();
    a_failed_tear_out_is_loud_not_silent();
    no_bound_handler_is_still_a_loud_answer();
    a_tear_out_with_no_panel_id_fails_closed();
    a_move_to_enqueues_for_the_target_window();
    a_move_to_with_a_non_numeric_target_fails_closed();
    a_seed_is_delivered_once_then_absent();
    list_reports_this_window_and_its_peers();
    every_method_binds_and_serves_over_a_real_router();
    SHELL_TEST_MAIN_END();
}
