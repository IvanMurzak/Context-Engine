// A scripted WireChannel for the subscription-consumer protocol tests: it answers requests from
// per-method handlers and pushes server-originated frames on cue, so every branch of the protocol
// (snapshot, delta, ack, gap, reconnect, incarnation change) is drivable deterministically with NO
// daemon, NO sockets, and NO timing races.
//
// It is a MOCK of the wire, not of the consumer's logic: it speaks the same JSON-RPC frames the real
// daemon emits (dispatcher.cpp's envelopes + kernel_server.cpp's `event` / `event.gap`
// notifications), so a consumer that satisfies this mock satisfies the real protocol. The live-daemon
// e2e test is what proves the two agree.
//
// ONE shape is NOT an envelope, and a `Responder` must script it verbatim rather than reaching for
// ok_envelope(): the ATTACH handshake. `Dispatcher::handle` puts {protocolMajor, clientId,
// capabilities, scopes} FLAT into JSON-RPC `result`, with no `ok`/`data` wrapper. A mock that
// envelopes it is MORE forgiving than the daemon and hides a real reader bug (it hid an empty
// granted_scopes() from e02 to e08a) — see test_client.cpp's flat-handshake test.

#pragma once

#include "context/editor/client/wire.h"
#include "context/editor/contract/json.h"

