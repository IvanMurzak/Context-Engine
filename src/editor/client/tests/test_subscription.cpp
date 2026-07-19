// T1 — the subscription-consumer protocol over WIRE MOCKS (the live-daemon half is
// test_subscription_e2e.cpp). Every branch the SDK owns, happy / edge / failure:
//   1. snapshot-then-delta        — subscribe seeds the cursor from the snapshot; deltas apply after
//   2. ordering + replay guard    — an at-or-below-cursor seq is dropped, never double-applied
//   3. ack cursor management      — acks fire on cadence and on demand, carrying the right seq
//   4. gap -> re-snapshot         — an event.gap re-snapshots EVERY subscription
//   5. resume + gapped fallback   — sinceSeq resume applies catch-up; gapped:true falls back to snapshot
//   6. reconnect with backoff     — a dropped wire re-dials, re-attaches, resumes; backoff is bounded
//   7. incarnation epoch          — a new incarnation id invalidates cursors and forces a snapshot
//   8. failure paths              — reconnect exhaustion + a daemon-refused subscribe surface as errors

#include "context/editor/client/subscription.h"

#include "client_test.h"
#include "mock_channel.h"

#include <memory>
#include <string>
#include <vector>

using context::editor::client::AttachOptions;
using context::editor::client::BackoffPolicy;
using context::editor::client::Client;
using context::editor::client::ClientEvent;
using context::editor::client::SubscriptionConsumer;
using context::editor::client::SubscriptionSpec;
using context::editor::contract::Json;
using clientmock::make_event;
using clientmock::make_snapshot;
using clientmock::MockChannel;

namespace
{
constexpr const char* kIncarnationA = "inc-aaaa";
constexpr const char* kIncarnationB = "inc-bbbb";

// A consumer wired over a mock, with the attach + subscribe responders every test needs.
struct Harness
{
    MockChannel* mock = nullptr;
    std::unique_ptr<Client> client;
    std::unique_ptr<SubscriptionConsumer> consumer;
    std::vector<ClientEvent> seen;
    std::vector<Json> snapshots;
    std::string incarnation = kIncarnationA;
    std::uint64_t snapshot_last_seq = 0;
    int next_sub = 0;

    explicit Harness(SubscriptionConsumer::Options options = SubscriptionConsumer::Options())
    {
        auto owned = std::make_unique<MockChannel>();
        mock = owned.get();

        mock->on("attach",
                 [](const clientmock::Request&)
                 {
                     Json data = Json::object();
                     Json scopes = Json::array();
                     scopes.push_back(Json(std::string("read_query")));
                     data.set("scopes", std::move(scopes));
                     return MockChannel::ok_envelope(std::move(data));
                 });
        mock->on("subscribe",
                 [this](const clientmock::Request& r)
                 {
                     Json data = Json::object();
                     data.set("subId", Json("sub-" + std::to_string(++next_sub)));
                     data.set("snapshot", make_snapshot(incarnation, snapshot_last_seq));
                     if (r.params.contains("sinceSeq"))
                     {
                         data.set("gapped", Json(gapped_resume));
                         Json catchup = Json::array();
                         for (const Json& e : resume_catchup)
                             catchup.push_back(e);
                         data.set("catchup", std::move(catchup));
                     }
                     return MockChannel::ok_envelope(std::move(data));
                 });
        mock->on("ack",
                 [](const clientmock::Request& r)
                 {
                     Json data = Json::object();
                     data.set("subId", r.params.at("subId"));
                     data.set("ackedSeq", r.params.at("seq"));
                     return MockChannel::ok_envelope(std::move(data));
                 });

        client = std::make_unique<Client>(std::move(owned));
        AttachOptions attach;
        attach.token = "tok-1234";
        consumer = std::make_unique<SubscriptionConsumer>(*client, attach, options);
        consumer->on_event([this](const std::string&, const ClientEvent& e) { seen.push_back(e); });
        consumer->on_snapshot([this](const std::string&, const Json& s) { snapshots.push_back(s); });
    }

