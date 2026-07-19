// The subscription consumer (R-CLI-015 / R-BRIDGE-008): the piece every LIVE client needs and no
// client should write twice.
//
// The daemon's half of this protocol shipped with the event stream; the CLIENT half — maintaining N
// topic subscriptions, applying snapshot-then-delta, keeping ack cursors, recovering from a gap, and
// surviving a daemon restart — did not exist until now (01 §2: "no client-side subscription
// helper"). This is it.
//
// The five behaviors it owns, each of which a hand-rolled client gets subtly wrong:
//   1. Snapshot-then-delta — `subscribe` returns the current-state snapshot plus (on a resume) the
//      replayed catch-up; deltas apply strictly AFTER the snapshot they follow.
//   2. Ack cursor management — the daemon's ring retention is defined relative to the SLOWEST acked
//      cursor, so a consumer that never acks silently pins the daemon's memory. Acks go out on a
//      cadence (every `ack_interval` applied events) and at every recovery point.
//   3. Gap -> automatic re-snapshot — an `event.gap` notification (or `gapped:true` on a resume)
//      means events were dropped; the ONLY correct recovery is a fresh snapshot, never "keep going".
//   4. Reconnect with backoff — a dropped connection re-dials on an exponential, bounded backoff and
//      resumes each subscription from its last delivered seq.
//   5. Incarnation epoch — a daemon restart mints a NEW incarnation id, which invalidates every
//      cursor (seqs restart). Detecting the change and taking a FRESH snapshot (no sinceSeq) is what
//      keeps a client from silently replaying a dead lifetime's cursor against a new one.

#pragma once

