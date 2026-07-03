// Bridge RPC dispatcher (R-BRIDGE / R-SEC-007 / R-CLI-010): JSON-RPC 2.0 over the bridge, dispatching
// OVER the single contract registry with the attach-token scope checked on every method.
//
// The dispatcher does NOT re-declare verbs — it dispatches over the one registry in
// src/editor/contract/ (registry.h), so CLI ≡ RPC ≡ MCP holds by construction. Attach is the
// capability-negotiation handshake (R-CLI-010): it reuses contract::negotiate (carry
// {protocolMajor, capabilities[]}, hard-fail outside the window at v1). Authorization is R-SEC-007:
// enforcement lives HERE, in the dispatcher — not the MCP adapter, whose tool filtering is bypassable
// via direct RPC — so the token's scope is checked on EVERY method before the verb resolves.

#pragma once

#include "context/editor/bridge/scope.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/contract/json.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace context::editor::bridge
{

class EventStream;

// A per-connection session. The transport owns one Session per attached client; the dispatcher
// establishes it on a successful attach and reads its scopes on every subsequent method.
struct Session
{
    bool attached = false;
    std::uint32_t protocol_major = contract::kProtocolMajor;
    std::vector<std::string> capabilities; // the negotiated capability subset
    ScopeSet scopes;                       // granted scopes (read/query baseline by default)
};

class Dispatcher
{
public:
    // `stream` (optional, non-owning): when set, a successful attach emits a `clients` event.
    explicit Dispatcher(EventStream* stream = nullptr) : stream_(stream) {}

    using AttachResult = std::variant<Session, contract::Envelope>;

    // The capability-negotiation attach (R-CLI-010). Reuses contract::negotiate: on success an
    // attached Session carrying the negotiated capability subset + the requested scopes; outside the
    // compatibility window the R-CLI-008 failure envelope (handshake.incompatible_protocol).
    [[nodiscard]] AttachResult attach(const contract::ClientHandshake& client,
                                      ScopeSet requested) const;

    // Scope-enforced dispatch of one resolved method over the contract registry. The scope check
    // (R-SEC-007) happens FIRST — an under-scoped token is rejected before the verb resolves — then
    // the registry is consulted (`describe` returns the self-description; other verbs are the
    // reserved bridge surface at M1).
    [[nodiscard]] contract::Envelope dispatch(const std::string& method,
                                              const contract::Json& params,
                                              const Session& session) const;

    // JSON-RPC 2.0 message entry. Parses one request object: the "attach" method negotiates and
    // MUTATES `session`; every other method is scope-checked against the attached session. Returns
    // the JSON-RPC 2.0 response string (compact), or "" for a notification (a request with no id).
    [[nodiscard]] std::string handle(const std::string& request_json, Session& session) const;

private:
    EventStream* stream_;
};

} // namespace context::editor::bridge
