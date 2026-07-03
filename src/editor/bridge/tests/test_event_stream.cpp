// Event-stream unit tests (R-BRIDGE-008): incarnation epoch + monotonic seq, the generation counter
// + derivation.settled quiescence event, the stability field on diagnostics, log forwarding,
// bounded-queue subscribers with gap markers, "since seq N" replay, and the snapshot.

#include "context/editor/bridge/event_stream.h"

#include "bridge_test.h"

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

    BRIDGE_TEST_MAIN_END();
}