#include <cassert>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace clientmock
{

using context::editor::client::WireChannel;
using context::editor::contract::Json;

// One recorded outbound request.
struct Request
{
    std::int64_t id = 0;
    std::string method;
    Json params;
};

class MockChannel final : public WireChannel
{
public:
    // A per-method responder: given the request, return the envelope to put in `result`.
    using Responder = std::function<Json(const Request&)>;

    // Wrap `data` in the R-CLI-008 success envelope the dispatcher returns.
    static Json ok_envelope(Json data)
    {
        Json env = Json::object();
        env.set("ok", Json(true));
        env.set("data", std::move(data));
        return env;
    }

    void on(const std::string& method, Responder responder)
    {
        responders_[method] = std::move(responder);
    }

    // Make `method` answer with a JSON-RPC ERROR response (the daemon-side refusal shape — an
    // `attach.denied`, a `daemon.busy`, an unknown subId), as distinct from a transport failure.
    //
    // `catalog_code` fills `error.data.code`, which is where dispatcher.cpp's envelope_error_data()
    // puts the R-CLI-008 code and where Client reads last_error_code() from. Leave it EMPTY only to
    // model a peer that sent no structured code (the `fallback` branch of Client::failure_code) —
    // a real daemon always sends one, so a mock that never did would make every refusal look like
    // that one exceptional case.
    // `detail` fills the NESTED `error.data.data` — the refusal's structured payload (the
    // cas.mismatch fresh state, design 05 §7). Leave it null for a refusal that carries none; a
    // mock that never modelled it is what let the daemon compute a rebase payload no SDK consumer
    // could read (M9 e09b-1).
    void fail_method(const std::string& method, std::string message, std::string catalog_code = {},
                     Json detail = {})
    {
        errors_[method] = {std::move(message), std::move(catalog_code), std::move(detail), -1};
    }

    // Make the next `times` calls to `method` fail, then fall through to its ordinary responder — a
    // TRANSIENT refusal, which `fail_method`'s permanent form cannot express.
    //
    // The motivating consumer (M9 e09b-2): the L-30 REBASE path is defined by a first attempt refused
    // `cas.mismatch` and a RETRY against the fresh token that SUCCEEDS. Under a permanent failure the
    // retry is refused too, so the engine drops — and a rebase that is never retried is
    // indistinguishable from a drop, which is exactly the distinction the L-30 policy exists to make.
    //
    // `times` MUST be >= 1. The retire check is `remaining > 0`, so a non-positive count is the
    // PERMANENT sentinel `fail_method` installs — i.e. `fail_method_times(m, 0, …)` would fail every
    // call forever rather than none, the opposite of what the name reads like. Asserted rather than
    // clamped so the caller's intent is corrected at the call site.
    void fail_method_times(const std::string& method, int times, std::string message,
                           std::string catalog_code = {}, Json detail = {})
    {
        assert(times >= 1);
        errors_[method] = {std::move(message), std::move(catalog_code), std::move(detail), times};
    }

    // Queue a server-pushed frame the consumer will read on its next receive().
    void push_frame(std::string frame) { inbound_.push_back(std::move(frame)); }

    void push_event(const std::string& sub_id, Json event)
    {
        Json params = Json::object();
        params.set("subId", Json(sub_id));
        params.set("event", std::move(event));
        push_frame(notification("event", std::move(params)));
    }

    void push_gap() { push_frame(notification("event.gap", Json::object())); }

    // Make the next receive() report a peer disconnect (the reconnect path).
    void break_connection() { connected_ = false; }
    // How many reconnect() calls must fail before one succeeds (drives the backoff assertions).
    void set_failed_reconnects(int n) { failed_reconnects_ = n; }
    [[nodiscard]] int reconnect_attempts() const { return reconnect_attempts_; }

    // Every request recorded for `method`, in order.
    [[nodiscard]] std::vector<Request> requests_for(const std::string& method) const
    {
        std::vector<Request> out;
        for (const Request& r : requests_)
            if (r.method == method)
                out.push_back(r);
        return out;
    }

    // --- WireChannel -----------------------------------------------------------------------------

    [[nodiscard]] bool send(std::string_view request_json) override
    {
        if (!connected_)
        {
            error_ = "not connected";
            return false;
        }
        const Json req = Json::parse(std::string(request_json));
        Request r;
        r.id = req.contains("id") ? req.at("id").as_int() : 0;
        r.method = req.contains("method") ? req.at("method").as_string() : std::string();
        if (req.contains("params"))
            r.params = req.at("params");
        requests_.push_back(r);

        Json response = Json::object();
        response.set("jsonrpc", Json(std::string("2.0")));
        response.set("id", Json(r.id));

        const auto failed = errors_.find(r.method);
        if (failed != errors_.end())
        {
            const Refusal refusal = failed->second;
            // A BOUNDED refusal (`fail_method_times`) is consumed here; the permanent form
            // (`remaining < 0`) is left in place. Retiring it on exhaustion is what lets the SAME
            // method answer normally afterwards — the whole point of the bounded form.
            if (failed->second.remaining > 0 && --failed->second.remaining == 0)
                errors_.erase(failed);

            Json err = Json::object();
            err.set("code", Json(-32000)); // the JSON-RPC server-error band the dispatcher uses
            err.set("message", Json(refusal.message));
            if (!refusal.catalog_code.empty())
            {
                // envelope_error_data()'s shape: the R-CLI-008 error object under `error.data`.
                Json data = Json::object();
                data.set("code", Json(refusal.catalog_code));
                data.set("message", Json(refusal.message));
                data.set("retriable", Json(false));
                if (!refusal.detail.is_null())
                    data.set("data", refusal.detail); // the nested structured payload
                err.set("data", std::move(data));
            }
            response.set("error", std::move(err));
            inbound_.push_back(response.dump(0));
            return true;
        }

        Json result = Json::object();
        const auto it = responders_.find(r.method);
        if (it != responders_.end())
            result = it->second(r);
        else
            result = ok_envelope(Json::object());
        response.set("result", std::move(result));
        inbound_.push_back(response.dump(0));
        return true;
    }

    [[nodiscard]] std::optional<std::string> receive(int /*timeout_ms*/, bool& timed_out) override
    {
        timed_out = false;
        if (!inbound_.empty())
        {
            std::string frame = inbound_.front();
            inbound_.pop_front();
            return frame;
        }
        if (!connected_)
        {
            error_ = "peer disconnected";
            return std::nullopt;
        }
        timed_out = true;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> receive_blocking() override
    {
        if (!inbound_.empty())
        {
            std::string frame = inbound_.front();
            inbound_.pop_front();
            return frame;
        }
        // Nothing queued and nothing can arrive later (the mock is driven, not concurrent), so a
        // blocking read here is exactly a disconnect.
        error_ = "peer disconnected";
        return std::nullopt;
    }

    [[nodiscard]] bool connected() const override { return connected_; }

    [[nodiscard]] bool reconnect(int /*timeout_ms*/) override
    {
        ++reconnect_attempts_;
        if (failed_reconnects_ > 0)
        {
            --failed_reconnects_;
            error_ = "connection refused";
            return false;
        }
        connected_ = true;
        inbound_.clear();
        return true;
    }

    [[nodiscard]] const std::string& error() const override { return error_; }

private:
    static std::string notification(std::string method, Json params)
    {
        Json out = Json::object();
        out.set("jsonrpc", Json(std::string("2.0")));
        out.set("method", Json(std::move(method)));
        out.set("params", std::move(params));
        return out.dump(0);
    }

    struct Refusal
    {
        std::string message;
        std::string catalog_code;
        Json detail; // the nested error.data.data (null = the refusal carried no structured detail)
        // How many more calls this refusal answers: -1 = forever (`fail_method`), N > 0 = the next N
        // (`fail_method_times`), after which the entry is retired and the method answers normally.
        int remaining = -1;
    };

    std::map<std::string, Responder> responders_;
    std::map<std::string, Refusal> errors_;
    std::deque<std::string> inbound_;
    std::vector<Request> requests_;
    std::string error_;
    bool connected_ = true;
    int failed_reconnects_ = 0;
    int reconnect_attempts_ = 0;
};

// Build the {seq, incarnationId, generation, topic, payload} wire envelope.
inline Json make_event(std::uint64_t seq, const std::string& incarnation, const std::string& topic,
                       std::uint64_t generation = 1)
{
    Json e = Json::object();
    e.set("seq", Json(seq));
    e.set("incarnationId", Json(incarnation));
    e.set("generation", Json(generation));
    e.set("topic", Json(topic));
    e.set("payload", Json::object());
    return e;
}

// Build the stream snapshot shape {incarnationId, generation, lastSeq}.
inline Json make_snapshot(const std::string& incarnation, std::uint64_t last_seq,
                          std::uint64_t generation = 1)
{
    Json s = Json::object();
    s.set("incarnationId", Json(incarnation));
    s.set("generation", Json(generation));
    s.set("lastSeq", Json(last_seq));
    return s;
}

} // namespace clientmock
