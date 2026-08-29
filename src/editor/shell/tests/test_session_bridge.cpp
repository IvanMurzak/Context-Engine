// T1 for the daemon SESSION read surface (M9 e08d): the relay's wire shape, the honest unbound and
// throwing-provider degradations, the generation the renderer polls on, and the full JSON-RPC
// binding over a real BridgeRouter.
//
// WHAT THIS PROVES AND WHY IT MATTERS. `session.state` is the ONLY path by which daemon play state
// reaches editor-core's `when`-contexts — the browser side has no daemon socket (04 §1 / 08 §1) —
// so a defect here reads EXACTLY like the frozen `STUB_SESSION_STATE` bug this task removes: the
// editor is up, nothing errors, and every `playState == playing` clause is silently wrong forever.
// The properties pinned here are therefore the ones whose failure is INVISIBLE at runtime:
//
//   * the reply carries the daemon's own `play-state` FACT shape, so editor-core's e08b
//     `DaemonSessionState.applyFact` consumes it verbatim (a renamed member = a silently ignored
//     reply = the frozen bug back);
//   * an UNBOUND bridge is honest (`edit`, `attached:false`) rather than absent — that is the state
//     every CEF smoke installs, and the state a welcome-screen boot with no daemon is genuinely in;
//   * a THROWING provider degrades to that same baseline instead of taking the renderer's boot down.
//
// The `SessionFeed` -> token half is deliberately NOT re-proven here: the composition root reaches
// it through `panels::session_play_state`, whose token vocabulary is pinned by
// `editor-shell-test_session_feed` and by `gui::playbar::state_token` itself.

#include "context/editor/shell/session_bridge.h"

#include "context/editor/shell/ipc_bridge.h"

#include "shell_test.h"

#include <cstdint>
#include <stdexcept>
#include <string>

using namespace context::editor::shell;
using Json = context::editor::contract::Json;

