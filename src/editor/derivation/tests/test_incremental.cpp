// Incremental re-derive recomputes ONLY affected subgraph nodes, and content-hash memoization skips a
// dirty node whose canonical form did not actually change (DoD bullet 2; L-22).

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
    DerivationGraph graph;
    graph.apply(change("a"), "AAA");
    graph.apply(change("b"), "BBB");
    graph.apply(change("c"), "CCC");
    graph.run_pass(); // generation 1: all three derived
    CHECK(graph.derivations() == 3);
    CHECK(graph.node("a")->generation == 1);
    CHECK(graph.node("b")->generation == 1);
    CHECK(graph.node("c")->generation == 1);

    // Change ONLY b. Only b re-parses and re-derives; a and c are never revisited.
    const auto parses_before = graph.parse_invocations();
    const auto derivations_before = graph.derivations();
    graph.apply(change("b"), "BBB-v2");
    auto r1 = graph.run_pass(); // generation 2
    CHECK(graph.parse_invocations() == parses_before + 1); // only b parsed
    CHECK(r1.nodes_derived == 1);
    CHECK(graph.derivations() == derivations_before + 1);
    CHECK(graph.node("b")->generation == 2);
    CHECK(graph.node("a")->generation == 1); // untouched
    CHECK(graph.node("c")->generation == 1); // untouched
    CHECK(graph.generation() == 2);

    // Memoization via canonicalization: rewriting a with formatting-only noise (whitespace + key
    // order — the file is JSON, so the REAL canonicalizer erases both) yields the SAME canonical
    // form, so the node is dirty-then-skipped — no downstream re-derive, entity generation frozen.
    const auto derivations_after_b = graph.derivations();
    graph.apply(change("j"), "{\"k\": 1, \"m\": 2}");
    graph.run_pass(); // generation 3: j derived once
    const auto derivations_after_j = graph.derivations();
    graph.apply(change("j"), "  {\n  \"m\" : 2,\t\"k\" : 1 }  "); // same canonical form
    auto r2 = graph.run_pass();                                   // generation 4
    CHECK(r2.nodes_skipped == 1);
    CHECK(r2.nodes_derived == 0);
    CHECK(graph.derivations() == derivations_after_j); // no derivation work happened
    CHECK(graph.node("j")->generation == 3);           // memoized: value never changed
    CHECK(graph.node("j")->canonical_hash ==
          context::editor::derivation::canonical_parse("{\"k\": 1, \"m\": 2}").canonical_hash);
    CHECK(graph.derivations() == derivations_after_b + 1); // exactly the one j derivation
    CHECK(graph.generation() == 4); // global generation still advances (the batch was incorporated)

    // Memoization via byte-identical rewrite (the common "save with no edit" case).
    graph.apply(change("c"), "CCC");
    auto r3 = graph.run_pass();
    CHECK(r3.nodes_skipped == 1);
    CHECK(graph.node("c")->generation == 1);

    // Coalescing: several writes to one path before a pass collapse into a SINGLE derivation, latest
    // content winning — the graph derives once, not once per event.
    graph.apply(change("a"), "AAA-x");
    graph.apply(change("a"), "AAA-y");
    CHECK(graph.pending_count() == 1);
    const auto derivations_before_coalesce = graph.derivations();
    auto r4 = graph.run_pass();
    CHECK(r4.nodes_derived == 1);
    CHECK(graph.derivations() == derivations_before_coalesce + 1);
    CHECK(graph.node("a")->canonical_hash == canonical_hash_of("AAA-y")); // latest write wins

    DERIVATION_TEST_MAIN_END();
}
