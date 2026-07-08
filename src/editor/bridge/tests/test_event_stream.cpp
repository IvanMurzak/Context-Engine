// Event-stream unit tests (R-BRIDGE-008): incarnation epoch + monotonic seq, the generation counter
// + derivation.settled quiescence event, the stability field on diagnostics, log forwarding,
// bounded-queue subscribers with gap markers, "since seq N" replay, and the snapshot.
//
// Plus the R-CLI-015 subscription protocol (issue #98): subscribe → snapshot + subId (snapshot-then-
// delta), ack advancing the cursor, slowest-acked ring retention (a slow subscriber pins history, a
// fast one does not), gap-marker + re-snapshot on overflow, unsubscribe raising the retention floor,
// sinceSeq catch-up + gap fallback, and path-scoped delivery. (R-QA-013: happy + failure paths.)

#include "context/editor/bridge/event_stream.h"

#include "bridge_test.h"

#include <optional>
#include <string>

using namespace context::editor::bridge;
using context::editor::contract::Json;

int main()
{
    // --- incarnation epoch + monotonic, totally-ordered seq -------------------------------------
    {
        EventStream s("inc-test");
        CHECK(s.incarnation_id() == "inc-test");
        CHECK(s.last_seq() == 0);
        const std::uint64_t a = s.publish("files", Json::object());
        const std::uint64_t b = s.publish("files", Json::object());
        CHECK(a == 1);
        CHECK(b == 2); // strictly increasing
        CHECK(s.last_seq() == 2);
    }

    // --- generation counter + derivation.settled{generation} quiescence event -------------------
    {
        EventStream s("inc-test");
        CHECK(s.generation() == 0);
        Subscriber sub({"derivation"}, 8);
        s.add_subscriber(&sub);
        const std::uint64_t g1 = s.settle();
        CHECK(g1 == 1);
        CHECK(s.generation() == 1);
        const std::uint64_t g2 = s.settle();
        CHECK(g2 == 2);

        auto events = sub.drain();
        CHECK(events.size() == 2);
        CHECK(events[0].topic == "derivation");
        CHECK(events[0].payload.at("event").as_string() == "derivation.settled");
        CHECK(events[0].payload.at("generation").as_int() == 1);
        CHECK(events[0].generation == 1); // the event is stamped with the post-settle generation
        CHECK(events[1].payload.at("generation").as_int() == 2);
        CHECK(events[0].incarnation_id == "inc-test");
    }

    // --- stability field on diagnostics ---------------------------------------------------------
    {
        EventStream s("inc-test");
        Subscriber sub({}, 16); // all topics
        s.add_subscriber(&sub);

        s.publish("diagnostics", Json::object());                          // default stability
        s.publish("diagnostics", Json::object(), Stability::settling);     // explicit
        s.publish("files", Json::object());                               // no stability field
        s.publish("files", Json::object(), Stability::unstable);          // explicit on non-diag

        auto events = sub.drain();
        CHECK(events.size() == 4);
        CHECK(events[0].payload.contains("stability"));
        CHECK(events[0].payload.at("stability").as_string() == "stable"); // diagnostics default
        CHECK(events[1].payload.at("stability").as_string() == "settling");
        CHECK(!events[2].payload.contains("stability"));                  // files: none supplied
        CHECK(events[3].payload.at("stability").as_string() == "unstable");
    }

    // --- forwarding the kernel `log` topic onto the bridge `log` topic (distinct streams) --------
    {
        EventStream s("inc-test");
        Subscriber sub({"log"}, 8);
        s.add_subscriber(&sub);
        s.forward_log(context::kernel::LogEvent{context::kernel::LogLevel::warn, "disk almost full"});
        auto events = sub.drain();
        CHECK(events.size() == 1);
        CHECK(events[0].topic == "log");
        CHECK(events[0].payload.at("level").as_string() == "warn");
        CHECK(events[0].payload.at("message").as_string() == "disk almost full");
    }

    // --- subscriber topic filtering -------------------------------------------------------------
    {
        EventStream s("inc-test");
        Subscriber only_files({"files"}, 8);
        s.add_subscriber(&only_files);
        s.publish("files", Json::object());
        s.publish("session", Json::object()); // filtered out
        auto events = only_files.drain();
        CHECK(events.size() == 1);
        CHECK(events[0].topic == "files");
        CHECK(only_files.drain().empty()); // drain clears the queue
    }

    // --- FAILURE PATH: bounded queue overflow -> gap marker (never block a slow client) ----------
    {
        EventStream s("inc-test");
        Subscriber slow({"files"}, 2); // capacity 2
        s.add_subscriber(&slow);
        s.publish("files", Json::object());
        s.publish("files", Json::object());
        CHECK(!slow.gap());
        s.publish("files", Json::object()); // overflow — dropped
        CHECK(slow.gap());                   // the client must re-snapshot
        auto events = slow.drain();
        CHECK(events.size() == 2); // only the two that fit
        slow.reset_gap();
        CHECK(!slow.gap());
    }

    // --- "since seq N" replay + gap fallback ----------------------------------------------------
    {
        EventStream s("inc-test", /*ring_capacity=*/3);
        for (int i = 0; i < 5; ++i)
            s.publish("files", Json::object()); // seq 1..5; ring holds only the last 3 (3,4,5)

        bool gapped = false;
        auto recent = s.replay_since(4, gapped); // want > 4 => seq 5
        CHECK(!gapped);
        CHECK(recent.size() == 1);
        CHECK(recent[0].seq == 5);

        auto stale = s.replay_since(1, gapped); // seq 2 was evicted — fresh snapshot needed
        CHECK(gapped);
    }

    // --- snapshot -------------------------------------------------------------------------------
    {
        EventStream s("inc-test");
        s.publish("files", Json::object());
        s.settle();
        const Json snap = s.snapshot();
        CHECK(snap.at("incarnationId").as_string() == "inc-test");
        CHECK(snap.at("generation").as_int() == 1);
        CHECK(snap.at("lastSeq").as_int() == 2);
    }

    // --- distinct incarnations differ -----------------------------------------------------------
    {
        EventStream a;
        EventStream b;
        CHECK(a.incarnation_id() != b.incarnation_id());
    }

    // ============================================================================================
    // R-CLI-015 subscription protocol (subscribe / unsubscribe / ack + slowest-acked retention)
    // ============================================================================================

    // --- subscribe: snapshot + subId, then snapshot-then-delta ordering -------------------------
    {
        EventStream s("inc-sub");
        s.publish("files", Json::object()); // seq 1, BEFORE subscribe
        auto r = s.subscribe({"files"});
        CHECK(!r.sub_id.empty());
        CHECK(r.snapshot.at("incarnationId").as_string() == "inc-sub");
        CHECK(r.snapshot.at("lastSeq").as_int() == 1); // the snapshot reflects state through seq 1
        CHECK(r.catchup.empty());                      // no sinceSeq => snapshot-only
        CHECK(!r.gapped);
        CHECK(s.subscription_count() == 1);

        // Deltas after subscribe are delivered in order, all with seq strictly after the snapshot.
        s.publish("files", Json::object());   // seq 2
        s.publish("session", Json::object()); // seq 3 — filtered out (files-only subscription)
        s.publish("files", Json::object());   // seq 4
        auto deltas = s.poll(r.sub_id);
        CHECK(deltas.size() == 2);
        CHECK(deltas[0].seq == 2); // strictly after the snapshot's lastSeq (1)
        CHECK(deltas[1].seq == 4); // the session event (3) was topic-filtered
        CHECK(s.poll(r.sub_id).empty()); // poll drains the queue
    }

    // --- ack advances the subscription cursor (monotonic; unknown sub fails) ---------------------
    {
        EventStream s("inc-ack");
        auto r = s.subscribe({}); // all topics; subscribed at seq 0
        CHECK(s.slowest_acked_seq() == 0);
        s.publish("files", Json::object()); // seq 1
        s.publish("files", Json::object()); // seq 2
        CHECK(s.ack(r.sub_id, 2));
        CHECK(s.slowest_acked_seq() == 2);
        CHECK(!s.ack("sub-does-not-exist", 1)); // FAILURE PATH: unknown subId
        CHECK(s.ack(r.sub_id, 1));               // a stale/duplicate ack never rewinds the cursor
        CHECK(s.slowest_acked_seq() == 2);
    }

    // --- slowest-acked retention: a slow subscriber pins history, a fast one does not ------------
    {
        EventStream s("inc-ret", /*ring_capacity=*/100); // generous cap => eviction is ack-driven
        auto fast = s.subscribe({});
        auto slow = s.subscribe({});
        for (int i = 0; i < 5; ++i)
            s.publish("files", Json::object()); // seq 1..5

        // fast acks to 5, slow stuck at 2 => retention floor = 2.
        CHECK(s.ack(fast.sub_id, 5));
        CHECK(s.ack(slow.sub_id, 2));
        CHECK(s.slowest_acked_seq() == 2);

        // 3,4,5 are PINNED for the slow subscriber's catch-up; 1,2 (acked by all) aged out.
        bool gapped = false;
        auto caught = s.replay_since(2, gapped);
        CHECK(!gapped);
        CHECK(caught.size() == 3);
        CHECK(caught.front().seq == 3);
        (void)s.replay_since(1, gapped); // resume before the pinned window => gap => re-snapshot
        CHECK(gapped);

        // Once the slow subscriber catches up (acks past 5), ALL that history ages out.
        CHECK(s.ack(slow.sub_id, 5));
        CHECK(s.slowest_acked_seq() == 5);
        bool g2 = false;
        CHECK(s.replay_since(2, g2).empty()); // 3,4,5 released — no live subscriber still needs them
        CHECK(s.replay_since(4, g2).empty());
    }

    // --- FAILURE PATH: bounded per-subscription queue overflow -> gap marker + re-snapshot -------
    {
        EventStream s("inc-gap");
        auto r = s.subscribe({"files"}, /*path_scope=*/std::string(), /*since_seq=*/std::nullopt,
                             /*capacity=*/2);
        s.publish("files", Json::object()); // queued (1/2)
        s.publish("files", Json::object()); // queued (2/2, full)
        CHECK(!s.sub_gapped(r.sub_id));
        s.publish("files", Json::object()); // overflow — dropped, gap set
        CHECK(s.sub_gapped(r.sub_id));       // the daemon never blocks; the client must re-snapshot
        auto got = s.poll(r.sub_id);
        CHECK(got.size() == 2); // only the two that fit
        // Re-snapshot path: after a gap, the client reads a fresh snapshot to resync.
        CHECK(s.snapshot().at("lastSeq").as_int() == 3);
    }

    // --- unsubscribe removes the subscription and raises the retention floor ---------------------
    {
        EventStream s("inc-unsub", 100);
        auto keep = s.subscribe({});
        auto slow = s.subscribe({});
        for (int i = 0; i < 3; ++i)
            s.publish("files", Json::object()); // 1..3
        CHECK(s.ack(keep.sub_id, 3));
        CHECK(s.slowest_acked_seq() == 0); // the un-acked slow subscriber pins everything
        CHECK(s.subscription_count() == 2);

        CHECK(s.unsubscribe(slow.sub_id)); // drop the slow one
        CHECK(s.subscription_count() == 1);
        CHECK(s.slowest_acked_seq() == 3); // floor rises to keep's cursor; history can age out
        bool g = false;
        CHECK(s.replay_since(2, g).empty());
        CHECK(!s.unsubscribe("sub-nope")); // FAILURE PATH: unknown subId
        CHECK(!s.unsubscribe(slow.sub_id)); // already removed
    }

    // --- sinceSeq reconnect: ring catch-up + gap fallback ---------------------------------------
    {
        EventStream s("inc-since", /*ring_capacity=*/3);
        for (int i = 0; i < 5; ++i)
            s.publish("files", Json::object()); // 1..5; the ring holds only 3,4,5

        // Reconnect from seq 4: catch up the one event after it (5), no gap.
        auto r = s.subscribe({"files"}, std::string(), std::optional<std::uint64_t>(4));
        CHECK(!r.gapped);
        CHECK(r.catchup.size() == 1);
        CHECK(r.catchup[0].seq == 5);

        // Reconnect from seq 1 (already evicted): gapped => the snapshot is the fallback.
        auto r2 = s.subscribe({"files"}, std::string(), std::optional<std::uint64_t>(1));
        CHECK(r2.gapped);
        CHECK(r2.snapshot.at("lastSeq").as_int() == 5);
    }

    // --- path-scoped delivery (R-BRIDGE-008 "optionally path-scoped") ---------------------------
    {
        EventStream s("inc-path");
        auto with_path = [](const std::string& p)
        {
            Json j = Json::object();
            j.set("path", Json(p));
            return j;
        };
        auto scoped = s.subscribe({"files"}, std::string("src/game"));
        s.publish("files", with_path("src/game/player.scene")); // in scope
        s.publish("files", with_path("src/ui/hud.scene"));       // out of scope — dropped
        s.publish("files", with_path("src/game"));               // the scope root itself — in
        s.publish("files", with_path("src/gamely/x"));           // NOT a child (component boundary)
        auto got = s.poll(scoped.sub_id);
        CHECK(got.size() == 2);
        CHECK(got[0].payload.at("path").as_string() == "src/game/player.scene");
        CHECK(got[1].payload.at("path").as_string() == "src/game");

        // A path-scoped subscription still receives pathless lifecycle events (clients/session/log).
        auto scoped_all = s.subscribe({}, std::string("src/game"));
        s.publish("clients", Json::object()); // pathless => passes the scope filter
        auto lifecycle = s.poll(scoped_all.sub_id);
        CHECK(lifecycle.size() == 1);
        CHECK(lifecycle[0].topic == "clients");
    }

    BRIDGE_TEST_MAIN_END();
}
