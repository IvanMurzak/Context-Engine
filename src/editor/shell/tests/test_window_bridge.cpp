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
#include <optional>
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

// --- the chrome contract (editor-window-chrome a1, target design 02 §1 / §5) ----------------------

void chrome_state_unbound_is_the_honest_system_default()
{
    WindowMoveStore store;
    WindowBridge bridge(kPrimaryWindowId, store);

    const Json out = bridge.chrome_state();
    // The LITERALS, not the kChrome* constants: this pins the wire strings (the uibus.test.ts
    // rationale) — a constant compared to itself would hold for whatever value it drifted to.
    CHECK(out.at("mode").as_string() == "system");
    CHECK(out.at("controlsInset").at("left").as_int() == 0);
    CHECK(out.at("controlsInset").at("right").as_int() == 0);
    CHECK(out.at("maximized").as_bool() == false);
    CHECK(out.at("focused").as_bool() == false);
    CHECK(out.at("window").as_string() == "primary");
    CHECK(bridge.chrome_reads() == 1);

    // A NON-primary bridge reports the secondary role — the strips gate their compact frame on it
    // (02 §9). Derived from self_id, so no provider can misreport it.
    WindowBridge secondary(7, store);
    CHECK(secondary.chrome_state().at("window").as_string() == "secondary");
}

void chrome_state_serves_the_bound_provider()
{
    WindowMoveStore store;
    WindowBridge bridge(kPrimaryWindowId, store);
    bridge.bind_chrome_state(
        []() -> ChromeState
        {
            ChromeState state;
            // Values a DEFAULT could not have (the fresh-impossible-state discipline): a c1-shaped
            // hybrid answer, so a provider that stopped being consulted is caught by every field.
            state.mode = ChromeMode::hybrid;
            state.controls_inset_left = 72;
            state.controls_inset_right = 4;
            state.maximized = true;
            state.focused = true;
            return state;
        });

    const Json out = bridge.chrome_state();
    CHECK(out.at("mode").as_string() == "hybrid");
    CHECK(out.at("controlsInset").at("left").as_int() == 72);
    CHECK(out.at("controlsInset").at("right").as_int() == 4);
    CHECK(out.at("maximized").as_bool());
    CHECK(out.at("focused").as_bool());
    CHECK(out.at("window").as_string() == "primary");
}

void chrome_mode_tokens_cover_the_closed_enum()
{
    CHECK(std::string(chrome_mode_token(ChromeMode::system)) == "system");
    CHECK(std::string(chrome_mode_token(ChromeMode::custom)) == "custom");
    CHECK(std::string(chrome_mode_token(ChromeMode::hybrid)) == "hybrid");
}

void window_controls_unbound_degrade_to_accepted_false_and_still_count()
{
    WindowMoveStore store;
    WindowBridge bridge(kPrimaryWindowId, store);

    // No handlers bound — every sibling smoke's state. Each verb still ANSWERS (never a refusal)
    // and still COUNTS: the ten-smoke rule's claim is the routing, and the live smoke asserts it
    // from exactly these counters.
    CHECK(bridge.minimize().at("accepted").as_bool() == false);
    const Json toggled = bridge.toggle_maximize();
    CHECK(toggled.at("accepted").as_bool() == false);
    CHECK(toggled.at("maximized").as_bool() == false);
    std::string focus_error;
    CHECK(bridge.focus(Json::object(), focus_error).at("accepted").as_bool() == false);
    CHECK(focus_error.empty());
    CHECK(bridge.minimizes() == 1);
    CHECK(bridge.maximize_toggles() == 1);
    CHECK(bridge.focus_requests() == 1);
}

