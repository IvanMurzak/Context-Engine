// Client-facing event stream implementation (see event_stream.h).

#include "context/editor/bridge/event_stream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
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

// Is `path` within the `scope` subtree? A prefix match with a component boundary — `scope` itself,
// or a strict child under `scope/…` — so scope "a/b" matches "a/b" and "a/b/c" but never "a/bc".
bool path_within_scope(const std::string& path, const std::string& scope)
{
    if (path == scope)
        return true;
    return path.size() > scope.size() && path.compare(0, scope.size(), scope) == 0 &&
           path[scope.size()] == '/';
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

Subscriber::Subscriber(std::vector<std::string> topics, std::size_t capacity, std::string path_scope)
    : topics_(std::move(topics)), path_scope_(std::move(path_scope)),
      capacity_(capacity == 0 ? 1 : capacity)
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

bool Subscriber::accepts(const Event& e) const
{
    if (!wants(e.topic))
        return false;
    if (path_scope_.empty())
        return true;
    // Path-scoped: a path-bearing event is delivered only when its payload `path` is within the
    // scope subtree. A pathless event (session/clients/log lifecycle) is not a path-scoped fact and
    // always passes — narrowing to a subtree must not hide the daemon's own lifecycle stream.
    if (e.payload.type() != contract::Json::Type::object || !e.payload.contains("path"))
        return true;
    const contract::Json& p = e.payload.at("path");
    if (!p.is_string())
        return true;
    return path_within_scope(p.as_string(), path_scope_);
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
    prune_ring();

    for (Subscriber* sub : subscribers_)
        if (sub != nullptr && sub->accepts(e))
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
    if (ring_.empty())
        // Slowest-acked retention (R-CLI-015) can drain the ring entirely while last_seq_ > 0 (every
        // retained event acked by all live subscribers). A stale reconnect (since < last_seq_) then
        // predates retention just as it does when the ring is non-empty: gap => fresh snapshot. Only
        // `since == last_seq_` is genuinely caught-up (nothing after `since` existed to evict).
        gapped = since < last_seq_;
    else if (ring_.front().seq > since + 1)
        gapped = true; // events after `since` were already evicted — fresh snapshot needed

    std::vector<Event> out;
    for (const Event& e : ring_)
        if (e.seq > since)
            out.push_back(e);
    return out;
}

std::uint64_t EventStream::slowest_acked_seq() const
{
    // No live subscriptions => floor 0: pure size-cap retention, preserving the reconnect
    // "since seq N" window for a client that has not (re)subscribed yet.
    if (subscriptions_.empty())
        return 0;
    std::uint64_t floor = subscriptions_.front().acked_seq;
    for (const Subscription& s : subscriptions_)
        floor = std::min(floor, s.acked_seq);
    return floor;
}

void EventStream::prune_ring()
{
    const std::uint64_t floor = slowest_acked_seq();
    // Evict the oldest event while EITHER the hard capacity is exceeded (bounded memory — an
    // over-slow subscriber must never grow the ring without bound) OR it has been acked by every
    // live subscriber (seq <= floor), so no live subscriber still needs it for catch-up.
    while (!ring_.empty() && (ring_.size() > ring_capacity_ || ring_.front().seq <= floor))
        ring_.pop_front();
}

EventStream::SubscribeResult EventStream::subscribe(std::vector<std::string> topics,
                                                    std::string path_scope,
                                                    std::optional<std::uint64_t> since_seq,
                                                    std::size_t capacity)
{
    SubscribeResult result;
    result.sub_id = "sub-" + std::to_string(++sub_counter_);

    // Snapshot-then-delta (R-BRIDGE-008): the client reads this current-state snapshot first; live
    // deltas then arrive on the subscription's queue (drained via poll()).
    result.snapshot = snapshot();

    // The initial ack cursor. A fresh subscriber has "seen" state through the snapshot's lastSeq, so
    // it does not pin ring history at or below it. A reconnect with since_seq resumes from there —
    // its cursor sits at since_seq until it acks the replayed catch-up forward, so retention keeps
    // that history pinned for it (bounded by ring_capacity).
    std::uint64_t acked = last_seq_;
    if (since_seq.has_value())
    {
        result.catchup = replay_since(*since_seq, result.gapped);
        if (*since_seq < acked)
            acked = *since_seq;
    }

    auto owned = std::make_unique<Subscriber>(std::move(topics), capacity, std::move(path_scope));
    Subscriber* raw = owned.get();
    subscriptions_.push_back({result.sub_id, std::move(owned), acked});
    add_subscriber(raw);
    return result;
}

bool EventStream::unsubscribe(const std::string& sub_id)
{
    for (auto it = subscriptions_.begin(); it != subscriptions_.end(); ++it)
    {
        if (it->id == sub_id)
        {
            remove_subscriber(it->sub.get());
            subscriptions_.erase(it);
            prune_ring(); // dropping the slowest subscriber may raise the retention floor
            return true;
        }
    }
    return false;
}

bool EventStream::ack(const std::string& sub_id, std::uint64_t seq)
{
    for (Subscription& s : subscriptions_)
    {
        if (s.id == sub_id)
        {
            if (seq > s.acked_seq) // monotonic: a stale/duplicate ack is a no-op, never a rewind
                s.acked_seq = seq;
            prune_ring(); // advancing the slowest cursor ages out newly-acked history
            return true;
        }
    }
    return false;
}

std::vector<Event> EventStream::poll(const std::string& sub_id)
{
    for (Subscription& s : subscriptions_)
        if (s.id == sub_id)
            return s.sub->drain();
    return {};
}

bool EventStream::sub_gapped(const std::string& sub_id) const
{
    for (const Subscription& s : subscriptions_)
        if (s.id == sub_id)
            return s.sub->gap();
    return false;
}

} // namespace context::editor::bridge
