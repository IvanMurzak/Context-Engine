// Bridge RPC dispatcher implementation (see dispatcher.h).

#include "context/editor/bridge/dispatcher.h"

#include "context/editor/bridge/event_stream.h"
#include "context/editor/contract/registry.h"

#include <stdexcept>
#include <utility>

namespace context::editor::bridge
{

using contract::ClientHandshake;
using contract::Envelope;
using contract::Json;
using contract::Negotiated;
using contract::Registry;
using contract::VerbSpec;

namespace
{
// JSON-RPC 2.0 error codes: the reserved set plus the -32000 server-error band for application
// failures (whose specific class travels in error.data.code — R-CLI-008 "same code" on the wire).
constexpr int kJsonRpcParseError = -32700;
constexpr int kJsonRpcInvalidRequest = -32600;
constexpr int kJsonRpcMethodNotFound = -32601;
constexpr int kJsonRpcInvalidParams = -32602;
constexpr int kJsonRpcServerError = -32000;

// Map an R-CLI-008 envelope error code onto the JSON-RPC transport-level integer.
int jsonrpc_code_for(const std::string& envelope_code)
{
    if (envelope_code == "usage.unknown_verb")
        return kJsonRpcMethodNotFound;
    if (envelope_code.rfind("usage.", 0) == 0)
        return kJsonRpcInvalidParams;
    return kJsonRpcServerError;
}

Json ok_response(const Json& id, Json result)
{
    Json out = Json::object();
    out.set("jsonrpc", Json(std::string("2.0")));
    out.set("id", id);
    out.set("result", std::move(result));
    return out;
}

// Build a JSON-RPC error response. `data` (optional) carries the R-CLI-008 error object so the wire
// error still exposes the same `code`.
Json error_response(const Json& id, int code, const std::string& message,
                    Json data = Json())
{
    Json err = Json::object();
    err.set("code", Json(code));
    err.set("message", Json(message));
    if (!data.is_null())
        err.set("data", std::move(data));

    Json out = Json::object();
    out.set("jsonrpc", Json(std::string("2.0")));
    out.set("id", id);
    out.set("error", std::move(err));
    return out;
}

// The R-CLI-008 error object of a failed envelope, for JSON-RPC error.data.
Json envelope_error_data(const Envelope& env)
{
    Json data = Json::object();
    if (env.error().has_value())
    {
        const contract::Error& e = *env.error();
        data.set("code", Json(e.code));
        data.set("message", Json(e.message));
        data.set("retriable", Json(e.retriable));
        if (e.pointer.has_value())
            data.set("pointer", Json(*e.pointer));
    }
    return data;
}

// R-CLI-015 subscription protocol, served directly against this daemon's live event stream (the same
// "real backing" pattern `describe` uses over the registry). subscribe returns {subId, snapshot}
// (snapshot-then-delta) + an optional replayed catch-up; unsubscribe/ack address a subscription by
// its subId. An unknown subId is subscription.unknown_sub; a missing subId/seq is a usage error.
Envelope serve_subscription(EventStream& stream, const std::string& method, const Json& params)
{
    if (method == "subscribe")
    {
        std::vector<std::string> topics;
        if (params.contains("topics") && params.at("topics").is_array())
        {
            const Json& t = params.at("topics");
            for (std::size_t i = 0; i < t.size(); ++i)
                if (t.at(i).is_string())
                    topics.push_back(t.at(i).as_string());
        }
        std::string path_scope;
        if (params.contains("pathScope") && params.at("pathScope").is_string())
            path_scope = params.at("pathScope").as_string();
        std::optional<std::uint64_t> since;
        if (params.contains("sinceSeq") && params.at("sinceSeq").is_number())
            since = static_cast<std::uint64_t>(params.at("sinceSeq").as_int());

        EventStream::SubscribeResult r =
            stream.subscribe(std::move(topics), std::move(path_scope), since);

        Json data = Json::object();
        data.set("subId", Json(r.sub_id));
        data.set("snapshot", r.snapshot);
        if (since.has_value())
        {
            data.set("gapped", Json(r.gapped)); // true => the sinceSeq predated retention; use snapshot
            Json catchup = Json::array();
            for (const Event& e : r.catchup)
                catchup.push_back(e.to_json());
            data.set("catchup", std::move(catchup));
        }
        return Envelope::success(std::move(data));
    }

    // unsubscribe + ack both address a subscription by subId.
    if (!params.contains("subId") || !params.at("subId").is_string() ||
        params.at("subId").as_string().empty())
        return Envelope::failure("usage.missing_argument",
                                 "the '" + method + "' method requires a 'subId'.");
    const std::string sub_id = params.at("subId").as_string();

    if (method == "unsubscribe")
    {
        if (!stream.unsubscribe(sub_id))
            return Envelope::failure("subscription.unknown_sub",
                                     "no live subscription '" + sub_id + "' on this daemon.");
        Json data = Json::object();
        data.set("subId", Json(sub_id));
        data.set("removed", Json(true));
        return Envelope::success(std::move(data));
    }

    // ack: advance the subscription's retention cursor.
    if (!params.contains("seq") || !params.at("seq").is_number())
        return Envelope::failure("usage.missing_argument", "the 'ack' method requires a 'seq'.");
    const auto seq = static_cast<std::uint64_t>(params.at("seq").as_int());
    if (!stream.ack(sub_id, seq))
        return Envelope::failure("subscription.unknown_sub",
                                 "no live subscription '" + sub_id + "' on this daemon.");
    Json data = Json::object();
    data.set("subId", Json(sub_id));
    data.set("ackedSeq", Json(seq));
    data.set("slowestAckedSeq", Json(stream.slowest_acked_seq()));
    return Envelope::success(std::move(data));
}

// Translate a completed envelope into the JSON-RPC response for request `id`.
Json envelope_to_response(const Json& id, const Envelope& env)
{
    if (env.ok())
        return ok_response(id, env.to_json());
    const std::string code = env.error().has_value() ? env.error()->code : std::string("internal.error");
    const std::string message =
        env.error().has_value() ? env.error()->message : std::string("Unclassified error.");
    return error_response(id, jsonrpc_code_for(code), message, envelope_error_data(env));
}
} // namespace

Dispatcher::AttachResult Dispatcher::attach(const ClientHandshake& client, ScopeSet requested) const
{
    contract::HandshakeResult result = contract::negotiate(client);
    if (const Envelope* fail = std::get_if<Envelope>(&result))
        return *fail;

    const Negotiated& agreed = std::get<Negotiated>(result);
    Session session;
    session.attached = true;
    session.protocol_major = agreed.protocol_major;
    session.capabilities = agreed.capabilities;
    // R-SEC-007 least privilege: clamp the requested scopes to the launch-time operator ceiling. This
    // is the SINGLE clamp point for BOTH the in-process (Daemon::attach_client) and the cross-process
    // wire (handle → attach) paths, so a wire client cannot escalate past `--launch-scopes`.
    session.scopes = ceiling_.intersect(requested);

    if (stream_ != nullptr)
    {
        Json ev = Json::object();
        ev.set("event", Json(std::string("attached")));
        ev.set("protocolMajor", Json(static_cast<std::uint64_t>(session.protocol_major)));
        Json scopes = Json::array();
        for (const std::string& s : session.scopes.names())
            scopes.push_back(Json(s));
        ev.set("scopes", std::move(scopes));
        stream_->publish("clients", std::move(ev));
    }
    return session;
}

Envelope Dispatcher::dispatch(const std::string& method, const Json& params,
                              const Session& session) const
{
    // R-SEC-007: the token scope is checked on EVERY method, BEFORE the verb resolves. An
    // under-scoped token cannot even learn whether the verb is implemented.
    if (!authorize(method, session.scopes))
    {
        return Envelope::failure(kScopeDeniedCode,
                                 "The attach token's scope does not permit '" + method +
                                     "' (R-SEC-007).");
    }

    // Offer the method to the composing backend FIRST (post scope-check): it serves the real
    // operational verbs (edit/query/… over the daemon's composed EditorKernel). nullopt => the
    // backend does not serve this method, so fall through to the default registry routing below.
    if (backend_ != nullptr)
    {
        if (std::optional<Envelope> served = backend_->invoke(method, params, session))
            return std::move(*served);
    }

    // R-CLI-015 subscription protocol: subscribe/unsubscribe/ack are served against this daemon's
    // live event stream (the "real backing" pattern, like `describe`). They are registered
    // operational verbs, so the R-SEC-007 scope gate above already ran (read/query baseline). A
    // dispatcher constructed without a stream (pure contract introspection) has no subscriptions, so
    // these fall through to the reserved-surface handling below.
    if (stream_ != nullptr &&
        (method == "subscribe" || method == "unsubscribe" || method == "ack"))
        return serve_subscription(*stream_, method, params);

    // Dispatch OVER the single registry (do NOT re-declare verbs). Resolve the method-id to a verb.
    const Registry& reg = Registry::instance();
    const VerbSpec* verb = nullptr;
    for (const VerbSpec& v : reg.verbs())
    {
        if (v.rpc_method == method)
        {
            verb = &v;
            break;
        }
    }
    if (verb == nullptr)
        return Envelope::failure("usage.unknown_verb", "no such RPC method: '" + method + "'");

    // `describe` has a real bridge backing (the whole-contract self-description). Every other verb is
    // the reserved bridge surface at M1 — its backing (file writes via filesync, install, session
    // control) lands in later tasks; invoking it returns contract.unimplemented (R-CLI-009).
    if (verb->noun.empty() && verb->verb == "describe")
        return Envelope::success(reg.describe());

    return Envelope::failure("contract.unimplemented",
                             "'" + verb->cli_command() +
                                 "' is reserved on the bridge; its backing is not wired yet.");
}

std::string Dispatcher::handle(const std::string& request_json, Session& session) const
{
    Json request;
    try
    {
        request = Json::parse(request_json);
    }
    catch (const std::exception&)
    {
        return error_response(Json(), kJsonRpcParseError, "Parse error: invalid JSON.").dump(0);
    }

    if (request.type() != Json::Type::object)
        return error_response(Json(), kJsonRpcInvalidRequest, "Invalid Request: expected an object.")
            .dump(0);

    // A request without an id is a notification — no response is produced.
    const bool is_notification = !request.contains("id");
    const Json id = request.contains("id") ? request.at("id") : Json();

    if (!request.contains("method") || !request.at("method").is_string())
    {
        if (is_notification)
            return std::string();
        return error_response(id, kJsonRpcInvalidRequest, "Invalid Request: missing method.").dump(0);
    }

    const std::string method = request.at("method").as_string();
    const Json params = request.contains("params") ? request.at("params") : Json::object();

    // --- attach: the capability-negotiation handshake (mutates the session) ---------------------
    if (method == "attach")
    {
        ClientHandshake client;
        if (params.contains("protocolMajor"))
            client.protocol_major = static_cast<std::uint32_t>(params.at("protocolMajor").as_int());
        if (params.contains("capabilities"))
        {
            const Json& caps = params.at("capabilities");
            for (std::size_t i = 0; i < caps.size(); ++i)
                client.capabilities.push_back(caps.at(i).as_string());
        }
        ScopeSet requested = params.contains("scope")
                                 ? ScopeSet::parse(params.at("scope").as_string())
                                 : ScopeSet::read_query();

        AttachResult result = attach(client, requested);
        if (const Envelope* fail = std::get_if<Envelope>(&result))
        {
            if (is_notification)
                return std::string();
            return envelope_to_response(id, *fail).dump(0);
        }

        session = std::get<Session>(result);
        if (is_notification)
            return std::string();

        Json data = Json::object();
        data.set("protocolMajor", Json(static_cast<std::uint64_t>(session.protocol_major)));
        Json caps = Json::array();
        for (const std::string& c : session.capabilities)
            caps.push_back(Json(c));
        data.set("capabilities", std::move(caps));
        Json scopes = Json::array();
        for (const std::string& s : session.scopes.names())
            scopes.push_back(Json(s));
        data.set("scopes", std::move(scopes));
        return ok_response(id, std::move(data)).dump(0);
    }

    // --- every other method requires an established session -------------------------------------
    if (!session.attached)
    {
        if (is_notification)
            return std::string();
        Envelope env = Envelope::failure(
            "usage.invalid", "attach first: send the 'attach' handshake before '" + method + "'.");
        return envelope_to_response(id, env).dump(0);
    }

    const Envelope env = dispatch(method, params, session);
    if (is_notification)
        return std::string();
    return envelope_to_response(id, env).dump(0);
}

} // namespace context::editor::bridge
