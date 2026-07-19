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
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace context::editor::bridge
{

class EventStream;

// The D20 attach-token refusal (missing/wrong token when enforcement is ON). Promoted into the
// versioned error-code catalog (error_catalog.{h,cpp}) as a permission-class code, grep-stable and
// identical to the catalog entry — so the bridge references the one catalog entry by value (the same
// promote-a-local-string pattern as kScopeDeniedCode).
inline constexpr const char* kAttachDeniedCode = "attach.denied";

// A per-connection session. The transport owns one Session per attached client; the dispatcher
// establishes it on a successful attach and reads its scopes on every subsequent method.
struct Session
{
    bool attached = false;
    std::uint32_t protocol_major = contract::kProtocolMajor;
    std::vector<std::string> capabilities; // the negotiated capability subset
    ScopeSet scopes;                       // granted scopes (read/query baseline by default)
    // The R-CLI-015 subscription ids this connection minted (D19 fan-out bookkeeping): the serve
    // loop drains + pushes each of these on every fan-out pass and unsubscribes them all on
    // disconnect. Populated by handle() on a successful subscribe, pruned on unsubscribe. Only ever
    // read/written on the daemon's single dispatch thread (serialized), so it needs no own lock.
    std::vector<std::string> subscriptions;
};

// The seam that lets a COMPOSING layer (the EditorKernel, which sits ABOVE the bridge and therefore
// cannot be depended on from here without a cycle) supply the real backing for a resolved method —
// so a cross-process client's `edit`/`query`/… actually mutate/read the derived world instead of
// returning contract.unimplemented. The dispatcher runs the R-SEC-007 scope check FIRST, then offers
// each method to the backend (on an attached session only); the backend returns nullopt for any
// method it does not serve, letting the dispatcher fall back to its default registry routing.
class MethodBackend
{
public:
    virtual ~MethodBackend() = default;
    [[nodiscard]] virtual std::optional<contract::Envelope>
    invoke(const std::string& method, const contract::Json& params, const Session& session) const = 0;
};

class Dispatcher
{
public:
    // `stream` (optional, non-owning): when set, a successful attach emits a `clients` event.
    // `backend` (optional, non-owning): the real backing for resolved verbs (see MethodBackend); when
    // null the reserved verbs return contract.unimplemented exactly as before.
    // `ceiling`: the launch-time operator scope ceiling (R-SEC-007). It is the SINGLE clamp point —
    // attach() intersects EVERY attaching client's requested scopes against it, so the in-process
    // (Daemon::attach_client) and cross-process wire (handle → attach) paths honor `--launch-scopes`
    // identically. Default: all scopes (no restriction).
    explicit Dispatcher(EventStream* stream = nullptr, const MethodBackend* backend = nullptr,
                        ScopeSet ceiling = ScopeSet::all())
        : stream_(stream), backend_(backend), ceiling_(ceiling)
    {
    }

    // D20 attach-token enforcement (R-SEC-002). `expected` is the daemon's per-instance token (the
    // one it writes to `.editor/instance.json`); `required` is the compat flag. Enforcement is
    // DEFAULT OFF (e01, C-F1 sequencing): with `required == false` the client's token is carried but
    // NEVER gated on (the M1 ambient-OS-guard trust model preserved). With `required == true` an
    // attach whose token is absent or does not match `expected` is refused with `attach.denied`.
    //
    // MUST be called BEFORE the serve loop spawns per-connection threads (the thread-creation
    // happens-before edge publishes these values to the reader threads that call attach()); it is not
    // safe to reconfigure once clients are being served.
    void configure_attach_auth(std::string expected, bool required)
    {
        expected_token_ = std::move(expected);
        auth_required_ = required;
    }

    [[nodiscard]] bool attach_auth_required() const noexcept { return auth_required_; }

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
    const MethodBackend* backend_;
    ScopeSet ceiling_;
    // D20 attach-token enforcement (default OFF — see configure_attach_auth). Set-once before the
    // serve loop spawns connection threads; read on the attach path of every connection thereafter.
    std::string expected_token_;
    bool auth_required_ = false;
};

} // namespace context::editor::bridge
