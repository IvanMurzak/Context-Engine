// The client-side wire seam: JSON-RPC 2.0 request framing, inbound-frame classification, and the
// abstract channel every SDK component talks to.
//
// A live client receives TWO frame shapes on one connection (D19): responses to its own requests and
// server-PUSHED `event` / `event.gap` notifications. So the SDK never assumes "the next frame is my
// response" (bridge::TransportClient::request's documented restriction) — it sends, receives, and
// DEMUXES by frame shape. WireChannel is the seam that makes that logic drivable both over the real
// transport and, in tests, over a scripted mock — the subscription consumer's protocol (snapshot,
// delta, ack, gap, reconnect, incarnation epoch) is then testable with no daemon at all.

#pragma once

#include "context/editor/contract/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace context::editor::client
{

// Build one compact JSON-RPC 2.0 request string.
[[nodiscard]] std::string build_request(std::int64_t id, const std::string& method,
                                        contract::Json params);

// What one inbound frame IS. `unknown` covers anything the SDK does not model (a future
// notification method) — deliberately ignorable rather than fatal, so a newer daemon's additive
// notification never breaks an older client.
enum class FrameKind
{
    response, // {jsonrpc, id, result|error}
    event,    // {jsonrpc, method:"event", params:{subId, event}}
    gap,      // {jsonrpc, method:"event.gap", params:{}}
    unknown,
};

// One classified inbound frame.
struct InboundFrame
{
    FrameKind kind = FrameKind::unknown;

    // response
    std::int64_t id = 0;
    bool has_error = false;
    std::string error_message;
    // The R-CLI-008 catalog code the daemon put in `error.data.code` (`attach.denied`,
    // `subscription.unknown_sub`, …). Empty when the peer sent no structured code. Carried so a
    // client can re-emit the daemon's OWN classification instead of inferring one from context —
    // the refusals differ in exit class, so a guess is a wrong exit code.
    std::string error_code;
    contract::Json result;

    // event
    std::string sub_id;
    contract::Json event;
};

// Classify + destructure one inbound frame. nullopt only when the text is not parseable JSON or is
// not an object (a framing/protocol fault, distinct from an unmodelled-but-valid frame).
[[nodiscard]] std::optional<InboundFrame> parse_frame(const std::string& text);

// The transport seam. Implementations: TransportChannel (the real IPC wire) and, in tests, a
// scripted mock. Every method is non-blocking-friendly: receive() takes a bounded wait so a consumer
// can interleave protocol work with waiting.
class WireChannel
{
public:
    virtual ~WireChannel() = default;

    // Write one framed request WITHOUT waiting for a reply. false on a transport failure.
    [[nodiscard]] virtual bool send(std::string_view request_json) = 0;

    // Read one framed message, waiting up to `timeout_ms`. Returns the frame on success; nullopt
    // with `timed_out=true` when nothing arrived (NOT an error — loop), or nullopt with
    // `timed_out=false` on disconnect / I/O error (error() carries the reason).
    //
    // Use this ONLY where "nothing arrived" is a normal outcome — i.e. pumping for pushed events.
    // Never for a request's response: see receive_blocking().
    [[nodiscard]] virtual std::optional<std::string> receive(int timeout_ms, bool& timed_out) = 0;

    // Block until one framed message arrives, or the peer goes away. nullopt on disconnect / I/O
    // error only.
    //
    // WHY a request's response MUST use this rather than a timed poll: on Windows the timed read is
    // a PeekNamedPipe + Sleep poll (a synchronous byte-mode pipe has no cross-frame wait primitive),
    // so between polls the client is NOT parked in a read — and when a named pipe's SERVER end
    // closes, any data the client has not yet read is DISCARDED. A daemon that replies and exits in
    // the same breath (`shutdown`) therefore loses its reply to the sleeping client, which then
    // reports "peer disconnected" for a call the daemon actually served. A blocking read is parked
    // in ReadFile when the write lands, so the frame is delivered. (POSIX is not exposed to this —
    // a closed socket still delivers buffered bytes before EOF — but the contract is the same on
    // both.) This mirrors bridge::TransportClient::request()'s proven write-then-blocking-read.
    [[nodiscard]] virtual std::optional<std::string> receive_blocking() = 0;

    [[nodiscard]] virtual bool connected() const = 0;

    // Re-establish the connection after a disconnect (the reconnect-with-backoff path). false when
    // the endpoint is still unreachable within `timeout_ms`.
    [[nodiscard]] virtual bool reconnect(int timeout_ms) = 0;

    [[nodiscard]] virtual const std::string& error() const = 0;
};

} // namespace context::editor::client
