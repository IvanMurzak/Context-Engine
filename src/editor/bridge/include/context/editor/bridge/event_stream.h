// Client-facing event stream (R-BRIDGE-008): the SEPARATE, post-derivation stream clients subscribe
// to — distinct from the kernel-internal EventBus (context::kernel, event_bus.h).
//
// The kernel EventBus is how RuntimeKernel systems talk to each other in-process; NOTHING on it is
// exposed to clients. This stream carries post-derivation FACTS to attached CLI/GUI/AI clients over
// the bridge. Every event carries a monotonic, totally-ordered `seq` and the current INCARNATION
// epoch id (so a client can tell one daemon lifetime from the next) plus the derived-world
// GENERATION counter. The core topics mirror the contract registry's advertised set
// (files/derivation/diagnostics/session/clients/log). settle(generation) ADOPTS the derived-world
// generation and emits the `derivation.settled{generation}` quiescence event; diagnostics carry a
// `stability` field; and the kernel `log` topic is FORWARDED here (kept a separate stream, never the
// same object). Slow subscribers get a bounded queue and, on overflow, an explicit gap marker
// instructing a re-snapshot — the stream never blocks on a slow client.

#pragma once

#include "context/editor/contract/json.h"
#include "context/kernel/event_bus.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace context::editor::bridge
{

// ------------------------------------- the package fact bus's bounds + refusals (editor-UX d2) ---
//
// ⚠ EVERY BOUND HERE EXISTS BECAUSE THIS SURFACE IS REACHABLE FROM UNTRUSTED CODE IN A PROCESS THAT
// STAYS UP FOR DAYS. A sandboxed package chooses what it declares and what it publishes; nothing
// about a bus with a per-topic retained value is self-limiting, so the registry, the topic name and
// the retained payload are each bounded rather than trusted to be reasonable — the identical
// reasoning `package_events.h` states for the per-package event buffer.

// How many package topics ONE daemon incarnation may hold declared. Sized to be obviously beyond
// what a machine's installed packages declare and obviously unable to exhaust memory: the retained
// values, not the names, are the weight, and those are bounded separately below.
inline constexpr std::size_t kMaxPackageTopics = 256;

// The longest declarable package topic name, in bytes.
inline constexpr std::size_t kMaxPackageTopicLength = 128;

// The largest RETAINED package-fact payload, measured on the canonical form the dedup compares.
// Matched to editor-core's own `PANEL_STATE_MAX_JSON_LENGTH` (64 KiB) so a package author meets ONE
// number for "how big may a blob I hand the editor be" rather than two that differ by a factor
// nobody can explain.
inline constexpr std::size_t kMaxPackageFactBytes = 64 * 1024;

// The refusal codes. DEFINED HERE and CATALOGUED in `contract/src/error_catalog.cpp` — the
// promote-a-local-string pattern `scope.denied` and the `install.*` codes already follow, which is
// what keeps this layer from depending on the catalog while the codes stay introspectable.
/** The topic is not a well-formed, namespaced package topic (grammar, length, or reserved). */
inline constexpr const char* kErrPackageTopicInvalid = "package.topic_invalid";
/** No package registered that topic on this daemon — deny-by-default, never publish-to-create. */
inline constexpr const char* kErrPackageTopicUndeclared = "package.topic_undeclared";
/** The package-topic registry is full (`kMaxPackageTopics`). */
inline constexpr const char* kErrPackageTopicCapacity = "package.topic_capacity";
/** The fact's canonical form exceeds `kMaxPackageFactBytes`; nothing is retained. */
inline constexpr const char* kErrPackageFactTooLarge = "package.fact_too_large";
/** D5 rule 3: a publish issued from INSIDE an event handler, refused so the loop is diagnosable. */
inline constexpr const char* kErrPackageFactReentrant = "package.fact_reentrant";

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
    // the bounded queue depth. `path_scope` (optional, R-BRIDGE-008 "optionally path-scoped"): when
    // non-empty, a path-bearing event is delivered only when its payload `path` is within the scope
    // subtree; pathless events (session/clients/log lifecycle) are not path-scoped facts and always
    // pass.
    explicit Subscriber(std::vector<std::string> topics, std::size_t capacity = 64,
                        std::string path_scope = std::string());

    // Topic-only filter (an empty topic list wants every topic).
    [[nodiscard]] bool wants(const std::string& topic) const;
    // The full delivery predicate: topic filter AND (when set) the path scope.
    [[nodiscard]] bool accepts(const Event& e) const;
    [[nodiscard]] const std::string& path_scope() const noexcept { return path_scope_; }
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
    std::string path_scope_;
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

    // ADOPT `derived_generation` as this stream's generation stamp, then emit the
    // `derivation.settled{generation}` quiescence event on the `derivation` topic. Returns the
    // assigned seq (like every other emitter here) — NOT the generation, which is the caller's own
    // input and needs no echo.
    //
    // ⚠ THE PARAMETER IS THE WHOLE POINT, and it is what turned this from a shape with no non-test
    // caller into the ONE settle publisher (M9 x9, CE #449). It used to `++generation_` — a counter
    // of its OWN, unrelated to the derived world — so `EditorKernel::settle` could not use it and
    // hand-rolled the same payload with the DERIVED-WORLD generation instead. The cost of that split
    // was not the duplication: it was that nothing ever advanced `generation_`, so the wire envelope
    // field the contract registry advertises as "the derived-world generation the event reflects"
    // (`describe`'s eventEnvelope) was permanently **0** on every event a live daemon ever pushed —
    // and the Shell's ProblemsFeed / SceneTreeFeed read their `on_derivation_settled(generation)`
    // stamp from THAT envelope, so the R-BRIDGE-008 stale-provisional discrimination was inert by
    // construction. Adopting the caller's generation here makes one number mean one thing on both
    // the envelope and the payload.
    //
    // PRECONDITION, and it is the caller's because adoption moved it there: `derived_generation` must
    // be NON-DECREASING across calls. It is assigned, not clamped — deliberately, since a `max()` here
    // would silently paper over a mis-ordered caller while still corrupting nothing visibly, and the
    // stream is not the component that can tell a rewind from a legitimate value. Today's one non-test
    // caller (`EditorKernel::settle` -> `DerivationGraph::generation()`) satisfies it by construction
    // and every settle is serialized under the server's single dispatch mutex. A regressing value
    // would rewind BOTH the envelope stamp on every later event and `snapshot()`'s R-BRIDGE-008
    // reconnect cursor, which is what makes this a precondition rather than a preference.
    std::uint64_t settle(std::uint64_t derived_generation);

    // Forward a kernel-internal LogEvent onto this stream's `log` topic (kept a distinct stream).
    // Returns the assigned seq.
    std::uint64_t forward_log(const kernel::LogEvent& e);

    // Register / unregister a non-owning subscriber. (The low-level live-delivery primitive; the
    // R-CLI-015 subscribe/unsubscribe/ack protocol below owns its Subscribers internally.)
    void add_subscriber(Subscriber* sub);
    void remove_subscriber(Subscriber* sub);

    // Current-state snapshot a newly-attached or reconnecting client reads before deltas.
    [[nodiscard]] contract::Json snapshot() const;

    // Replay ring-buffered events with seq > `since`. Sets `gapped` when `since` predates the buffer
    // (the events between were evicted → the client must take a fresh snapshot instead).
    [[nodiscard]] std::vector<Event> replay_since(std::uint64_t since, bool& gapped) const;

    // --- R-CLI-015 subscription protocol (subscribe / unsubscribe / ack) ------------------------
    // The concrete methods pinned in the versioned contract (R-CLI-004). `subscribe` mints a subId,
    // registers a bounded-queue Subscriber for live delivery, and returns the current-state snapshot
    // (snapshot-then-delta); an optional `since_seq` replays retained ring history for a reconnect
    // within THIS incarnation (gapped ⇒ that cursor predated retention → use the snapshot). `ack`
    // advances a subscription's cursor; `unsubscribe` drops it.
    //
    // Ring-buffer retention is defined RELATIVE TO THE SLOWEST ACKED cursor (R-CLI-015 is the single
    // normative home of this rule): the stream retains catch-up history until the slowest live
    // subscriber has acked past it, then ages it out — always bounded by ring_capacity, so an
    // over-slow subscriber never blocks the stream; it gets the gap-marker + re-snapshot on its next
    // replay instead. The seq a subscriber acks (and `since_seq`) is exactly the monotonic seq of the
    // R-CLI-012 / R-BRIDGE-008 unified cursor (its event form, with an empty keyset position) — this
    // protocol does NOT introduce a second cursor shape.
    struct SubscribeResult
    {
        std::string sub_id;         // the minted subscription id ("sub-<n>", unique to this stream)
        contract::Json snapshot;    // the current-state snapshot the client reads before deltas
        std::vector<Event> catchup; // replayed events with seq > since_seq (empty without since_seq)
        bool gapped = false;        // since_seq predated retained history — take the snapshot instead
    };

    [[nodiscard]] SubscribeResult subscribe(std::vector<std::string> topics,
                                            std::string path_scope = std::string(),
                                            std::optional<std::uint64_t> since_seq = std::nullopt,
                                            std::size_t capacity = 64);
    bool unsubscribe(const std::string& sub_id);
    bool ack(const std::string& sub_id, std::uint64_t seq);

    // Drain the events queued for one subscription (the transport delivers these over the wire).
    // Empty for an unknown subId.
    [[nodiscard]] std::vector<Event> poll(const std::string& sub_id);
    // True once a subscription's bounded queue overflowed — the client must re-snapshot and resume
    // "since" its last delivered seq (R-BRIDGE-008). False for an unknown subId.
    [[nodiscard]] bool sub_gapped(const std::string& sub_id) const;
    // Clear a subscription's overflow gap flag. The D19 push serve loop calls this once it has
    // enqueued the re-snapshot gap marker frame for the client, so the same overflow is not signaled
    // on every subsequent fan-out pass. No-op for an unknown subId.
    void reset_sub_gap(const std::string& sub_id);
    // The retention floor: the slowest acked cursor across live subscriptions (0 when none). Events
    // with seq <= this have been acked by EVERY live subscriber and may age out (R-CLI-015).
    [[nodiscard]] std::uint64_t slowest_acked_seq() const;
    [[nodiscard]] std::size_t subscription_count() const noexcept { return subscriptions_.size(); }

    // ===================== THE PACKAGE FACT BUS (editor-UX d2, D4 + D5) =======================
    //
    // WHAT THIS IS. The mechanism by which two independent packages exchange facts WITHOUT
    // depending on each other: package A publishes onto a topic it declared in its own manifest,
    // package B subscribes to it under an operator-consented grant, and neither ever names the
    // other's code. It lives HERE, on the daemon's client-facing stream, rather than on the
    // window-local `editor.ui` bus, because a fact on that bus is invisible to the CLI, to an agent
    // and to a second window (design 05 §3 "Why not the editor.ui bus"). Delivery is the SHIPPING
    // path, unchanged: an ordinary `subscribe` on the package's baseline daemon session, drained
    // into the Shell's bounded per-package buffer (`package_events.h`) and pushed into the frame by
    // `PackageEventPump`. No new transport.
    //
    // ⚠ D5 — A PACKAGE FACT IS A **STATE**, NOT AN EDGE, AND THAT IS THE CYCLE BREAKER. Two
    // packages hold SEPARATE baseline sessions and therefore DIFFERENT `origin`s, so the origin
    // echo suppression that protects `selection` / `camera` does nothing at all for an A -> B -> A
    // mirroring loop: every hop looks foreign to the next. The rule that actually breaks the cycle
    // is the OTHER one the session state already relies on — "a no-op publishes nothing" — which
    // needs daemon-side state to compare against, and an arbitrary package topic had none. So this
    // bus RETAINS the last value per topic and REFUSES a repeat: a mirroring pair converges after
    // exactly one round instead of spinning.
    //
    //   1. LAST-VALUE RETENTION + DEDUP. `publish_package_fact` answers `changed == false` and emits
    //      NOTHING — no seq, no ring entry, no subscriber delivery — when the canonical form of the
    //      payload equals the retained one. A refusal and a dedup are both indistinguishable, from
    //      every subscriber, from the publish never having happened (`EditorUiBus::publish`'s rule,
    //      one tier down).
    //   2. SNAPSHOT-ON-SUBSCRIBE FALLS OUT OF RETENTION. `snapshot()` carries `packageFacts`, so a
    //      panel mounted mid-session reads the current value in its `subscribe` reply instead of
    //      drawing nothing until the next change. That is the `editor.ui` bus's model, and it is
    //      what removes the subscribe-then-separately-ask race rather than documenting around it.
    //   3. REENTRANCY IS REFUSED, WITH A DIAGNOSTIC. See `publishing()` below.
    //
    // ⚠ THE ACCEPTED COST, STATED SO NOBODY "FIXES" IT: a package CANNOT send pure edge events.
    // "the button was pressed twice" is not expressible — the second publish deduplicates against
    // the first. Edge semantics must be modelled as state (a counter, a token). This was taken
    // knowingly as the price of a broadcast bus that cannot be made to loop.
    //
    // ⚠ WHAT THIS LAYER DOES **NOT** DECIDE: whether the CALLER was entitled to publish that topic.
    // The daemon has no manifests and no packages — the "topic is declared by THAT package" half of
    // D4 is enforced in the Shell (`package_facts.h`), which is the only component that has read
    // one. What this layer enforces is what it CAN know: the grammar (a package topic is dotted and
    // namespaced, and can therefore never collide with a contract-owned topic), the DECLARATION (a
    // topic nobody registered cannot be published on), and the BOUNDS. Two controls, independently,
    // in the discipline `package_sessions.h` control 2 states.

    // One `publish_package_fact` outcome. `accepted == false` carries the refusal; `accepted &&
    // !changed` is the D5 dedup — a SUCCESS that deliberately emitted nothing.
    struct PackageFactResult
    {
        bool accepted = false;
        bool changed = false;
        std::uint64_t seq = 0; // 0 when nothing was emitted (a refusal or a dedup)
        std::string error_code;
        std::string message;
    };

    // Why `topic` is not a publishable package topic, or `""` when it is. GRAMMAR ONLY — this
    // cannot know which package asked.
    //
    // The grammar is `is_segmented_name`'s (gui/contract/registry.cpp) with a length bound and a
    // MINIMUM OF TWO SEGMENTS, and the two-segment floor is the load-bearing half: every
    // contract-owned topic (`files`, `derivation`, `diagnostics`, `session`, `clients`, `log`) is a
    // single bare segment, so a package topic can never spell one. That is why this bus needs no
    // deny-list of core topic names — a list would have to be kept in step with
    // `Registry::topics()`, and the drift would read as a grant.
    [[nodiscard]] static std::string package_topic_defect(const std::string& topic);

    // REGISTER `topic` on this daemon's package-topic registry — D4's "topics registered at
    // install/load". Idempotent: re-declaring a live topic succeeds and changes nothing.
    //
    // False + `error` for a malformed topic or a full registry. Declaring confers NOTHING on its
    // own: a declared topic with no publish delivers nothing and appears in `describe` as a name.
    bool declare_package_topic(const std::string& topic, std::string& error);

    // Every declared package topic, sorted — what `describe` enumerates (R-CLI-013 parity) and what
    // an agent discovers a package's vocabulary from.
    [[nodiscard]] std::vector<std::string> package_topics() const;
    [[nodiscard]] bool is_package_topic_declared(const std::string& topic) const;

    // Publish a package FACT (D4/D5). Refused for an undeclared topic, an oversized payload, or a
    // reentrant call; deduplicated against the retained value; otherwise emitted on `topic` exactly
    // as any other event, so `subscribe`'s ordinary topic filter delivers it.
    [[nodiscard]] PackageFactResult publish_package_fact(const std::string& topic,
                                                         contract::Json payload);

    // The retained value for `topic`, or nullptr when nothing has been published on it. The
    // observable a "a topic with no publish yet delivers nothing" case asserts directly.
    [[nodiscard]] const contract::Json* retained_package_fact(const std::string& topic) const;

    // A SYNCHRONOUS in-process consumer of published events.
    //
    // ⚠ WHY THIS EXISTS AT ALL, STATED PLAINLY BECAUSE IT HAS NO PRODUCTION CONSUMER TODAY. D5's
    // third rule refuses a publish issued from inside an event handler, and a guard can only be
    // armed around something that RUNS a handler. Every consumer this stream has today is a QUEUE
    // (`Subscriber::offer`), so there is nothing in-process to re-enter and the guard would be
    // inert — which is precisely the trap: a reentrancy guard added AFTER the first in-process
    // consumer lands is a guard added after the loop it exists to prevent has already shipped. So
    // the seam is opened and the guard armed across it NOW, and the T1 tier is what drives the
    // re-entry. A listener that throws is not the stream's problem to diagnose and is left to
    // propagate — this is not a fan-out to untrusted code (a package reaches this stream over the
    // wire, never as a listener).
    using PublishListener = std::function<void(const Event&)>;
    void add_publish_listener(PublishListener listener);

    // Is a publish (any publish — a settle, a forwarded log, or a package fact) currently fanning
    // out on this thread? TRUE inside a `PublishListener`, which is what makes the D5 reentrancy
    // refusal a fact about the stream rather than a convention callers keep.
    [[nodiscard]] bool publishing() const noexcept { return publishing_; }

    /** Package facts REFUSED (grammar, undeclared, oversized, reentrant). Non-zero is a package bug. */
    [[nodiscard]] std::uint64_t package_facts_refused() const noexcept
    {
        return package_facts_refused_;
    }
    /** Package facts DEDUPLICATED — D5 rule 1 doing its work. Non-zero is the cycle breaker biting. */
    [[nodiscard]] std::uint64_t package_facts_deduplicated() const noexcept
    {
        return package_facts_deduplicated_;
    }

private:
    std::uint64_t emit(const std::string& topic, contract::Json payload);
    // Evict ring history that is either past the hard capacity OR already acked by every live
    // subscriber (seq <= slowest_acked_seq). The slowest-acked retention rule (R-CLI-015).
    void prune_ring();

    // One live subscription: its minted id, the owned live-delivery Subscriber, and its ack cursor.
    struct Subscription
    {
        std::string id;
        std::unique_ptr<Subscriber> sub;
        std::uint64_t acked_seq = 0;
    };

    std::string incarnation_id_;
    std::size_t ring_capacity_;
    std::uint64_t last_seq_ = 0;
    std::uint64_t generation_ = 0;
    std::deque<Event> ring_;
    std::vector<Subscriber*> subscribers_;
    std::vector<Subscription> subscriptions_;
    std::uint64_t sub_counter_ = 0;

    // --- the package fact bus's own state (D4/D5) -------------------------------------------------
    // One declared topic and, once something has been published on it, its retained value plus the
    // CANONICAL form the dedup compares. Holding the canonical string beside the payload is what
    // keeps a repeat publish O(payload) rather than re-canonicalising the retained side every time.
    struct PackageTopic
    {
        std::string name;
        bool has_value = false;
        contract::Json value;
        std::string canonical;
    };
    [[nodiscard]] PackageTopic* find_package_topic(const std::string& topic);
    [[nodiscard]] const PackageTopic* find_package_topic(const std::string& topic) const;

    // A vector, not a map: it is bounded by `kMaxPackageTopics`, the lookups are per-publish rather
    // than per-frame, and `package_topics()`' sort is then the only ordering decision in the file
    // (the same reasoning `PackageGrantStore::entries_` states).
    std::vector<PackageTopic> package_topics_;
    std::vector<PublishListener> publish_listeners_;
    bool publishing_ = false;
    std::uint64_t package_facts_refused_ = 0;
    std::uint64_t package_facts_deduplicated_ = 0;
};

} // namespace context::editor::bridge
