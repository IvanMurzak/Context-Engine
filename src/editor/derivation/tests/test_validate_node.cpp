// The M2 validate node (R-DATA-006 wired per R-FILE-003): schema-bound payloads validate on
// ingest; a FAILING payload retains its last-good derived state (or never derives) while its
// diagnostics (JSON-pointer + line/column) surface through validation(); a later valid write
// clears the finding (the one-loop self-correction). Stability follows L-31: provisional while a
// re-derivation of the path is queued, stable once settled.
// (R-QA-013: happy path, edge cases, AND failure paths.)

#include "context/editor/derivation/derivation_graph.h"
#include "context/editor/schema/kind_schema.h"

#include "derivation_test.h"

#include <string>
#include <string_view>

using context::editor::derivation::DerivationGraph;
using context::editor::filesync::ChangeType;
using context::editor::filesync::ReconcileChange;
namespace schema = context::editor::schema;

namespace
{
ReconcileChange change(std::string path, ChangeType type)
{
    ReconcileChange c;
    c.path = std::move(path);
    c.type = type;
    return c;
}

const char* kValidScene = R"({
  "$schema": "ctx:scene",
  "version": 1,
  "notes": "a valid scene",
  "entities": [
    {"name": "MainCamera",
     "components": {"camera": {"fov": 1.0471975511965976, "near": 0.1, "far": 1000}}}
  ]
})";

// `entities` is an object — a schema.type_mismatch against ctx:scene.
const char* kInvalidScene = "{\n"
                            "  \"$schema\": \"ctx:scene\",\n"
                            "  \"version\": 1,\n"
                            "  \"entities\": {\"broken\": true}\n"
                            "}\n";
} // namespace

