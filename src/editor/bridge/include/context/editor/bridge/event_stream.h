// Client-facing event stream (R-BRIDGE-008): the SEPARATE, post-derivation stream clients subscribe
// to — distinct from the kernel-internal EventBus (context::kernel, event_bus.h).
//
// The kernel EventBus is how RuntimeKernel systems talk to each other in-process; NOTHING on it is
// exposed to clients. This stream carries post-derivation FACTS to attached CLI/GUI/AI clients over
// the bridge. Every event carries a monotonic, totally-ordered `seq` and the current INCARNATION
// epoch id (so a client can tell one daemon lifetime from the next) plus the derived-world
// GENERATION counter. The core topics mirror the contract registry's advertised set
// (files/derivation/diagnostics/session/clients/log). settle() advances the generation and emits the
// `derivation.settled{generation}` quiescence event; diagnostics carry a `stability` field; and the
// kernel `log` topic is FORWARDED here (kept a separate stream, never the same object). Slow
// subscribers get a bounded queue and, on overflow, an explicit gap marker instructing a re-snapshot
// — the stream never blocks on a slow client.

#pragma once

#include "context/editor/contract/json.h"
#include "context/kernel/event_bus.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace context::editor::bridge
{

// The `stability` field carried on diagnostics (R-BRIDGE-008). A diagnostic emitted while the
// derived world is still settling is `settling`; once quiescent it is `stable`; `unstable` marks a
// diagnostic the next derivation may invalidate.
enum class Stability
{
    stable,
    unstable,
    settling,
};

[[nodiscard]] const char* stability_name(Stability s);

// One event on the stream. `seq` is monotonic and totally ordered across the incarnation; clients
// resume with "since seq N".
struct Event
{
    std::uint64_t seq = 0;
    std::string incarnation_id;
    std::uint64_t generation = 0;
    std::string topic;
    contract::Json payload;

    // {seq, incarnationId, generation, topic, payload} — the wire shape.
    [[nodiscard]] contract::Json to_json() const;
};

// A client subscription with a BOUNDED queue. When the queue overflows the stream sets the gap flag
// and drops — the subscriber must re-snapshot (R-BRIDGE-008: the daemon never blocks on a slow
// client). Non-owning w.r.t. the stream; the caller keeps it alive for the subscription's lifetime.
class Subscriber
{
public:
    // `topics` empty => subscribe to every topic; otherwise only the listed topics. `capacity` is
    // the bounded queue depth.
    explicit Subscriber(std::vector<std::string> topics, std::size_t capacity = 64);

    [[nodiscard]] bool wants(const std::string& topic) const;
    // Pop all currently-queued events (oldest first), clearing the queue.
    [[nodiscard]] std::vector<Event> drain();
    // True once an overflow dropped at least one event since the last reset_gap(); the client must
    // re-snapshot and resume "since" its last delivered seq.
    [[nodiscard]] bool gap() const noexcept { return gap_; }
    void reset_gap() noexcept { gap_ = false; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::uint64_t last_delivered_seq() const noexcept { return last_seq_; }

private:
    friend class EventStream;
    void offer(const Event& e); // enqueue, or mark gap on overflow

    std::vector<std::string> topics_;
    std::size_t capacity_;
    std::deque<Event> queue_;
    bool gap_ = false;
    std::uint64_t last_seq_ = 0;
};

class EventStream
{
public:
    // Generates a fresh incarnation id (a new daemon lifetime).
    EventStream();
    // Deterministic ctor for tests: an explicit incarnation id + ring-buffer depth for "since seq N"
    // catch-up.
    explicit EventStream(std::string incarnation_id, std::size_t ring_capacity = 256);

    [[nodiscard]] const std::string& incarnation_id() const noexcept { return incarnation_id_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint64_t last_seq() const noexcept { return last_seq_; }

    // Publish `payload` on `topic`; returns the assigned seq. On the `diagnostics` topic a
    // `stability` field is always attached (defaulting to `stable` when none is supplied); on other
    // topics it is attached only when `stability` is provided.
    std::uint64_t publish(const std::string& topic, contract::Json payload,
                          std::optional<Stability> stability = std::nullopt);

    // Advance the derived-world generation counter and emit the `derivation.settled{generation}`
    // quiescence event on the `derivation` topic. Returns the new generation.
    std::uint64_t settle();

    // Forward a kernel-internal LogEvent onto this stream's `log` topic (kept a distinct stream).
    // Returns the assigned seq.
    std::uint64_t forward_log(const kernel::LogEvent& e);

    // Register / unregister a non-owning subscriber.
    void add_subscriber(Subscriber* sub);
    void remove_subscriber(Subscriber* sub);

    // Current-state snapshot a newly-attached or reconnecting client reads before deltas.
    [[nodiscard]] contract::Json snapshot() const;

    // Replay ring-buffered events with seq > `since`. Sets `gapped` when `since` predates the buffer
    // (the events between were evicted → the client must take a fresh snapshot instead).
    [[nodiscard]] std::vector<Event> replay_since(std::uint64_t since, bool& gapped) const;

private:
    std::uint64_t emit(const std::string& topic, contract::Json payload);

    std::string incarnation_id_;
    std::size_t ring_capacity_;
    std::uint64_t last_seq_ = 0;
    std::uint64_t generation_ = 0;
    std::deque<Event> ring_;
    std::vector<Subscriber*> subscribers_;
};

} // namespace context::editor::bridge
