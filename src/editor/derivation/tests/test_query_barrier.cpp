// Read-your-writes query barrier (DoD bullet 4; R-CLI-006): a read bounded-blocks until the derived
// world reflects the write's canonical hash (own-write) or a foreign generation, else times out.

#include "context/editor/derivation/canonical_parse.h"
#include "context/editor/derivation/derivation_graph.h"
#include "context/editor/derivation/query_barrier.h"

#include "derivation_test.h"

#include <string>

using context::editor::derivation::BarrierStatus;
using context::editor::derivation::canonical_hash_of;
using context::editor::derivation::DerivationConfig;
using context::editor::derivation::DerivationGraph;
using context::editor::derivation::wait_for_generation;
using context::editor::derivation::wait_for_hash;
using context::editor::filesync::ChangeType;
using context::editor::filesync::ReconcileChange;

namespace
{
ReconcileChange change(std::string path, ChangeType type = ChangeType::modified)
{
    ReconcileChange c;
    c.path = std::move(path);
    c.type = type;
    return c;
}
} // namespace

int main()
{
    // --- own-write barrier (`--after-hash`) ---------------------------------------------------
    {
        DerivationGraph graph;
        auto ticket = graph.apply(change("doc"), "hello world");
        CHECK(!graph.reflects_hash(ticket.canonical_hash)); // not derived yet
        CHECK(graph.generation() == 0);

        auto r = wait_for_hash(graph, ticket.canonical_hash, 8);
        CHECK(r.ok());
        CHECK(r.passes == 1);
        CHECK(graph.reflects_hash(ticket.canonical_hash));
        CHECK(graph.node("doc")->canonical_hash == ticket.canonical_hash);

        // Already satisfied → resolves with no pumping.
        auto again = wait_for_hash(graph, ticket.canonical_hash, 8);
        CHECK(again.ok());
        CHECK(again.passes == 0);

        // A hash that was never written times out explicitly (never hangs).
        auto missing = wait_for_hash(graph, canonical_hash_of("never written"), 5);
        CHECK(!missing.ok());
        CHECK(missing.status == BarrierStatus::timed_out);
        CHECK(missing.passes == 5);
    }

    // --- read-your-writes end-to-end: write then immediately observe the effect ---------------
    {
        DerivationGraph rw;
        auto w = rw.apply(change("player.scene"), "hp: 100");
        CHECK(!rw.node("player.scene").has_value()); // a naive read would miss the write

        auto barrier = wait_for_hash(rw, w.canonical_hash, 8);
        CHECK(barrier.ok());
        auto seen = rw.node("player.scene");
        CHECK(seen.has_value());
        CHECK(seen->canonical_hash == w.canonical_hash);
    }

    // --- foreign-generation barrier (`--after-generation`) ------------------------------------
    {
        DerivationGraph graph;
        graph.apply(change("g1"), "one");
        auto rg = wait_for_generation(graph, 1, 4);
        CHECK(rg.ok());
        CHECK(graph.generation() == 1);

        // Unreachable generation with nothing pending → the counter is frozen → explicit timeout.
        auto frozen = wait_for_generation(graph, 11, 4);
        CHECK(!frozen.ok());
        CHECK(frozen.status == BarrierStatus::timed_out);
    }

    // --- own-write barrier is robust under load-shedding --------------------------------------
    // A non-visible write repeatedly shed by the backpressure policy STILL resolves on the hash
    // barrier once it is finally incorporated (R-CLI-006: use the hash barrier for own writes).
    {
        DerivationConfig cfg;
        cfg.high_watermark = 2;
        cfg.max_batch_per_pass = 1;
        DerivationGraph shed(cfg);
        shed.set_visible("hot", true);

        auto cold = shed.apply(change("zzz-cold"), "cold-content"); // sorts last, not visible
        shed.apply(change("hot"), "h");
        shed.apply(change("a-other"), "o1");
        shed.apply(change("b-other"), "o2");
        CHECK(shed.backpressure().overloaded == true);

        auto r = wait_for_hash(shed, cold.canonical_hash, 20);
        CHECK(r.ok());          // resolved despite repeated deferral
        CHECK(r.passes >= 2);   // it took more than one pass (it was shed behind the visible node)
        CHECK(shed.node("zzz-cold").has_value());
    }

    DERIVATION_TEST_MAIN_END();
}