void window_controls_reach_their_handlers()
{
    WindowMoveStore store;
    WindowBridge bridge(kPrimaryWindowId, store);
    int minimized = 0;
    int focused = 0;
    WindowId focus_target = kInvalidWindowId;
    bridge.bind_minimize(
        [&minimized]() -> bool
        {
            ++minimized;
            return true;
        });
    // The toggle answers the NEW state the handler computed from OS truth — `true` here is a value
    // the unbound default could not produce, so a bridge that stopped consulting the handler reds
    // both members.
    bridge.bind_toggle_maximize([]() -> std::optional<bool> { return true; });
    bridge.bind_focus(
        [&focused, &focus_target](WindowId target) -> bool
        {
            ++focused;
            focus_target = target;
            return true;
        });

    CHECK(bridge.minimize().at("accepted").as_bool());
    CHECK(minimized == 1);
    const Json toggled = bridge.toggle_maximize();
    CHECK(toggled.at("accepted").as_bool());
    CHECK(toggled.at("maximized").as_bool());
    std::string focus_error;
    // d3: an ABSENT windowId resolves to SELF — the a1 behaviour, unchanged for every existing
    // caller (the titlebar's focus button sends no params).
    CHECK(bridge.focus(Json::object(), focus_error).at("accepted").as_bool());
    CHECK(focus_error.empty());
    CHECK(focused == 1);
    CHECK(focus_target == kPrimaryWindowId);
    // d3: a PRESENT windowId targets the named peer (the Window menu's window-list entries).
    Json peer = Json::object();
    peer.set("windowId", Json(static_cast<std::uint64_t>(4)));
    CHECK(bridge.focus(peer, focus_error).at("accepted").as_bool());
    CHECK(focus_error.empty());
    CHECK(focus_target == 4);
    // d3: a present-but-non-numeric id is REFUSED, never silently retargeted at self.
    Json malformed = Json::object();
    malformed.set("windowId", Json(std::string("four")));
    (void)bridge.focus(malformed, focus_error);
    CHECK(focus_error == kErrWindowBadParams);
    CHECK(focused == 2); // the malformed ask never reached the handler

    // A handler that answers "no window" (a retired session's registry lookup) is the SAME honest
    // degrade as unbound — accepted:false, not an error.
    bridge.bind_toggle_maximize([]() -> std::optional<bool> { return std::nullopt; });
    const Json refused_toggle = bridge.toggle_maximize();
    CHECK(refused_toggle.at("accepted").as_bool() == false);
    CHECK(refused_toggle.at("maximized").as_bool() == false);
}

void menu_publish_is_total_and_reaches_its_handler()
{
    // d3 — the menu publish (window_bridge.h § menu_publish). The OUTER shape is fail-closed
    // (anything but `{menus: [...]}` is kErrWindowBadParams — a malformed publish is a loud wiring
    // bug, never a silently-empty menu bar); a well-formed one counts, tallies its command items,
    // and reaches the bound handler verbatim.
    WindowMoveStore store;
    WindowBridge bridge(kPrimaryWindowId, store);

    std::string error_code;
    // Malformed shapes fail closed and are NOT counted.
    (void)bridge.menu_publish(Json::object(), error_code);
    CHECK(error_code == kErrWindowBadParams);
    error_code.clear();
    Json non_array = Json::object();
    non_array.set("menus", Json(std::string("not an array")));
    (void)bridge.menu_publish(non_array, error_code);
    CHECK(error_code == kErrWindowBadParams);
    error_code.clear();
    CHECK(bridge.menu_publishes() == 0);

    // A well-formed model: routed, counted (bound or not — the ten-smoke discipline), tallied.
    Json items = Json::array();
    Json about = Json::object();
    about.set("type", Json(std::string("command")));
    about.set("id", Json(std::string("help.about")));
    about.set("label", Json(std::string("About")));
    items.push_back(std::move(about));
    Json help = Json::object();
    help.set("label", Json(std::string("Help")));
    help.set("items", std::move(items));
    Json menus = Json::array();
    menus.push_back(std::move(help));
    Json model = Json::object();
    model.set("menus", std::move(menus));

    // Unbound: the honest accepted:false — every non-macOS composition root and every sibling smoke.
    Json out = bridge.menu_publish(model, error_code);
    CHECK(error_code.empty());
    CHECK(out.at("accepted").as_bool() == false);
    CHECK(bridge.menu_publishes() == 1);
    CHECK(bridge.last_menu_commands() == 1);

    // Bound: the model reaches the handler VERBATIM (the handler re-parses through menu_model.h),
    // and its answer is relayed as `accepted`.
    std::size_t handled = 0;
    bridge.bind_menu(
        [&handled](const Json& published) -> bool
        {
            ++handled;
            return published.at("menus").size() == 1;
        });
    out = bridge.menu_publish(model, error_code);
    CHECK(error_code.empty());
    CHECK(out.at("accepted").as_bool());
    CHECK(handled == 1);
    CHECK(bridge.menu_publishes() == 2);
}