#include "context/editor/client/client.h"
#include "context/editor/contract/json.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace context::editor::client
{

// One event as a client sees it: the wire envelope {seq, incarnationId, generation, topic, payload}.
struct ClientEvent
{
    std::uint64_t seq = 0;
    std::string incarnation_id;
    std::uint64_t generation = 0;
    std::string topic;
    contract::Json payload;
};

// Parse one wire event envelope. nullopt when it is not an object carrying a numeric `seq`.
[[nodiscard]] std::optional<ClientEvent> parse_event(const contract::Json& envelope);

// Bounded exponential reconnect backoff. Pure + separately testable — a backoff whose growth is only
// observable by waiting is a backoff nobody tests.
struct BackoffPolicy
{
    int initial_ms = 50;
    int max_ms = 5000;
    int multiplier = 2;
    // Attempts before the consumer gives up and surfaces the failure. 0 => retry forever.
    int max_attempts = 6;

    // The delay before attempt `attempt` (0-based): initial * multiplier^attempt, clamped to max_ms.
    // A `multiplier` of 1 or less is a FIXED-delay policy (every attempt waits initial_ms, clamped)
    // rather than a degenerate 0 / never-growing one — stated here because it is a legitimate
    // configuration, not a misuse.
    [[nodiscard]] int delay_for_attempt(int attempt) const;
};

// What to subscribe to. An empty `topics` means every topic (the stream's own convention).
struct SubscriptionSpec
{
    std::vector<std::string> topics;
    std::string path_scope;
};

// The consumer's per-subscription cursor state. Exposed (read-only) so a test — or a diagnostic
// panel — can assert the protocol rather than infer it from side effects.
struct SubscriptionState
{
    SubscriptionSpec spec;
    std::string sub_id;
    std::uint64_t last_seq = 0;  // highest delivered seq (the resume cursor)
    std::uint64_t acked_seq = 0; // highest seq acked to the daemon
    contract::Json snapshot;     // the most recent snapshot this subscription rebuilt from
    bool live = false;
};

// Consumer tuning. At namespace scope (rather than nested in SubscriptionConsumer) so the class can
// default a constructor parameter to it — a nested struct's default member initializers are not
// available while its own enclosing class is still incomplete.
struct SubscriptionOptions
{
    // Ack after this many applied events (also acked at every snapshot/recovery point).
    std::uint64_t ack_interval = 16;
    // How long one pump() waits for an inbound frame before returning.
    int poll_timeout_ms = 50;
    int reconnect_timeout_ms = 3000;
    BackoffPolicy backoff;
};

// Observability counters — the protocol's behavior made assertable.
struct SubscriptionStats
{
    std::uint64_t events_applied = 0;
    std::uint64_t snapshots_taken = 0;
    std::uint64_t gaps_recovered = 0;
    std::uint64_t reconnects = 0;
    std::uint64_t acks_sent = 0;
    std::uint64_t incarnation_changes = 0;
};

class SubscriptionConsumer
{
public:
    using Options = SubscriptionOptions;
    using Stats = SubscriptionStats;

    using EventHandler = std::function<void(const std::string& sub_id, const ClientEvent& event)>;
    using SnapshotHandler =
        std::function<void(const std::string& sub_id, const contract::Json& snapshot)>;

    // Non-owning: `client` must outlive the consumer. `attach` is the handshake replayed on every
    // reconnect (the token + scopes the consumer re-presents).
    SubscriptionConsumer(Client& client, AttachOptions attach, Options options = Options());

    void on_event(EventHandler handler) { on_event_ = std::move(handler); }
    void on_snapshot(SnapshotHandler handler) { on_snapshot_ = std::move(handler); }

    // Register a subscription. Before start() it is merely recorded; after, it is established
    // immediately — and that subscribe CAN fail, so pass `error` if you care: on failure the
    // returned index's states()[i].live is false and `*error` says why. Returns its index into
    // states() either way.
    std::size_t add(SubscriptionSpec spec, std::string* error = nullptr);

    // Establish every registered subscription with a FRESH snapshot. false (+ `error`) if any
    // subscribe call fails.
    [[nodiscard]] bool start(std::string& error);

    // One pump: apply whatever arrived (deltas / gap markers), ack on cadence, and reconnect +
    // re-subscribe if the wire dropped. Returns false (+ `error`) only on an unrecoverable failure
    // (reconnect backoff exhausted, or a re-subscribe/ack the daemon REFUSED). A transport failure
    // on any path — the poll, an ack, a re-subscribe — is recoverable and routes into the
    // reconnect-with-backoff ladder instead. A quiet window is a successful, no-op pump.
    //
    // BLOCKING BOUND: normally bounded by options.poll_timeout_ms (50 ms by default). But when the
    // wire dropped, this call ALSO runs the reconnect ladder inline, so its worst case is
    // sum(backoff delays) + max_attempts * reconnect_timeout_ms — with the defaults, ~21 s. Drive
    // pump() off a thread that can afford that (never a UI thread that must stay responsive during
    // a daemon restart).
    //
    // THREAD SAFETY: neither SubscriptionConsumer nor the Client it rides is thread-safe. Confine
    // both to one thread, or serialize every call yourself.
    [[nodiscard]] bool pump(std::string& error);

    // Ack every subscription's current cursor now (flushing the cadence). Cheap and idempotent — a
    // subscription whose acked cursor is already current sends nothing.
    [[nodiscard]] bool flush_acks(std::string& error);

    // Drop every live subscription (best-effort `unsubscribe`); the consumer can be start()ed again.
    void stop();

    [[nodiscard]] const std::vector<SubscriptionState>& states() const noexcept { return states_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] const std::string& incarnation_id() const noexcept { return incarnation_id_; }

private:
    // Establish/re-establish ONE subscription. `since` empty => a fresh snapshot; set => a resume
    // that falls back to a fresh snapshot when the daemon reports `gapped`. When `state` is still
    // LIVE it is unsubscribed first (see unsubscribe_one) so re-subscribing never leaks the old
    // subId. `rejected_by_daemon`, when non-null, distinguishes a daemon refusal from a transport
    // failure — the caller reconnects on the latter.
    [[nodiscard]] bool subscribe_one(SubscriptionState& state, std::optional<std::uint64_t> since,
                                     std::string& error, bool* rejected_by_daemon = nullptr);
    // Drop `state`'s CURRENT daemon-side subscription, best-effort. Re-subscribing without this
    // leaks it: the daemon holds every minted subId until an explicit `unsubscribe`, fans each
    // inbound event out once PER subId, and pins its ring retention to the slowest acked cursor
    // across them — so a leaked subscription both multiplies wire traffic and defeats R-CLI-015
    // retention, feeding the very gaps that triggered the re-subscribe.
    void unsubscribe_one(SubscriptionState& state);
    // Re-snapshot EVERY subscription (the gap + incarnation-change recovery).
    [[nodiscard]] bool resnapshot_all(std::string& error, bool* rejected_by_daemon = nullptr);
    // Reconnect with backoff, then resume every subscription from its cursor.
    [[nodiscard]] bool reconnect_and_resume(std::string& error);
    // The consumer's central recovery policy, named once: a refusal the daemon actually sent is
    // unrecoverable (retrying cannot talk it out of saying no), while a transport failure is an
    // ordinary disconnect that belongs in the reconnect ladder.
    [[nodiscard]] bool recover_or_fail(bool rejected_by_daemon, std::string& error);
    // Apply one delivered event to its subscription's cursor + the handler.
    void apply_event(const std::string& sub_id, const ClientEvent& event);
    // Note the incarnation carried by an inbound snapshot/event; true when it CHANGED (a restart).
    bool note_incarnation(const std::string& incarnation_id);
    [[nodiscard]] bool ack_if_due(SubscriptionState& state, std::string& error, bool force,
                                  bool* rejected_by_daemon = nullptr);
    [[nodiscard]] SubscriptionState* find_state(const std::string& sub_id);

    Client& client_;
    AttachOptions attach_;
    Options options_;
    std::vector<SubscriptionState> states_;
    Stats stats_;
    std::string incarnation_id_;
    bool started_ = false;
    EventHandler on_event_;
    SnapshotHandler on_snapshot_;
};

} // namespace context::editor::client