namespace
{

Json dispatch_state(BridgeRouter& router, bool& refused)
{
    Json request = Json::object();
    request.set("jsonrpc", Json("2.0"));
    request.set("id", Json(7));
    request.set("method", Json(kSessionStateMethod));
    request.set("params", Json::object());
    const BridgeDispatch dispatch = router.dispatch(request.dump());
    refused = dispatch.refused();
    const Json response = Json::parse(dispatch.response);
    return response.at("result");
}

// One `session.control` dispatch. Returns the WHOLE response envelope: a bad verb answers a handler
// ERROR (result absent, error.code set), which callers here must be able to see.
Json dispatch_control(BridgeRouter& router, Json params, bool& refused)
{
    Json request = Json::object();
    request.set("jsonrpc", Json("2.0"));
    request.set("id", Json(8));
    request.set("method", Json(kSessionControlMethod));
    request.set("params", std::move(params));
    const BridgeDispatch dispatch = router.dispatch(request.dump());
    refused = dispatch.refused();
    return Json::parse(dispatch.response);
}

Json verb_params(const char* verb)
{
    Json params = Json::object();
    params.set("verb", Json(std::string(verb)));
    return params;
}

// An UNBOUND bridge is a supported state, not a hole: the CEF smokes install exactly this, and so
// does a Shell whose panel composition produced no session feed.
void unbound_serves_the_boot_baseline()
{
    SessionBridge bridge;
    const Json snapshot = bridge.snapshot_json();
    CHECK(snapshot.at("event").as_string() == std::string(kSessionPlayStateEvent));
    CHECK(snapshot.at("state").as_string() == std::string(kSessionPlayStateEdit));
    CHECK(snapshot.at("attached").as_bool() == false);
    CHECK(snapshot.at("generation").as_int() == 0);
    // d1: the additive simTick relay — 0 with no session is the honest baseline, and its PRESENCE
    // is the contract (an absent member reads as 0 browser-side, so a dropped relay would freeze
    // the strip's timer with nothing reporting it).
    CHECK(snapshot.contains("simTick"));
    CHECK(snapshot.at("simTick").as_int() == 0);
}

// The reply is the DAEMON's fact shape (docs/editor-session-state.md), because editor-core hands it
// straight to `DaemonSessionState.applyFact`. `origin` is the daemon's own 0: this is a relay of
// daemon state, not a change any client caused.
void a_live_provider_is_relayed_in_the_daemon_fact_shape()
{
    SessionBridge bridge;
    bridge.bind_provider(
        []
        {
            SessionStateSnapshot snapshot;
            snapshot.play_state = "playing";
            snapshot.attached = true;
            snapshot.generation = 4;
            snapshot.sim_tick = 42;
            return snapshot;
        });
    const Json snapshot = bridge.snapshot_json();
    CHECK(snapshot.at("event").as_string() == "play-state");
    CHECK(snapshot.at("state").as_string() == "playing");
    CHECK(snapshot.at("origin").as_int() == 0);
    CHECK(snapshot.at("attached").as_bool() == true);
    CHECK(snapshot.at("generation").as_int() == 4);
    CHECK(snapshot.at("simTick").as_int() == 42);
}

// The provider is read on EVERY call — a cached first answer would freeze the browser side exactly
// as the stub did, one layer lower and just as invisibly.
void every_read_re_asks_the_provider()
{
    SessionBridge bridge;
    std::string state = "edit";
    std::uint64_t generation = 0;
    bridge.bind_provider(
        [&state, &generation]
        {
            SessionStateSnapshot snapshot;
            snapshot.play_state = state;
            snapshot.attached = true;
            snapshot.generation = generation;
            return snapshot;
        });
    CHECK(bridge.snapshot().play_state == "edit");
    state = "paused";
    generation = 9;
    CHECK(bridge.snapshot().play_state == "paused");
    CHECK(bridge.snapshot().generation == 9);
}

// An EMPTY token would ride the wire as a value editor-core's `toPlayState` correctly refuses,
// leaving it on its last known state — a freeze with no diagnostic. Normalised to the baseline.
void an_empty_token_is_normalised_to_the_baseline()
{
    SessionBridge bridge;
    bridge.bind_provider(
        []
        {
            SessionStateSnapshot snapshot;
            snapshot.play_state.clear();
            snapshot.attached = true;
            return snapshot;
        });
    CHECK(bridge.snapshot().play_state == std::string(kSessionPlayStateEdit));
    CHECK(bridge.snapshot_json().at("attached").as_bool() == true);
}

// This runs on the renderer's query path. A throwing provider must cost the reply its freshness,
// never the editor its boot.
void a_throwing_provider_degrades_to_the_baseline()
{
    SessionBridge bridge;
    bridge.bind_provider([]() -> SessionStateSnapshot { throw std::runtime_error("no feed"); });
    const SessionStateSnapshot snapshot = bridge.snapshot();
    CHECK(snapshot.play_state == std::string(kSessionPlayStateEdit));
    CHECK(snapshot.attached == false);
}

// The end-to-end binding: a real router, a real envelope, no refusal, and the read counter the live
// CEF smoke uses as its "the channel is wired" evidence.
void served_over_a_real_router()
{
    BridgeRouter router;
    SessionBridge bridge;
    bridge.bind_provider(
        []
        {
            SessionStateSnapshot snapshot;
            snapshot.play_state = "paused";
            snapshot.attached = true;
            snapshot.generation = 2;
            return snapshot;
        });
    CHECK(bridge.install(router));
    CHECK(bridge.reads() == 0);

    bool refused = true;
    const Json first = dispatch_state(router, refused);
    CHECK(!refused);
    CHECK(first.at("state").as_string() == "paused");
    CHECK(bridge.reads() == 1);

    const Json second = dispatch_state(router, refused);
    CHECK(!refused);
    CHECK(second.at("generation").as_int() == 2);
    CHECK(bridge.reads() == 2);
}

// A second install is a WIRING BUG (the router refuses a duplicate name), and the caller is expected
// to check — a silently dropped registration presents as a mysteriously unknown method at runtime.
void a_duplicate_install_is_refused()
{
    BridgeRouter router;
    SessionBridge first;
    SessionBridge second;
    CHECK(first.install(router));
    CHECK(!second.install(router));
}

// --- the d1 `session.control` write half ---------------------------------------------------------

// UNBOUND control is the smokes' (and a feed-less Shell's) state: SERVED, never refused, answering
// the same honest "nothing to drive" a gateway-less PlaybarModel reports — changed:false, NO code.
void an_unbound_control_serves_nothing_to_drive()
{
    BridgeRouter router;
    SessionBridge bridge;
    CHECK(bridge.install(router));
    CHECK(router.has_method(kSessionControlMethod));

    bool refused = true;
    const Json response = dispatch_control(router, verb_params(kSessionControlVerbPlay), refused);
    CHECK(!refused);
    const Json& result = response.at("result");
    CHECK(result.at("changed").as_bool() == false);
    CHECK(result.at("errorCode").as_string().empty());
    CHECK(result.at("state").as_string() == std::string(kSessionPlayStateEdit));
    CHECK(result.at("simTick").as_int() == 0);
    CHECK(bridge.controls() == 1);
}

// A bound handler receives the validated verb and its outcome is relayed member-for-member — the
// strip renders exactly what the dock panel would.
void a_bound_control_handler_round_trips()
{
    BridgeRouter router;
    SessionBridge bridge;
    std::string seen;
    bridge.bind_control(
        [&seen](const std::string& verb)
        {
            seen = verb;
            SessionControlOutcome outcome;
            outcome.changed = true;
            outcome.play_state = "playing";
            outcome.sim_tick = 7;
            return outcome;
        });
    CHECK(bridge.install(router));

    bool refused = true;
    const Json response = dispatch_control(router, verb_params(kSessionControlVerbStep), refused);
    CHECK(!refused);
    CHECK(seen == std::string(kSessionControlVerbStep));
    const Json& result = response.at("result");
    CHECK(result.at("changed").as_bool() == true);
    CHECK(result.at("state").as_string() == "playing");
    CHECK(result.at("simTick").as_int() == 7);
    CHECK(bridge.controls() == 1);

    // A daemon refusal's reserved play.* code rides through verbatim (each maps to an exit class).
    bridge.bind_control(
        [](const std::string&)
        {
            SessionControlOutcome outcome;
            outcome.error_code = "play.not_running";
            outcome.play_state = "edit";
            return outcome;
        });
    const Json refusal = dispatch_control(router, verb_params(kSessionControlVerbPause), refused);
    CHECK(!refused);
    CHECK(refusal.at("result").at("changed").as_bool() == false);
    CHECK(refusal.at("result").at("errorCode").as_string() == "play.not_running");
}

// A verb outside the closed vocabulary — or a missing one — is a handler ERROR, never a router
// refusal (the envelope was valid, so `refused() == 0` holds even against a hostile caller), and
// the handler is never consulted.
void a_malformed_verb_is_an_error_not_a_refusal()
{
    BridgeRouter router;
    SessionBridge bridge;
    bool called = false;
    bridge.bind_control(
        [&called](const std::string&)
        {
            called = true;
            return SessionControlOutcome{};
        });
    CHECK(bridge.install(router));

    bool refused = true;
    const Json unknown = dispatch_control(router, verb_params("rewind"), refused);
    CHECK(!refused);
    CHECK(unknown.at("error").at("data").at("reason").as_string() ==
          std::string(kSessionControlBadVerbCode));
    const Json missing = dispatch_control(router, Json::object(), refused);
    CHECK(!refused);
    CHECK(missing.contains("error"));
    CHECK(!called);
    CHECK(bridge.controls() == 0);
}

// The renderer's query path again: a throwing handler costs the press its effect — reported through
// the provider's honest state — never the editor its boot.
void a_throwing_control_handler_degrades()
{
    BridgeRouter router;
    SessionBridge bridge;
    bridge.bind_provider(
        []
        {
            SessionStateSnapshot snapshot;
            snapshot.play_state = "paused";
            snapshot.sim_tick = 3;
            return snapshot;
        });
    bridge.bind_control([](const std::string&) -> SessionControlOutcome
                        { throw std::runtime_error("no feed"); });
    CHECK(bridge.install(router));

    bool refused = true;
    const Json response = dispatch_control(router, verb_params(kSessionControlVerbStop), refused);
    CHECK(!refused);
    const Json& result = response.at("result");
    CHECK(result.at("changed").as_bool() == false);
    CHECK(result.at("errorCode").as_string().empty());
    CHECK(result.at("state").as_string() == "paused");
    CHECK(result.at("simTick").as_int() == 3);
}

} // namespace

int main()
{
    unbound_serves_the_boot_baseline();
    a_live_provider_is_relayed_in_the_daemon_fact_shape();
    every_read_re_asks_the_provider();
    an_empty_token_is_normalised_to_the_baseline();
    a_throwing_provider_degrades_to_the_baseline();
    served_over_a_real_router();
    a_duplicate_install_is_refused();
    an_unbound_control_serves_nothing_to_drive();
    a_bound_control_handler_round_trips();
    a_malformed_verb_is_an_error_not_a_refusal();
    a_throwing_control_handler_degrades();
    SHELL_TEST_MAIN_END();
}