    bool gapped_resume = false;
    std::vector<Json> resume_catchup;

    // The sub id the daemon minted for subscription `index`.
    [[nodiscard]] std::string sub_id(std::size_t index) const
    {
        return consumer->states()[index].sub_id;
    }
};

// --- 1. snapshot-then-delta -----------------------------------------------------------------------
void test_snapshot_then_delta()
{
    Harness h;
    h.snapshot_last_seq = 10;
    h.consumer->add(SubscriptionSpec{{"files"}, ""});

    std::string error;
    CHECK(h.consumer->start(error));
    CHECK(error.empty());
    CHECK(h.consumer->stats().snapshots_taken == 1);
    CHECK(h.snapshots.size() == 1);
    // The snapshot seeds the cursor: everything at or before lastSeq is already represented by it.
    CHECK(h.consumer->states()[0].last_seq == 10);
    CHECK(h.consumer->states()[0].acked_seq == 10);
    CHECK(h.consumer->incarnation_id() == kIncarnationA);

    // A delta AFTER the snapshot applies and advances the cursor.
    h.mock->push_event(h.sub_id(0), make_event(11, kIncarnationA, "files"));
    CHECK(h.consumer->pump(error));
    CHECK(h.seen.size() == 1);
    CHECK(h.seen[0].seq == 11);
    CHECK(h.seen[0].topic == "files");
    CHECK(h.consumer->states()[0].last_seq == 11);
}

// --- 2. ordering + replay guard -------------------------------------------------------------------
void test_replay_below_cursor_is_dropped()
{
    Harness h;
    h.snapshot_last_seq = 10;
    h.consumer->add(SubscriptionSpec{{"files"}, ""});
    std::string error;
    CHECK(h.consumer->start(error));

    // seq 9 and seq 10 are at/below the snapshot cursor — the snapshot already represents them.
    h.mock->push_event(h.sub_id(0), make_event(9, kIncarnationA, "files"));
    h.mock->push_event(h.sub_id(0), make_event(10, kIncarnationA, "files"));
    h.mock->push_event(h.sub_id(0), make_event(12, kIncarnationA, "files"));
    for (int i = 0; i < 3; ++i)
        CHECK(h.consumer->pump(error));

    CHECK(h.seen.size() == 1);
    CHECK(h.seen[0].seq == 12);
    CHECK(h.consumer->stats().events_applied == 1);
}

// --- 3. ack cursor management ---------------------------------------------------------------------
void test_ack_cadence_and_flush()
{
    SubscriptionConsumer::Options options;
    options.ack_interval = 3;
    Harness h(options);
    h.consumer->add(SubscriptionSpec{{"files"}, ""});
    std::string error;
    CHECK(h.consumer->start(error));

    // Two events: below the cadence, so no ack yet.
    for (std::uint64_t seq = 1; seq <= 2; ++seq)
        h.mock->push_event(h.sub_id(0), make_event(seq, kIncarnationA, "files"));
    for (int i = 0; i < 2; ++i)
        CHECK(h.consumer->pump(error));
    CHECK(h.mock->requests_for("ack").empty());

    // The third crosses the cadence -> one ack carrying the current cursor.
    h.mock->push_event(h.sub_id(0), make_event(3, kIncarnationA, "files"));
    CHECK(h.consumer->pump(error));
    const std::vector<clientmock::Request> acks = h.mock->requests_for("ack");
    CHECK(acks.size() == 1);
    CHECK(acks[0].params.at("seq").as_int() == 3);
    CHECK(acks[0].params.at("subId").as_string() == h.sub_id(0));
    CHECK(h.consumer->states()[0].acked_seq == 3);

    // A sub-cadence remainder still flushes on demand (the retention floor must not stall).
    h.mock->push_event(h.sub_id(0), make_event(4, kIncarnationA, "files"));
    CHECK(h.consumer->pump(error));
    CHECK(h.mock->requests_for("ack").size() == 1);
    CHECK(h.consumer->flush_acks(error));
    CHECK(h.mock->requests_for("ack").size() == 2);
    CHECK(h.mock->requests_for("ack")[1].params.at("seq").as_int() == 4);

    // flush_acks is idempotent — an already-current cursor sends nothing.
    CHECK(h.consumer->flush_acks(error));
    CHECK(h.mock->requests_for("ack").size() == 2);
}

// --- 4. gap -> automatic re-snapshot ---------------------------------------------------------------
void test_gap_triggers_resnapshot_of_every_subscription()
{
    Harness h;
    h.snapshot_last_seq = 5;
    h.consumer->add(SubscriptionSpec{{"files"}, ""});
    h.consumer->add(SubscriptionSpec{{"diagnostics"}, ""});
    std::string error;
    CHECK(h.consumer->start(error));
    CHECK(h.consumer->stats().snapshots_taken == 2);
    const std::size_t subscribes_before = h.mock->requests_for("subscribe").size();

    // The daemon dropped events for this connection: the cursor is untrustworthy for EVERY
    // subscription, so both must re-snapshot.
    h.snapshot_last_seq = 42;
    h.mock->push_gap();
    CHECK(h.consumer->pump(error));

    CHECK(h.mock->requests_for("subscribe").size() == subscribes_before + 2);
    CHECK(h.consumer->stats().gaps_recovered >= 1);
    CHECK(h.consumer->stats().snapshots_taken == 4);
    CHECK(h.consumer->states()[0].last_seq == 42);
    CHECK(h.consumer->states()[1].last_seq == 42);
    // A re-snapshot must NOT carry a sinceSeq — that is what makes it a fresh snapshot.
    const std::vector<clientmock::Request> subs = h.mock->requests_for("subscribe");
    CHECK(!subs.back().params.contains("sinceSeq"));
}

// The re-snapshot happens on a STILL-LIVE connection, so it must RELEASE each old subscription
// before minting its replacement. Counting subscribes alone cannot see the difference: the daemon
// retains every minted subId until an explicit unsubscribe, fans each event out once per subId, and
// pins ring retention to the slowest cursor across them — so a leak here multiplies wire traffic and
// manufactures the next gap. Assert the release explicitly, by id.
void test_gap_resnapshot_releases_the_old_subscriptions()
{
    Harness h;
    h.snapshot_last_seq = 5;
    h.consumer->add(SubscriptionSpec{{"files"}, ""});
    h.consumer->add(SubscriptionSpec{{"diagnostics"}, ""});
    std::string error;
    CHECK(h.consumer->start(error));
    const std::string first_sub = h.sub_id(0);
    const std::string second_sub = h.sub_id(1);
    CHECK(h.mock->requests_for("unsubscribe").empty());

    h.mock->push_gap();
    CHECK(h.consumer->pump(error));

    // Exactly one release per subscription, each naming the id it retired.
    const std::vector<clientmock::Request> drops = h.mock->requests_for("unsubscribe");
    CHECK(drops.size() == 2);
    CHECK(drops[0].params.at("subId").as_string() == first_sub);
    CHECK(drops[1].params.at("subId").as_string() == second_sub);
    // And the consumer moved onto the freshly minted ids, not the retired ones.
    CHECK(h.sub_id(0) != first_sub);
    CHECK(h.sub_id(1) != second_sub);
}

// A reconnect resumes against a connection whose session (and every subscription in it) already died
// with the old wire, so it must NOT spend an unsubscribe round-trip on ids the daemon has forgotten.
void test_reconnect_does_not_unsubscribe_dead_subscriptions()
{
    SubscriptionConsumer::Options options;
    options.backoff.initial_ms = 1;
    Harness h(options);
    h.consumer->add(SubscriptionSpec{{"files"}, ""});
    std::string error;
    CHECK(h.consumer->start(error));

    h.mock->break_connection();
    CHECK(h.consumer->pump(error));

    CHECK(h.consumer->stats().reconnects == 1);
    CHECK(h.mock->requests_for("unsubscribe").empty());
}

// --- 5. resume + gapped fallback -------------------------------------------------------------------
void test_resume_applies_catchup()
{
    Harness h;
    h.snapshot_last_seq = 7;
    h.consumer->add(SubscriptionSpec{{"files"}, ""});
    std::string error;
    CHECK(h.consumer->start(error));
    const std::size_t snapshots_after_start = h.consumer->stats().snapshots_taken;

    // Arrange the resume: the daemon still retains our history and replays it.
    h.resume_catchup = {make_event(8, kIncarnationA, "files"),
                        make_event(9, kIncarnationA, "files")};
    h.gapped_resume = false;
    h.mock->break_connection();
    CHECK(h.consumer->pump(error)); // sees the disconnect -> reconnect + resume

    CHECK(h.consumer->stats().reconnects == 1);
    // A successful resume replays catch-up rather than taking a new snapshot.
    CHECK(h.consumer->stats().snapshots_taken == snapshots_after_start);
    CHECK(h.seen.size() == 2);
    CHECK(h.seen[0].seq == 8);
    CHECK(h.seen[1].seq == 9);
    CHECK(h.consumer->states()[0].last_seq == 9);
}

void test_gapped_resume_falls_back_to_snapshot()
{
    Harness h;
    h.snapshot_last_seq = 7;
    h.consumer->add(SubscriptionSpec{{"files"}, ""});
    std::string error;
    CHECK(h.consumer->start(error));
    const std::size_t snapshots_after_start = h.consumer->stats().snapshots_taken;

    // The daemon's retained history no longer covers our cursor: gapped -> the snapshot IS the
    // recovery, and the catch-up must be ignored rather than applied on top of it.
    h.gapped_resume = true;
    h.resume_catchup = {make_event(8, kIncarnationA, "files")};
    h.snapshot_last_seq = 99;
    h.mock->break_connection();
    CHECK(h.consumer->pump(error));

    CHECK(h.consumer->stats().snapshots_taken == snapshots_after_start + 1);
    CHECK(h.consumer->stats().gaps_recovered >= 1);
    CHECK(h.seen.empty()); // the stale catch-up was NOT applied
    CHECK(h.consumer->states()[0].last_seq == 99);
}

// --- 6. reconnect with backoff ---------------------------------------------------------------------
void test_backoff_policy_is_bounded_and_exponential()
{
    BackoffPolicy policy;
    policy.initial_ms = 10;
    policy.multiplier = 2;
    policy.max_ms = 100;
    CHECK(policy.delay_for_attempt(0) == 10);
    CHECK(policy.delay_for_attempt(1) == 20);
    CHECK(policy.delay_for_attempt(2) == 40);
    CHECK(policy.delay_for_attempt(3) == 80);
    CHECK(policy.delay_for_attempt(4) == 100); // clamped
    CHECK(policy.delay_for_attempt(50) == 100);
}

// A multiplier of 1 is a legitimate FIXED-delay policy, not a misconfiguration — it must answer
// initial_ms at every attempt (and stay clamped by max_ms) rather than degenerate.
void test_fixed_delay_backoff_policy()
{
    BackoffPolicy fixed;
    fixed.initial_ms = 25;
    fixed.multiplier = 1;
    fixed.max_ms = 100;
    CHECK(fixed.delay_for_attempt(0) == 25);
    CHECK(fixed.delay_for_attempt(1) == 25);
    CHECK(fixed.delay_for_attempt(1000000) == 25); // and answers in O(1), not O(attempt)

    // max_ms is the ceiling even when initial_ms is above it.
    BackoffPolicy clamped;
    clamped.initial_ms = 500;
    clamped.multiplier = 1;
    clamped.max_ms = 100;
    CHECK(clamped.delay_for_attempt(0) == 100);
    CHECK(clamped.delay_for_attempt(7) == 100);
}

void test_reconnect_retries_then_succeeds()
{
    SubscriptionConsumer::Options options;
    options.backoff.initial_ms = 1;
    options.backoff.max_ms = 2;
    options.backoff.max_attempts = 5;
    Harness h(options);
    h.consumer->add(SubscriptionSpec{{"files"}, ""});
    std::string error;
    CHECK(h.consumer->start(error));

    h.mock->set_failed_reconnects(2); // two refusals, then the daemon is back
    h.mock->break_connection();
    CHECK(h.consumer->pump(error));

    CHECK(h.mock->reconnect_attempts() == 3);
    CHECK(h.consumer->stats().reconnects == 1);
    // The re-attach replays the handshake (with the token) on the fresh connection.
    CHECK(h.mock->requests_for("attach").size() == 1);
}

void test_reconnect_exhaustion_is_an_error()
{
    SubscriptionConsumer::Options options;
    options.backoff.initial_ms = 1;
    options.backoff.max_ms = 1;
    options.backoff.max_attempts = 3;
    Harness h(options);
    h.consumer->add(SubscriptionSpec{{"files"}, ""});
    std::string error;
    CHECK(h.consumer->start(error));

    h.mock->set_failed_reconnects(99); // never comes back
    h.mock->break_connection();
    CHECK(!h.consumer->pump(error));
    CHECK(!error.empty());
    CHECK(h.mock->reconnect_attempts() == 3);
}

// --- 7. incarnation epoch --------------------------------------------------------------------------
void test_incarnation_change_forces_fresh_snapshot()
{
    Harness h;
    h.snapshot_last_seq = 20;
    h.consumer->add(SubscriptionSpec{{"files"}, ""});
    std::string error;
    CHECK(h.consumer->start(error));
    CHECK(h.consumer->incarnation_id() == kIncarnationA);
    const std::size_t snapshots_after_start = h.consumer->stats().snapshots_taken;

    // The daemon restarted: a NEW incarnation, and its seq space restarted from 1. Comparing seq 1
    // against our seq-20 cursor would silently swallow the new lifetime's whole event stream.
    h.incarnation = kIncarnationB;
    h.snapshot_last_seq = 0;
    h.mock->push_event(h.sub_id(0), make_event(1, kIncarnationB, "files"));
    CHECK(h.consumer->pump(error));

    CHECK(h.consumer->stats().incarnation_changes == 1);
    CHECK(h.consumer->stats().snapshots_taken == snapshots_after_start + 1);
    CHECK(h.consumer->incarnation_id() == kIncarnationB);
    CHECK(h.consumer->states()[0].last_seq == 0); // the cursor was reset, not carried across epochs
}

void test_incarnation_change_across_reconnect_resnapshots()
{
    SubscriptionConsumer::Options options;
    options.backoff.initial_ms = 1;
    Harness h(options);
    h.snapshot_last_seq = 20;
    h.consumer->add(SubscriptionSpec{{"files"}, ""});
    std::string error;
    CHECK(h.consumer->start(error));

    // The reconnect lands on a RESTARTED daemon — the resume must not be trusted.
    h.incarnation = kIncarnationB;
    h.snapshot_last_seq = 3;
    h.resume_catchup = {make_event(21, kIncarnationB, "files")};
    h.mock->break_connection();
    CHECK(h.consumer->pump(error));

    CHECK(h.consumer->stats().incarnation_changes == 1);
    CHECK(h.consumer->incarnation_id() == kIncarnationB);
    CHECK(h.consumer->states()[0].last_seq == 3); // from the fresh snapshot, not the resume cursor
}

// --- 8. failure path: a refused subscribe ----------------------------------------------------------
void test_refused_subscribe_surfaces_error()
{
    Harness h;
    h.mock->on("subscribe",
               [](const clientmock::Request&)
               {
                   Json data = Json::object();
                   return MockChannel::ok_envelope(std::move(data)); // no subId
               });
    h.consumer->add(SubscriptionSpec{{"files"}, ""});
    std::string error;
    CHECK(!h.consumer->start(error));
    CHECK(!error.empty());
}

// A daemon-side refusal IS unrecoverable — reconnecting cannot talk it out of saying no.
void test_refused_ack_is_unrecoverable()
{
    SubscriptionConsumer::Options options;
    options.ack_interval = 1;
    options.backoff.initial_ms = 1;
    Harness h(options);
    h.consumer->add(SubscriptionSpec{{"files"}, ""});
    std::string error;
    CHECK(h.consumer->start(error));

    h.mock->fail_method("ack", "subscription.unknown_sub");
    h.mock->push_event(h.sub_id(0), make_event(1, kIncarnationA, "files"));
    CHECK(!h.consumer->pump(error));
    CHECK(!error.empty());
    CHECK(h.consumer->stats().reconnects == 0); // NOT retried — the daemon answered
}

// ...but a TRANSPORT failure on the ack is an ordinary disconnect and must route into the reconnect
// ladder. poll_event() reports `disconnected` only when IT touched the wire — a pump that consumed a
// frame the peer had already queued sees a healthy poll and then dies on the ack, so the ack path is
// genuinely where the death surfaces.
void test_transport_failure_on_ack_reconnects()
{
    SubscriptionConsumer::Options options;
    options.ack_interval = 1;
    options.backoff.initial_ms = 1;
    Harness h(options);
    h.consumer->add(SubscriptionSpec{{"files"}, ""});
    std::string error;
    CHECK(h.consumer->start(error));

    // The event is already buffered, so the poll succeeds; the wire dies before the ack goes out.
    h.mock->push_event(h.sub_id(0), make_event(1, kIncarnationA, "files"));
    h.mock->break_connection();

    CHECK(h.consumer->pump(error));
    CHECK(h.consumer->stats().events_applied == 1);
    CHECK(h.consumer->stats().reconnects == 1);
    CHECK(h.mock->reconnect_attempts() == 1);
}

// add() after start() establishes immediately — and that subscribe can be refused. The failure must
// reach the caller instead of leaving a silently dead subscription in states().
void test_add_after_start_reports_a_refused_subscribe()
{
    Harness h;
    h.consumer->add(SubscriptionSpec{{"files"}, ""});
    std::string error;
    CHECK(h.consumer->start(error));

    h.mock->fail_method("subscribe", "daemon refused");
    std::string add_error;
    const std::size_t index = h.consumer->add(SubscriptionSpec{{"log"}, ""}, &add_error);
    CHECK(!add_error.empty());
    CHECK(!h.consumer->states()[index].live);
}

// --- unsubscribe on stop ---------------------------------------------------------------------------
void test_stop_unsubscribes_every_subscription()
{
    Harness h;
    h.consumer->add(SubscriptionSpec{{"files"}, ""});
    h.consumer->add(SubscriptionSpec{{"log"}, ""});
    std::string error;
    CHECK(h.consumer->start(error));
    h.consumer->stop();
    CHECK(h.mock->requests_for("unsubscribe").size() == 2);
}
} // namespace

int main()
{
    test_snapshot_then_delta();
    test_replay_below_cursor_is_dropped();
    test_ack_cadence_and_flush();
    test_gap_triggers_resnapshot_of_every_subscription();
    test_gap_resnapshot_releases_the_old_subscriptions();
    test_reconnect_does_not_unsubscribe_dead_subscriptions();
    test_resume_applies_catchup();
    test_gapped_resume_falls_back_to_snapshot();
    test_backoff_policy_is_bounded_and_exponential();
    test_fixed_delay_backoff_policy();
    test_reconnect_retries_then_succeeds();
    test_reconnect_exhaustion_is_an_error();
    test_incarnation_change_forces_fresh_snapshot();
    test_incarnation_change_across_reconnect_resnapshots();
    test_refused_subscribe_surfaces_error();
    test_refused_ack_is_unrecoverable();
    test_transport_failure_on_ack_reconnects();
    test_add_after_start_reports_a_refused_subscribe();
    test_stop_unsubscribes_every_subscription();
    CLIENT_TEST_MAIN_END();
}
