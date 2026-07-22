// context_client — the client SDK's connection object: connect, the R-CLI-010 capability-negotiating
// attach (carrying the D20 token + requested scopes), and typed one-shot request helpers.
//
// The ONE implementation of the client half of the wire. The `context` CLI rides it, and so does any
// out-of-tree consumer (the editor shell, e05's JS client host) — there is deliberately no second
// copy of this plumbing to drift.
//
// Demux discipline: a client that has subscribed also receives server-pushed `event` frames
// interleaved with its responses, so call() reads until it sees ITS response id and PARKS every
// event frame it passes on an internal queue that poll_event() drains. That is why a subscribing
// client must not use bridge::TransportClient::request() directly.

#pragma once

#include "context/editor/client/instance.h"
#include "context/editor/client/wire.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/contract/json.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::client
{

// The real channel, over the loopback IPC transport. Declared here so a consumer can build a Client
// over a live endpoint without reaching for bridge headers itself.
[[nodiscard]] std::unique_ptr<WireChannel> make_transport_channel(std::string endpoint,
                                                                  int connect_timeout_ms);

// What a client asks for at attach time.
struct AttachOptions
{
    // The R-SEC-007 scope spec ("read", "write,session", …). The daemon clamps it to the launch-time
    // operator ceiling — a client never gets more than the operator allowed.
    std::string scope = "read";
    std::vector<std::string> capabilities{"describe"};
    // The D20 per-instance attach token (from InstanceInfo::token). Enforcement is ON by default
    // since e02: an attach with a missing/wrong token is refused `attach.denied`.
    std::string token;
    std::uint32_t protocol_major = contract::kProtocolMajor;
};

class Client
{
public:
    explicit Client(std::unique_ptr<WireChannel> channel);

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = default;
    Client& operator=(Client&&) = default;

    // Discover + connect in one step: read `<project>/.editor/instance.json` and connect to the
    // endpoint it names. nullptr (+ `error`) when no daemon is discoverable or the endpoint refuses
    // the connection. The discovered D20 token is retained on the Client (see instance()), so a
    // subsequent attach() authenticates with no token plumbing by the caller.
    [[nodiscard]] static std::unique_ptr<Client> connect_to_project(
        const std::filesystem::path& project, int timeout_ms, std::string& error);

    // The R-CLI-010 capability handshake. false (+ `error`) on refusal or transport failure; when
    // `rejected_by_daemon` is non-null it distinguishes a daemon-side rejection (protocol mismatch,
    // `attach.denied`, `daemon.busy`) from a mere transport hiccup — pair it with last_error_code()
    // to learn WHICH refusal. An empty `options.token` falls back to the token discovered by
    // connect_to_project(); set it only to pin a token explicitly.
    [[nodiscard]] bool attach(const AttachOptions& options, std::string& error,
                              bool* rejected_by_daemon = nullptr);

    // Send one request and return its `result`. Event frames received while waiting are queued for
    // poll_event(). nullopt (+ `error`) on a transport failure, a JSON-RPC error response, or a
    // malformed reply.
    [[nodiscard]] std::optional<contract::Json> call(const std::string& method,
                                                     contract::Json params, std::string& error,
                                                     bool* rejected_by_daemon = nullptr);

    // Take the next queued/inbound event or gap frame, waiting up to `timeout_ms`. nullopt when
    // nothing arrived in the window (`disconnected` false) or the peer went away (`disconnected`
    // true — the caller reconnects).
    [[nodiscard]] std::optional<InboundFrame> poll_event(int timeout_ms, bool& disconnected);

    // Re-establish the wire and re-attach with `options` (the reconnect path). false (+ `error`) if
    // either half fails. Queued events from the dead connection are dropped: after a reconnect the
    // consumer re-subscribes, and stale pre-disconnect frames would corrupt its cursor.
    [[nodiscard]] bool reconnect(const AttachOptions& options, int timeout_ms, std::string& error);

    [[nodiscard]] bool attached() const noexcept { return attached_; }
    [[nodiscard]] bool connected() const { return channel_ && channel_->connected(); }

    // The catalog code (`attach.denied`, `subscription.unknown_sub`, …) the daemon returned with the
    // most recent REFUSED call. Empty otherwise, and reset at the start of every call. Re-emit it
    // verbatim rather than guessing a code: the refusals a caller must distinguish (auth vs protocol
    // vs busy) carry different exit classes.
    [[nodiscard]] const std::string& last_error_code() const noexcept { return last_error_code_; }

    // The R-CLI-008 code to report for the most recent FAILED call — the whole rule in one place,
    // because getting it wrong is invisible in review: every code maps to an EXIT CLASS, so a
    // plausible guess reports the wrong exit status to a script.
    //
    //   * the call failed on the WIRE (or the reply was malformed) -> `internal.error`: a transport
    //     fault says nothing about what the caller asked for;
    //   * the DAEMON refused -> its own catalog code, verbatim;
    //   * it refused without a structured code (an older daemon) -> `fallback`, the one refusal that
    //     call site could otherwise expect.
    //
    // Callers pass the fallback that fits their verb — `handshake.incompatible_protocol` for attach,
    // `resource.unknown_handle` for resource.read. Do NOT collapse every refusal into the fallback:
    // since e02 enforces the attach token, and since the dispatcher can refuse any verb with
    // `scope.denied` / `usage.invalid`, the fallback is the exception, not the rule.
    [[nodiscard]] std::string failure_code(std::string_view fallback) const;

    // The discovery hint this client was built from — populated by connect_to_project(), empty for a
    // Client constructed directly over a channel. Lets a caller report the endpoint it actually
    // reached without re-reading (and re-racing) the instance file, and carries the D20 token
    // attach() falls back to.
    [[nodiscard]] const InstanceInfo& instance() const noexcept { return instance_; }
    [[nodiscard]] const std::vector<std::string>& granted_scopes() const noexcept
    {
        return granted_scopes_;
    }

    // This connection's per-daemon CLIENT ID, learned from the attach reply (M9 e08a / D7). It is
    // the identity the `session` topic's `origin` field carries: a consumer applies a fact whose
    // `origin` differs from this and DROPS one that matches — that single rule is the whole
    // echo-suppression contract. 0 when not attached, or when the daemon predates e08a.
    [[nodiscard]] std::uint64_t client_id() const noexcept { return client_id_; }

private:
    // Only connect_to_project() (a static member, so it has access) seeds this. Deliberately NOT
    // public: the instance document is discovery output, not something a consumer overrides.
    void set_instance(InstanceInfo info) { instance_ = std::move(info); }

    std::unique_ptr<WireChannel> channel_;
    InstanceInfo instance_;
    std::deque<InboundFrame> pending_events_;
    std::string last_error_code_;
    std::int64_t next_id_ = 0;
    bool attached_ = false;
    // Whether the most recent failed call was a daemon REFUSAL rather than a transport fault.
    // Latched here so failure_code() answers without every caller threading a bool out of call().
    bool last_rejected_by_daemon_ = false;
    std::vector<std::string> granted_scopes_;
    // The e08a echo-suppression identity this connection was assigned (see client_id()).
    std::uint64_t client_id_ = 0;
};

} // namespace context::editor::client
