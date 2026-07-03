// Derivation-side backpressure (DoD bullet 3; R-FILE-013): a write burst coalesces into batched passes,
// exposes a queue-depth / bounded-lag signal, and load-sheds to the visible/queried subgraph first.

#include "context/editor/derivation/derivation_graph.h"

#include "context/kernel/event_bus.h"

#include "derivation_test.h"

#include <string>
#include <vector>

using context::editor::derivation::BackpressureEvent;
using context::editor::derivation::DerivationConfig;
using context::editor::derivation::DerivationGraph;
using context::editor::derivation::DerivationPassEvent;
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
    DerivationConfig cfg;
    cfg.high_watermark = 4;
    cfg.max_batch_per_pass = 3;

    context::kernel::EventBus bus;
    std::vector<BackpressureEvent> bp_events;
    std::vector<DerivationPassEvent> pass_events;
    bus.subscribe<BackpressureEvent>([&](const BackpressureEvent& e) { bp_events.push_back(e); });
    bus.subscribe<DerivationPassEvent>([&](const DerivationPassEvent& e)
                                       { pass_events.push_back(e); });

    DerivationGraph graph(cfg, &bus);
    graph.set_visible("visible", true); // a queried/visible node the load-shed policy prioritizes

    // A burst of 10 distinct dirty files (> high_watermark) trips the overload signal exactly once.
    graph.apply(change("visible"), "V");
    for (int i = 0; i < 9; ++i)
        graph.apply(change("f" + std::to_string(i)), "data" + std::to_string(i));
    CHECK(graph.pending_count() == 10);
    CHECK(graph.backpressure().queue_depth == 10); // the bounded-lag signal
    CHECK(graph.backpressure().overloaded == true);
    CHECK(bp_events.size() == 1); // published on the false->true transition
    CHECK(bp_events[0].overloaded == true);
    CHECK(bp_events[0].queue_depth == 5); // fired the moment depth crossed the watermark (5 > 4)

    // Pass 1 (overloaded): the visible node derives first, then a bounded fill — the rest is shed.
    auto p1 = graph.run_pass();
    CHECK(p1.nodes_derived == 3); // capped at max_batch_per_pass
    CHECK(p1.deferred == 7);
    CHECK(graph.node("visible").has_value()); // visible/queried subgraph prioritized under load
    CHECK(graph.node("visible")->generation == 1);
    CHECK(graph.pending_count() == 7);
    CHECK(graph.generation() == 1);

    // Pass 2 (still overloaded: 7 > 4): another bounded batch; depth falls back under the watermark.
    auto p2 = graph.run_pass();
    CHECK(p2.nodes_derived == 3);
    CHECK(graph.pending_count() == 4);
    CHECK(graph.backpressure().overloaded == false); // 4 is not > 4
    CHECK(bp_events.size() == 2);                     // true->false transition published
    CHECK(bp_events[1].overloaded == false);

    // Pass 3 (not overloaded): drain the remainder in one batch.
    auto p3 = graph.run_pass();
    CHECK(p3.nodes_derived == 4);
    CHECK(p3.deferred == 0);
    CHECK(graph.pending_count() == 0);
    CHECK(graph.node_count() == 10); // every file eventually derived, none lost
    CHECK(graph.generation() == 3);
    CHECK(graph.backpressure().queue_depth == 0);

    // The generation counter is surfaced on the event stream, one event per non-empty pass.
    CHECK(pass_events.size() == 3);
    CHECK(pass_events[0].generation == 1);
    CHECK(pass_events[1].generation == 2);
    CHECK(pass_events[2].generation == 3);

    // Coalescing of a hot path: 20 rapid writes to ONE file collapse into a single batched derivation
    // (not one pass per event) and never trip overload (queue depth stays 1).
    DerivationGraph hot(cfg);
    for (int i = 0; i < 20; ++i)
        hot.apply(change("hot"), "v" + std::to_string(i));
    CHECK(hot.pending_count() == 1);
    CHECK(hot.backpressure().overloaded == false);
    auto hr = hot.run_pass();
    CHECK(hr.nodes_derived == 1); // derived once despite 20 writes
    CHECK(hot.generation() == 1);
    CHECK(hot.node_count() == 1);

    // The visible/queried subgraph is NEVER shed: max_batch_per_pass caps only the NON-visible fill, so
    // when the visible-pending set ALONE exceeds it, every visible node still derives in one overloaded
    // pass (R-FILE-013 "never stall a query"). Locks in that intentional trade-off (visible batch is
    // deliberately unbounded); only the non-visible remainder defers.
    {
        DerivationConfig vcfg;
        vcfg.high_watermark = 4;
        vcfg.max_batch_per_pass = 3;
        DerivationGraph vis(vcfg);

        for (int i = 0; i < 6; ++i)
            vis.set_visible("v" + std::to_string(i), true); // 6 visible > max_batch_per_pass
        for (int i = 0; i < 6; ++i)
            vis.apply(change("v" + std::to_string(i)), "vis" + std::to_string(i));
        vis.apply(change("bg0"), "b0"); // 2 non-visible background writes
        vis.apply(change("bg1"), "b1");
        CHECK(vis.backpressure().overloaded == true); // 8 pending > 4

        auto pv = vis.run_pass();
        CHECK(pv.nodes_derived == 6);                       // ALL visible derived, none shed
        CHECK(pv.nodes_derived > vcfg.max_batch_per_pass);  // deliberately exceeds the fill cap
        CHECK(pv.deferred == 2);                            // only the non-visible fill was shed
        CHECK(vis.node_count() == 6);
        CHECK(vis.pending_count() == 2);
        CHECK(vis.generation() == 1);
        for (int i = 0; i < 6; ++i)
            CHECK(vis.node("v" + std::to_string(i)).has_value());
    }

    DERIVATION_TEST_MAIN_END();
}
