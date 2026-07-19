// Client implementation (see client.h): the real transport channel, the attach handshake, and the
// response/event demux.

#include "context/editor/client/client.h"

#include "context/editor/bridge/transport.h"

#include <utility>

namespace fs = std::filesystem;

namespace context::editor::client
{

using contract::Json;

namespace
{
// The real channel: bridge::TransportClient plus both reads the SDK needs — a blocking one for a
// request's response and a bounded one for the event pump (see wire.h).
class TransportChannel final : public WireChannel
{
public:
    TransportChannel(std::string endpoint, int connect_timeout_ms)
        : endpoint_(std::move(endpoint)), client_(endpoint_),
          connect_timeout_ms_(connect_timeout_ms)
    {
    }

    [[nodiscard]] bool open() { return client_.connect(connect_timeout_ms_); }

    [[nodiscard]] bool send(std::string_view request_json) override
    {
        return client_.send(request_json);
    }

    [[nodiscard]] std::optional<std::string> receive(int timeout_ms, bool& timed_out) override
    {
        return client_.receive_timed(timeout_ms, timed_out);
    }

    [[nodiscard]] std::optional<std::string> receive_blocking() override
    {
        return client_.receive();
    }

    [[nodiscard]] bool connected() const override { return client_.connected(); }

    [[nodiscard]] bool reconnect(int timeout_ms) override
    {
        client_.close();
        client_ = bridge::TransportClient(endpoint_);
        return client_.connect(timeout_ms);
    }

