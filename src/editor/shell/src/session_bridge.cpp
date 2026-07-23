// The daemon session read surface for editor-core (M9 e08d) — see session_bridge.h for the model.

#include "context/editor/shell/session_bridge.h"

#include <string>
#include <utility>

namespace context::editor::shell
{

void SessionBridge::bind_provider(Provider provider)
{
    provider_ = std::move(provider);
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
    return out;
}

bool SessionBridge::install(BridgeRouter& router)
{
    return router.register_method(kSessionStateMethod,
                                  [this](const BridgeRequest&) -> BridgeResult
                                  {
                                      ++reads_;
                                      return BridgeResult::ok(snapshot_json());
                                  });
}

} // namespace context::editor::shell
