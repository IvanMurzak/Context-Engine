// End-to-end: a file change flows through the graph to an updated derived World and the generation
// counter advances (DoD bullet 1). Also covers creation / removal / empty-pass semantics.

#include "context/editor/derivation/canonical_parse.h"
#include "context/editor/derivation/derivation_graph.h"

#include "derivation_test.h"

#include <string>

using context::editor::derivation::canonical_hash_of;
using context::editor::derivation::DerivationGraph;
using context::editor::filesync::ChangeType;
using context::editor::filesync::ReconcileChange;

namespace
{
ReconcileChange change(std::string path, ChangeType type)
{
    ReconcileChange c;
    c.path = std::move(path);
    c.type = type;
    return c;
}
} // namespace

int main()
{
    DerivationGraph graph;
    CHECK(graph.generation() == 0);
    CHECK(graph.node_count() == 0);

    // Ingest a create. The write ticket carries the canonical hash immediately (own-write barrier key),
    // but the derived world does not change until a pass runs.
    auto ticket = graph.apply(change("a.scene", ChangeType::created), "entity: 1");
    CHECK(ticket.path == "a.scene");
    CHECK(ticket.canonical_hash == canonical_hash_of("entity: 1"));
    CHECK(ticket.generation_after == 1);
    CHECK(graph.pending_count() == 1);
    CHECK(graph.generation() == 0);
    CHECK(graph.node_count() == 0);

    // The pass flows the change into the derived World and advances the generation.
    auto r1 = graph.run_pass();
    CHECK(r1.generation == 1);
    CHECK(r1.nodes_derived == 1);
    CHECK(r1.nodes_skipped == 0);
    CHECK(graph.generation() == 1);
    CHECK(graph.node_count() == 1);
    CHECK(graph.pending_count() == 0);

    auto derived = graph.node("a.scene");
    CHECK(derived.has_value());
    CHECK(derived->canonical_hash == ticket.canonical_hash);
    CHECK(derived->generation == 1);
    CHECK(graph.reflects_hash(ticket.canonical_hash));

    // Two more files in one batched pass → generation advances by exactly one.
    graph.apply(change("b.scene", ChangeType::created), "b");
    graph.apply(change("c.scene", ChangeType::created), "c");
    auto r2 = graph.run_pass();
    CHECK(r2.generation == 2);
    CHECK(r2.nodes_derived == 2);
    CHECK(graph.node_count() == 3);
    CHECK(graph.generation() == 2);

    // Removal flows through: the derived node disappears and the generation advances.
    graph.apply(change("b.scene", ChangeType::removed), "");
    auto r3 = graph.run_pass();
    CHECK(r3.nodes_removed == 1);
    CHECK(r3.nodes_derived == 0);
    CHECK(graph.node_count() == 2);
    CHECK(!graph.node("b.scene").has_value());
    CHECK(graph.generation() == 3);

    // An empty pass is a no-op: the generation counter does NOT advance when nothing is pending.
    auto r4 = graph.run_pass();
    CHECK(r4.generation == 3);
    CHECK(r4.nodes_derived == 0);
    CHECK(graph.generation() == 3);

    // Removing a source that has no derived node is idempotent (no spurious generation churn beyond
    // the batched pass itself).
    graph.apply(change("never.scene", ChangeType::removed), "");
    auto r5 = graph.run_pass();
    CHECK(r5.nodes_removed == 0);
    CHECK(graph.node_count() == 2);

    DERIVATION_TEST_MAIN_END();
}
