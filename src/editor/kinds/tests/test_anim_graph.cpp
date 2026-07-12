// Anim-graph content kind (R-SYS-008): schema-fixture validation against the REAL engine_schemas()
// registration (valid + invalid classes) + the referential-integrity semantic analyzer (unique state
// ids, the initial state + every transition target resolving). (R-QA-013: happy path, edge cases, AND
// failure paths.) The per-kind SCHEMA lives in src/editor/schema/ (engine_schemas()); this exercises
// both the schema registration and the src/editor/kinds/ semantics on top.

#include "context/editor/kinds/anim_graph.h"

#include "context/editor/schema/kind_schema.h"
#include "context/editor/schema/validator.h"
#include "context/editor/serializer/json_parse.h"

#include "kinds_test.h"

#include <string_view>

namespace kinds = context::editor::kinds;
namespace schema = context::editor::schema;
namespace serializer = context::editor::serializer;

namespace
{

serializer::JsonValue parse(std::string_view text)
{
    serializer::ParseResult p = serializer::parse_json(text);
    CHECK(p.ok);
    return p.root;
}

schema::ValidationReport validate(std::string_view text)
{
    serializer::ParseResult p = serializer::parse_json(text);
    CHECK(p.ok);
    return schema::validate_document(p.root, text, schema::engine_schemas());
}

bool has(const schema::ValidationReport& r, std::string_view code)
{
    for (const schema::ValidationDiagnostic& d : r.diagnostics)
        if (d.code == code)
            return true;
    return false;
}

// A minimal, schema-valid + referentially-consistent anim-graph: idle <-> walk on a control param.
constexpr std::string_view kValidGraph = R"({
  "$schema": "ctx:anim-graph",
  "version": 1,
  "initial": "idle",
  "states": [
    {"id": "idle", "clip": "idle_clip", "transitions": [
      {"to": "walk", "op": "ge", "threshold": 1, "duration": 0.2}
    ]},
    {"id": "walk", "clip": "walk_clip", "transitions": [
      {"to": "idle", "op": "lt", "threshold": 1, "duration": 0.2}
    ]}
  ]
})";

} // namespace

int main()
{
    // --- schema validation: happy path -----------------------------------------------------------
    {
        schema::ValidationReport r = validate(kValidGraph);
        CHECK(r.schema_bound);
        CHECK(r.schema_id == "ctx:anim-graph");
        CHECK(r.version == 1);
        CHECK(r.ok);
        CHECK(r.diagnostics.empty());
    }

    // --- the kind is registered in the engine schema set (introspectable) -------------------------
    {
        const schema::KindSchema* s = schema::engine_schemas().latest("ctx:anim-graph");
        CHECK(s != nullptr);
        CHECK(s->version == 1);
    }

    // --- schema validation: invalid classes ------------------------------------------------------
    {
        // Missing a required root property (states).
        schema::ValidationReport r = validate(R"({"$schema": "ctx:anim-graph", "version": 1,
            "initial": "idle"})");
        CHECK(!r.ok);
        CHECK(has(r, "schema.required_missing"));
    }
    {
        // An undeclared property (additionalProperties: false everywhere).
        schema::ValidationReport r = validate(R"({"$schema": "ctx:anim-graph", "version": 1,
            "initial": "idle", "states": [], "bogus": 7})");
        CHECK(!r.ok);
        CHECK(has(r, "schema.unknown_property"));
    }
    {
        // A transition `op` outside the enum is rejected (the exact code is the schema's enum check;
        // asserting the blocking outcome keeps this robust to the code spelling).
        schema::ValidationReport r = validate(R"({"$schema": "ctx:anim-graph", "version": 1,
            "initial": "idle", "states": [
              {"id": "idle", "clip": "c", "transitions": [{"to": "idle", "op": "eq", "threshold": 1}]}]})");
        CHECK(!r.ok);
    }

    // --- semantic analysis: a schema-valid but referentially-broken graph -------------------------
    {
        // duplicate state id.
        const serializer::JsonValue doc = parse(R"({
          "$schema": "ctx:anim-graph", "version": 1, "initial": "idle", "states": [
            {"id": "idle", "clip": "a"}, {"id": "idle", "clip": "b"}]})");
        const auto diags = kinds::analyze_anim_graph(doc);
        CHECK(kinds::has_code(diags, "anim_graph.duplicate_state"));
    }
    {
        // initial names no declared state.
        const serializer::JsonValue doc = parse(R"({
          "$schema": "ctx:anim-graph", "version": 1, "initial": "run", "states": [
            {"id": "idle", "clip": "a"}]})");
        const auto diags = kinds::analyze_anim_graph(doc);
        CHECK(kinds::has_code(diags, "anim_graph.initial_unknown"));
    }
    {
        // a transition target names no declared state.
        const serializer::JsonValue doc = parse(R"({
          "$schema": "ctx:anim-graph", "version": 1, "initial": "idle", "states": [
            {"id": "idle", "clip": "a", "transitions": [
              {"to": "ghost", "op": "ge", "threshold": 1}]}]})");
        const auto diags = kinds::analyze_anim_graph(doc);
        CHECK(kinds::has_code(diags, "anim_graph.transition_unknown_target"));
    }

    // --- semantic analysis: the valid graph produces NO referential diagnostics -------------------
    {
        const serializer::JsonValue doc = parse(kValidGraph);
        const auto diags = kinds::analyze_anim_graph(doc);
        CHECK(diags.empty());
    }

    KINDS_TEST_MAIN_END();
}
