// Client-facing event stream implementation (see event_stream.h).

#include "context/editor/bridge/event_stream.h"

#include <atomic>
#include <chrono>
#include <utility>

namespace context::editor::bridge
{

using contract::Json;

const char* stability_name(Stability s)
{
    switch (s)
    {
    case Stability::stable:
        return "stable";
    case Stability::unstable:
        return "unstable";
    case Stability::settling:
        return "settling";
    }
    return "stable";
}

namespace
{
const char* log_level_name(kernel::LogLevel level)
{
    switch (level)
    {
    case kernel::LogLevel::trace:
        return "trace";
    case kernel::LogLevel::debug:
        return "debug";
    case kernel::LogLevel::info:
        return "info";
    case kernel::LogLevel::warn:
        return "warn";
    case kernel::LogLevel::error:
        return "error";
    }
    return "info";
}

// A process-unique incarnation id: a steady-clock stamp plus a monotonic counter so two daemons
// started in the same tick still differ. Hex-encoded, "inc-" prefixed, grep-friendly.
std::string generate_incarnation_id()
{
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    static const char* hex = "0123456789abcdef";
    std::string out = "inc-";
    const std::uint64_t mixed = ticks ^ (n * 0x9e3779b97f4a7c15ull);
    for (int shift = 60; shift >= 0; shift -= 4)
        out.push_back(hex[(mixed >> shift) & 0xf]);
    return out;
}
} // namespace

Json Event::to_json() const
{
    Json out = Json::object();
    out.set("seq", Json(seq));
    out.set("incarnationId", Json(incarnation_id));
    out.set("generation", Json(generation));
    out.set("topic", Json(topic));
    out.set("payload", payload);
    return out;
}

Subscriber::Subscriber(std::vector<std::string> topics, std::size_t capacity)
    : topics_(std::move(topics)), capacity_(capacity == 0 ? 1 : capacity)
{
}

bool Subscriber::wants(const std::string& topic) const
{
    if (topics_.empty())
        return true; // no filter => every topic
    for (const std::string& t : topics_)
        if (t == topic)
            return true;
    return false;
}

void Subscriber::offer(const Event& e)
{
    if (queue_.size() >= capacity_)
    {
        // Overflow: drop and mark the gap — never block the stream on a slow client.
        gap_ = true;
        return;
    }
    last_seq_ = e.seq;
    queue_.push_back(e);
}

std::vector<Event> Subscriber::drain()
{
    std::vector<Event> out(queue_.begin(), queue_.end());
    queue_.clear();
    return out;
}

EventStream::EventStream() : EventStream(generate_incarnation_id()) {}

EventStream::EventStream(std::string incarnation_id, std::size_t ring_capacity)
    : incarnation_id_(std::move(incarnation_id)), ring_capacity_(ring_capacity == 0 ? 1
                                                                                    : ring_capacity)
{
}

std::uint64_t EventStream::emit(const std::string& topic, Json payload)
{
    Event e;
    e.seq = ++last_seq_;
    e.incarnation_id = incarnation_id_;
    e.generation = generation_;
    e.topic = topic;
    e.payload = std::move(payload);

    ring_.push_back(e);
    while (ring_.size() > ring_capacity_)
        ring_.pop_front();

    for (Subscriber* sub : subscribers_)
        if (sub != nullptr && sub->wants(topic))
            sub->offer(e);

    return e.seq;
}

std::uint64_t EventStream::publish(const std::string& topic, Json payload,
                                   std::optional<Stability> stability)
{
    // Diagnostics ALWAYS carry a stability field (default stable); other topics only when supplied.
    if (topic == "diagnostics" && !stability.has_value())
        stability = Stability::stable;
    if (stability.has_value())
    {
        if (payload.type() != Json::Type::object)
            payload = Json::object();
        payload.set("stability", Json(std::string(stability_name(*stability))));
    }
    return emit(topic, std::move(payload));
}

std::uint64_t EventStream::settle()
{
    ++generation_;
    Json payload = Json::object();
    payload.set("event", Json(std::string("derivation.settled")));
    payload.set("generation", Json(generation_));
    return emit("derivation", std::move(payload));
}

std::uint64_t EventStream::forward_log(const kernel::LogEvent& e)
{
    Json payload = Json::object();
    payload.set("level", Json(std::string(log_level_name(e.level))));
    payload.set("message", Json(e.message));
    return emit("log", std::move(payload));
}

void EventStream::add_subscriber(Subscriber* sub)
{
    if (sub != nullptr)
        subscribers_.push_back(sub);
}

void EventStream::remove_subscriber(Subscriber* sub)
{
    for (auto it = subscribers_.begin(); it != subscribers_.end(); ++it)
    {
        if (*it == sub)
        {
            subscribers_.erase(it);
            return;
        }
    }
}

Json EventStream::snapshot() const
{
    Json out = Json::object();
    out.set("incarnationId", Json(incarnation_id_));
    out.set("generation", Json(generation_));
    out.set("lastSeq", Json(last_seq_));
    return out;
}

std::vector<Event> EventStream::replay_since(std::uint64_t since, bool& gapped) const
{
    gapped = false;
    if (!ring_.empty() && ring_.front().seq > since + 1)
        gapped = true; // events after `since` were already evicted — fresh snapshot needed

    std::vector<Event> out;
    for (const Event& e : ring_)
        if (e.seq > since)
            out.push_back(e);
    return out;
}

} // namespace context::editor::bridge