void appearance_report_is_total_and_reaches_its_handler()
{
    // b1 — the appearance report (window_bridge.h § set_appearance). The token parse is
    // FAIL-CLOSED: a drifted token silently defaulted would tint the frame wrong with both builds
    // green, so anything but the two pinned LITERALS (not the constants — the uibus.test.ts
    // wire-string rationale) is kErrWindowBadParams.
    WindowMoveStore store;
    WindowBridge bridge(kPrimaryWindowId, store);

    std::string error_code;
    Json params = Json::object();
    params.set("appearance", Json(std::string("dark")));
    // Unbound: routed, counted, honest accepted:false — the sibling-smoke state.
    Json out = bridge.set_appearance(params, error_code);
    CHECK(error_code.empty());
    CHECK(out.at("accepted").as_bool() == false);
    CHECK(bridge.appearance_reports() == 1);
    CHECK(bridge.last_appearance_dark());

    // Bound: the handler receives the decoded bool, both token directions.
    std::optional<bool> seen;
    bridge.bind_appearance(
        [&seen](bool dark) -> bool
        {
            seen = dark;
            return true;
        });
    out = bridge.set_appearance(params, error_code);
    CHECK(error_code.empty());
    CHECK(out.at("accepted").as_bool());
    CHECK(seen.has_value() && *seen);
    params.set("appearance", Json(std::string("light")));
    out = bridge.set_appearance(params, error_code);
    CHECK(error_code.empty());
    CHECK(seen.has_value() && !*seen);
    CHECK(bridge.appearance_reports() == 3);
    CHECK(!bridge.last_appearance_dark());

    // Fail-closed: a missing/non-string/unknown token refuses and neither counts nor reaches the
    // handler.
    seen.reset();
    error_code.clear();
    (void)bridge.set_appearance(Json::object(), error_code);
    CHECK(error_code == std::string(kErrWindowBadParams));
    error_code.clear();
    Json drifted = Json::object();
    drifted.set("appearance", Json(std::string("darkish")));
    (void)bridge.set_appearance(drifted, error_code);
    CHECK(error_code == std::string(kErrWindowBadParams));
    error_code.clear();
    Json wrong_type = Json::object();
    wrong_type.set("appearance", Json(true));
    (void)bridge.set_appearance(wrong_type, error_code);
    CHECK(error_code == std::string(kErrWindowBadParams));
    CHECK(!seen.has_value());
    CHECK(bridge.appearance_reports() == 3);
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
    // a1 — the chrome read + the three control verbs route (nothing is refused; unbound handlers
    // answer the honest degrade). This is the T1 half of the ten-smoke rule: install() is the ONE
    // registration site every live smoke rides, so these four binding here is what makes "installed
    // in all ten smokes" a structural fact rather than ten hand-edits.
    out = dispatch(router, kChromeStateMethod, Json::object(), refused);
    CHECK(!refused);
    CHECK(out.at("mode").as_string() == "system");
    CHECK(out.at("window").as_string() == "primary");
    out = dispatch(router, kWindowMinimizeMethod, Json::object(), refused);
    CHECK(!refused);
    CHECK(out.at("accepted").as_bool() == false);
    out = dispatch(router, kWindowToggleMaximizeMethod, Json::object(), refused);
    CHECK(!refused);
    CHECK(out.at("accepted").as_bool() == false);
    out = dispatch(router, kWindowFocusMethod, Json::object(), refused);
    CHECK(!refused);
    CHECK(out.at("accepted").as_bool() == false);
    // b1 — the appearance report routes too; a well-formed report is never a refusal.
    Json appearance = Json::object();
    appearance.set("appearance", Json(std::string("dark")));
    out = dispatch(router, kWindowSetAppearanceMethod, appearance, refused);
    CHECK(!refused);
    CHECK(out.at("accepted").as_bool() == false); // unbound: the honest degrade
    // ...and a malformed one is a HANDLER-level error like the malformed tear-out below — the
    // window.bad_params reason travels in data.reason, the router's refused() never moves.
    Json bad_appearance = dispatch(router, kWindowSetAppearanceMethod, Json::object(), refused);
    CHECK(!refused);
    CHECK(bad_appearance.at("data").at("reason").as_string() == std::string(kErrWindowBadParams));
    // d3 — the menu publish routes (the ten-smoke rule's structural half, exactly like the a1
    // block above): a well-formed model is never a refusal, unbound answers the honest false.
    Json menu_model = Json::object();
    menu_model.set("menus", Json::array());
    out = dispatch(router, kMenuPublishMethod, menu_model, refused);
    CHECK(!refused);
    CHECK(out.at("accepted").as_bool() == false); // unbound: no native menu host in this build

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
    chrome_state_unbound_is_the_honest_system_default();
    chrome_state_serves_the_bound_provider();
    chrome_mode_tokens_cover_the_closed_enum();
    window_controls_unbound_degrade_to_accepted_false_and_still_count();
    window_controls_reach_their_handlers();
    menu_publish_is_total_and_reaches_its_handler();
    appearance_report_is_total_and_reaches_its_handler();
    every_method_binds_and_serves_over_a_real_router();
    SHELL_TEST_MAIN_END();
}