int main()
{
    // --- an invalid NEW source never derives; fixing it derives (self-correction loop) ----------
    {
        DerivationGraph graph({}, nullptr, &schema::engine_schemas());
        graph.apply(change("a.scene.json", ChangeType::created), kInvalidScene);
        auto pass = graph.run_pass();
        CHECK(pass.nodes_invalid == 1);
        CHECK(pass.nodes_derived == 0);
        CHECK(!graph.node("a.scene.json").has_value()); // no derived node — nothing to be stale
        CHECK(graph.node_count() == 0);

        auto v = graph.validation("a.scene.json");
        CHECK(v.has_value());
        CHECK(v->report.schema_bound);
        CHECK(!v->report.ok);
        CHECK(v->stable); // settled: nothing pending for the path
        CHECK(v->generation == pass.generation);
        CHECK(!v->report.diagnostics.empty());
        CHECK(v->report.diagnostics[0].code == "schema.type_mismatch");
        CHECK(v->report.diagnostics[0].pointer == "/entities");
        CHECK(v->report.diagnostics[0].line == 4); // located in the AUTHORED source bytes
        CHECK(v->report.diagnostics[0].column == 15);

        // The fix derives normally and the finding clears (R-FILE-003 one-loop self-correction).
        graph.apply(change("a.scene.json", ChangeType::modified), kValidScene);
        auto pass2 = graph.run_pass();
        CHECK(pass2.nodes_invalid == 0);
        CHECK(pass2.nodes_derived == 1);
        CHECK(graph.node("a.scene.json").has_value());
        auto v2 = graph.validation("a.scene.json");
        CHECK(v2.has_value());
        CHECK(v2->report.ok);
        CHECK(v2->report.diagnostics.empty());
    }

    // --- LAST-GOOD RETENTION: an invalid edit never clobbers the derived state ------------------
    {
        DerivationGraph graph({}, nullptr, &schema::engine_schemas());
        auto good = graph.apply(change("s.scene.json", ChangeType::created), kValidScene);
        graph.run_pass();
        auto derived_good = graph.node("s.scene.json");
        CHECK(derived_good.has_value());
        CHECK(derived_good->canonical_hash == good.canonical_hash);

        graph.apply(change("s.scene.json", ChangeType::modified), kInvalidScene);
        auto pass = graph.run_pass();
        CHECK(pass.nodes_invalid == 1);

        // The derived World still carries the LAST-GOOD value at its original generation.
        auto retained = graph.node("s.scene.json");
        CHECK(retained.has_value());
        CHECK(retained->canonical_hash == good.canonical_hash);
        CHECK(retained->generation == derived_good->generation);
        CHECK(graph.reflects_hash(good.canonical_hash)); // own-write barrier still satisfied
        auto v = graph.validation("s.scene.json");
        CHECK(v.has_value());
        CHECK(!v->report.ok);

        // Recovery: a valid write replaces the retained state.
        graph.apply(change("s.scene.json", ChangeType::modified), kValidScene);
        graph.run_pass();
        CHECK(graph.validation("s.scene.json")->report.ok);
    }

    // --- STABILITY (L-31): provisional while the path has queued work, stable once settled ------
    {
        DerivationGraph graph({}, nullptr, &schema::engine_schemas());
        graph.apply(change("p.scene.json", ChangeType::created), kInvalidScene);
        graph.run_pass();
        CHECK(graph.validation("p.scene.json")->stable);

        // Queue a re-derivation of the SAME path: the existing finding may be cleared by the
        // settling pass, so it degrades to provisional.
        graph.apply(change("p.scene.json", ChangeType::modified), kValidScene);
        CHECK(!graph.validation("p.scene.json")->stable);
        graph.run_pass();
        CHECK(graph.validation("p.scene.json")->stable);
        CHECK(graph.validation("p.scene.json")->report.ok);
    }

    // --- non-blocking bindings derive normally ---------------------------------------------------
    {
        DerivationGraph graph({}, nullptr, &schema::engine_schemas());
        // An unknown $schema id (the 10k bench corpus shape) — informational only.
        graph.apply(change("c.scene.json", ChangeType::created),
                    "{\"$schema\": \"https://schemas.context-engine.dev/m0/scene.schema.json\","
                    " \"version\": 3, \"entities\": []}");
        auto pass = graph.run_pass();
        CHECK(pass.nodes_invalid == 0);
        CHECK(pass.nodes_derived == 1);
        CHECK(graph.node("c.scene.json").has_value());
        auto v = graph.validation("c.scene.json");
        CHECK(v.has_value());
        CHECK(v->report.ok);
        CHECK(!v->report.schema_bound);
        CHECK(v->report.diagnostics.size() == 1);
        CHECK(v->report.diagnostics[0].code == "schema.unknown_kind");

        // A doc with NO $schema — not bound, clean report, derives.
        graph.apply(change("plain.json", ChangeType::created), "{\"entity\": 1}");
        graph.run_pass();
        CHECK(graph.node("plain.json").has_value());
        CHECK(graph.validation("plain.json").has_value());
        CHECK(graph.validation("plain.json")->report.diagnostics.empty());

        // Non-JSON content (the L-32 carve-outs) skips the validate node entirely.
        graph.apply(change("shader.wgsl", ChangeType::created), "fn main() {}");
        graph.run_pass();
        CHECK(graph.node("shader.wgsl").has_value());
        CHECK(!graph.validation("shader.wgsl").has_value());
    }

    // --- removal clears validation state ---------------------------------------------------------
    {
        DerivationGraph graph({}, nullptr, &schema::engine_schemas());
        graph.apply(change("r.scene.json", ChangeType::created), kInvalidScene);
        graph.run_pass();
        CHECK(graph.validation("r.scene.json").has_value());
        graph.apply(change("r.scene.json", ChangeType::removed), std::string_view{});
        graph.run_pass();
        CHECK(!graph.validation("r.scene.json").has_value());
    }

    // --- the M1 default (no schema set) is byte-for-byte unchanged -------------------------------
    {
        DerivationGraph graph; // no schemas wired — validation off
        graph.apply(change("legacy.scene.json", ChangeType::created), kInvalidScene);
        auto pass = graph.run_pass();
        CHECK(pass.nodes_invalid == 0);
        CHECK(pass.nodes_derived == 1); // derives regardless (the M1 behavior)
        CHECK(graph.node("legacy.scene.json").has_value());
        CHECK(!graph.validation("legacy.scene.json").has_value());
    }

    DERIVATION_TEST_MAIN_END();
}
