// The daemon session read surface for editor-core (M9 e08d) — see session_bridge.h for the model.

#include "context/editor/shell/session_bridge.h"

#include <string>
#include <utility>

namespace context::editor::shell
{

namespace
{

// Is `verb` in the closed `session.control` vocabulary? The bridge validates BEFORE dispatching, so
// a bound handler only ever sees these four tokens.
[[nodiscard]] bool known_control_verb(const std::string& verb)
{
    return verb == kSessionControlVerbPlay || verb == kSessionControlVerbPause ||
           verb == kSessionControlVerbStop || verb == kSessionControlVerbStep;
}

} // namespace

void SessionBridge::bind_provider(Provider provider)
{
    provider_ = std::move(provider);
}

void SessionBridge::bind_control(ControlHandler handler)
{
    control_ = std::move(handler);
}

SessionStateSnapshot SessionBridge::snapshot() const
{
    if (!provider_)
    {
        return SessionStateSnapshot{};
    }
    try
    {
        SessionStateSnapshot snapshot = provider_();
        // A provider that answers with an EMPTY token would put an unrecognised value on the wire,
        // where editor-core's `toPlayState` correctly refuses it and keeps its last known state —
        // an invisible freeze. Normalising to the boot baseline keeps the reply honest instead.
        if (snapshot.play_state.empty())
        {
            snapshot.play_state = kSessionPlayStateEdit;
        }
        return snapshot;
    }
    catch (...)
    {
        // Contained, never propagated: this runs on the renderer's query path, and a throwing
        // provider must cost the reply its freshness, not the editor its boot.
        return SessionStateSnapshot{};
    }
}

contract::Json SessionBridge::snapshot_json() const
{
    const SessionStateSnapshot state = snapshot();
    contract::Json out = contract::Json::object();
    // The daemon's own fact shape, so editor-core hands the reply straight to
    // `DaemonSessionState.applyFact` (when.ts) with no translation step to drift.
    out.set("event", contract::Json(std::string(kSessionPlayStateEvent)));
    out.set("state", contract::Json(state.play_state));
    // The daemon's own origin. This is a RELAY of daemon state, not a client-caused change — see
    // session_bridge.h § THE REPLY IS THE DAEMON'S OWN FACT SHAPE.
    out.set("origin", contract::Json(static_cast<std::uint64_t>(0)));
    out.set("attached", contract::Json(state.attached));
    out.set("generation", contract::Json(state.generation));
    // ADDITIVE (editor-window-chrome d1): the running session's simTick, so the strip's `t+` timer
    // renders daemon truth. Riding the same reply keeps it under the same generation compare.
    out.set("simTick", contract::Json(state.sim_tick));
    return out;
}

contract::Json SessionBridge::control_json(const std::string& verb)
{
    SessionControlOutcome outcome;
    if (control_)
    {
        try
        {
            outcome = control_(verb);
        }
        catch (...)
        {
            // Contained, never propagated — the renderer's query path again. A throwing handler
            // costs the press its effect, reported honestly below, never the editor its boot.
            // ONE snapshot() read: two would pay the provider twice and could tear the
            // state/tick pair across a concurrent transition.
            const SessionStateSnapshot state = snapshot();
            outcome = SessionControlOutcome{};
            outcome.play_state = state.play_state;
            outcome.sim_tick = state.sim_tick;
        }
    }
    else
    {
        // UNBOUND is the smokes' (and a feed-less Shell's) state: nothing to drive. The same honest
        // shape a gateway-less PlaybarModel reports — ok:false with NO code, the current state.
        const SessionStateSnapshot state = snapshot();
        outcome.play_state = state.play_state;
        outcome.sim_tick = state.sim_tick;
    }
    if (outcome.play_state.empty())
    {
        // The snapshot() normalisation, applied to the write half for the same reason: an empty
        // token on the wire reads as "unrecognised" and silently freezes the consumer.
        outcome.play_state = kSessionPlayStateEdit;
    }
    contract::Json out = contract::Json::object();
    out.set("changed", contract::Json(outcome.changed));
    out.set("state", contract::Json(outcome.play_state));
    out.set("simTick", contract::Json(outcome.sim_tick));
    out.set("errorCode", contract::Json(outcome.error_code));
    return out;
}

bool SessionBridge::install(BridgeRouter& router)
{
    bool ok = router.register_method(kSessionStateMethod,
                                     [this](const BridgeRequest&) -> BridgeResult
                                     {
                                         ++reads_;
                                         return BridgeResult::ok(snapshot_json());
                                     });
    ok = router.register_method(
             kSessionControlMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 const contract::Json& verb_member = request.params.at("verb");
                 const std::string verb = verb_member.is_string() ? verb_member.as_string() : "";
                 if (!known_control_verb(verb))
                 {
                     // A handler ERROR, not a router refusal (the envelope was valid) — so the
                     // smokes' `refused() == 0` invariant is untouched even by a hostile caller.
                     return BridgeResult::error(kSessionControlBadVerbCode,
                                                "session.control needs a verb of play|pause|stop|step");
                 }
                 ++controls_;
                 return BridgeResult::ok(control_json(verb));
             }) &&
         ok;
    return ok;
}

} // namespace context::editor::shell
