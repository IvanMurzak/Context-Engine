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

    // Discover + connect in one step: read `<project>/.editor/instance.json`, connect to the endpoint
    // it names, and seed `attach_options.token` from the document. nullptr (+ `error`) when no daemon
    // is discoverable or the endpoint refuses the connection.
    [[nodiscard]] static std::unique_ptr<Client> connect_to_project(
        const std::filesystem::path& project, int timeout_ms, AttachOptions& attach_options,
        std::string& error);

    // The R-CLI-010 capability handshake. false (+ `error`) on refusal or transport failure; when
    // `rejected_by_daemon` is non-null it distinguishes a daemon-side rejection (protocol mismatch,
    // `attach.denied`, `daemon.busy`) from a mere transport hiccup.
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

    // The discovery hint this client was built from — populated by connect_to_project(), empty for a
    // Client constructed directly over a channel. Lets a caller report the endpoint it actually
    // reached without re-reading (and re-racing) the instance file.
    [[nodiscard]] const InstanceInfo& instance() const noexcept { return instance_; }
    void set_instance(InstanceInfo info) { instance_ = std::move(info); }
    [[nodiscard]] const std::vector<std::string>& granted_scopes() const noexcept
    {
        return granted_scopes_;
    }
    [[nodiscard]] WireChannel& channel() noexcept { return *channel_; }

private:
    std::unique_ptr<WireChannel> channel_;
    InstanceInfo instance_;
    std::deque<InboundFrame> pending_events_;
    std::int64_t next_id_ = 0;
    bool attached_ = false;
    std::vector<std::string> granted_scopes_;
};

} // namespace context::editor::client