    [[nodiscard]] const std::string& error() const override { return client_.error(); }

private:
    std::string endpoint_;
    bridge::TransportClient client_;
    int connect_timeout_ms_;
};
} // namespace

std::unique_ptr<WireChannel> make_transport_channel(std::string endpoint, int connect_timeout_ms)
{
    auto channel = std::make_unique<TransportChannel>(std::move(endpoint), connect_timeout_ms);
    if (!channel->open())
        return nullptr;
    return channel;
}

Client::Client(std::unique_ptr<WireChannel> channel) : channel_(std::move(channel)) {}

std::unique_ptr<Client> Client::connect_to_project(const fs::path& project, int timeout_ms,
                                                   std::string& error)
{
    const std::optional<InstanceInfo> info = discover_instance(project, timeout_ms);
    if (!info.has_value())
    {
        error = "no running daemon found for project '" + project.string() +
                "' (no .editor/instance.json). Start one with `context daemon`.";
        return nullptr;
    }

    std::unique_ptr<WireChannel> channel = make_transport_channel(info->endpoint, timeout_ms);
    if (!channel)
    {
        error = "could not connect to the daemon endpoint '" + info->endpoint + "'";
        return nullptr;
    }
    auto client = std::make_unique<Client>(std::move(channel));
    client->set_instance(*info);
    return client;
}

bool Client::attach(const AttachOptions& options, std::string& error, bool* rejected_by_daemon)
{
    Json params = Json::object();
    params.set("protocolMajor", Json(static_cast<std::uint64_t>(options.protocol_major)));
    Json caps = Json::array();
    for (const std::string& c : options.capabilities)
        caps.push_back(Json(c));
    params.set("capabilities", std::move(caps));
    params.set("scope", Json(options.scope));
    // D20: the token is always carried. A daemon with enforcement OFF ignores it; one with
    // enforcement ON (the default since e02) refuses an attach without it. An unset options.token
    // falls back to the one connect_to_project() discovered, so the common path needs no plumbing.
    const std::string& token = options.token.empty() ? instance_.token : options.token;
    if (!token.empty())
        params.set("token", Json(token));

    const std::optional<Json> result = call("attach", std::move(params), error, rejected_by_daemon);
    if (!result.has_value())
        return false;

    granted_scopes_.clear();
    if (result->contains("data") && result->at("data").is_object())
    {
        const Json& data = result->at("data");
        if (data.contains("scopes") && data.at("scopes").is_array())
            for (std::size_t i = 0; i < data.at("scopes").size(); ++i)
                if (data.at("scopes").at(i).is_string())
                    granted_scopes_.push_back(data.at("scopes").at(i).as_string());
    }
    attached_ = true;
    return true;
}

std::optional<Json> Client::call(const std::string& method, Json params, std::string& error,
                                 bool* rejected_by_daemon)
{
    if (!channel_)
    {
        error = "client has no wire channel";
        return std::nullopt;
    }
    last_error_code_.clear();
    last_rejected_by_daemon_ = false;
    const std::int64_t id = ++next_id_;
    if (!channel_->send(build_request(id, method, std::move(params))))
    {
        error = "transport error on '" + method + "': " + channel_->error();
        return std::nullopt;
    }

    // Read until OUR response arrives; park every event frame we pass for poll_event(). The read
    // BLOCKS (never a timed poll) — a sleep-poll on this path loses the reply of a daemon that
    // answers and exits in the same breath; see WireChannel::receive_blocking(). It is bounded by
    // the peer: the daemon either answers or dies, and death surfaces as a disconnect.
    for (;;)
    {
        const std::optional<std::string> raw = channel_->receive_blocking();
        if (!raw.has_value())
        {
            error = "transport error on '" + method + "': " + channel_->error();
            return std::nullopt;
        }
        // Non-const: every exit below MOVES out of this frame rather than deep-copying it. Json is a
        // recursive value type, so a copy here reallocates the whole response tree — on `fetch` that
        // is a multi-megabyte chunk payload, once per chunk.
        std::optional<InboundFrame> frame = parse_frame(*raw);
        if (!frame.has_value())
        {
            error = "malformed response to '" + method + "'";
            return std::nullopt;
        }
        if (frame->kind == FrameKind::event || frame->kind == FrameKind::gap)
        {
            pending_events_.push_back(std::move(*frame));
            continue;
        }
        if (frame->kind != FrameKind::response || frame->id != id)
            continue; // an unmodelled notification, or a stale response — keep waiting for ours
        if (frame->has_error)
        {
            error = "daemon rejected '" + method + "': " + frame->error_message;
            last_error_code_ = std::move(frame->error_code);
            last_rejected_by_daemon_ = true;
            if (rejected_by_daemon != nullptr)
                *rejected_by_daemon = true;
            return std::nullopt;
        }
        return std::move(frame->result);
    }
}

std::string Client::failure_code(std::string_view fallback) const
{
    if (!last_rejected_by_daemon_)
        return "internal.error";
    return last_error_code_.empty() ? std::string(fallback) : last_error_code_;
}

std::optional<InboundFrame> Client::poll_event(int timeout_ms, bool& disconnected)
{
    disconnected = false;
    if (!pending_events_.empty())
    {
        InboundFrame frame = std::move(pending_events_.front());
        pending_events_.pop_front();
        return frame;
    }
    if (!channel_)
    {
        disconnected = true;
        return std::nullopt;
    }
    bool timed_out = false;
    const std::optional<std::string> raw = channel_->receive(timeout_ms, timed_out);
    if (!raw.has_value())
    {
        disconnected = !timed_out;
        return std::nullopt;
    }
    // Non-const so the return moves: this is the SDK's hottest path (once per pump()).
    std::optional<InboundFrame> frame = parse_frame(*raw);
    if (!frame.has_value())
        return std::nullopt; // a malformed frame is dropped, not fatal to the subscription
    if (frame->kind == FrameKind::response)
        return std::nullopt; // a late response to an abandoned call — ignore
    return frame;
}

bool Client::reconnect(const AttachOptions& options, int timeout_ms, std::string& error)
{
    attached_ = false;
    pending_events_.clear(); // stale pre-disconnect frames would corrupt the re-subscribed cursor
    if (!channel_)
    {
        error = "client has no wire channel";
        return false;
    }
    if (!channel_->reconnect(timeout_ms))
    {
        error = "could not reconnect: " + channel_->error();
        return false;
    }
    return attach(options, error);
}

} // namespace context::editor::client
