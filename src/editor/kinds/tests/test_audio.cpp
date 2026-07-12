// Audio content kinds (R-SYS-006 / L-46): schema-fixture validation against the REAL engine_schemas()
// ctx:audio-bus + ctx:audio-event registration (valid + invalid classes) + the referential-integrity
// semantic analyzers (unique bus ids, parents resolving + acyclic; consistent attenuation range).
// (R-QA-013: happy path, edge cases, AND failure paths.) The per-kind SCHEMAS live in
// src/editor/schema/ (engine_schemas()); this exercises both the registration and the src/editor/kinds/
// semantics on top.

#include "context/editor/kinds/audio_bus.h"
#include "context/editor/kinds/audio_event.h"

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

// A minimal, schema-valid + referentially-consistent bus graph: master <- sfx, master <- music.
constexpr std::string_view kValidBusGraph = R"({
  "$schema": "ctx:audio-bus",
  "version": 1,
  "buses": [
    {"id": "master", "gain": 1.0},
    {"id": "sfx", "gain": 0.8, "parent": "master"},
    {"id": "music", "gain": 0.6, "parent": "master"}
  ]
})";

// A minimal, schema-valid + referentially-consistent spatial event.
constexpr std::string_view kValidEvent = R"({
  "$schema": "ctx:audio-event",
  "version": 1,
  "clip": "footstep",
  "bus": "sfx",
  "gain": 1.0,
  "spatial": {"minDistance": 1.0, "maxDistance": 20.0}
})";

} // namespace

int main()
{
    // ============================================================================================
    // ctx:audio-bus
    // ============================================================================================

    // --- schema validation: happy path -----------------------------------------------------------
    {
        schema::ValidationReport r = validate(kValidBusGraph);
        CHECK(r.schema_bound);
        CHECK(r.schema_id == "ctx:audio-bus");
        CHECK(r.version == 1);
        CHECK(r.ok);
        CHECK(r.diagnostics.empty());
    }

    // --- the kind is registered in the engine schema set (introspectable) -------------------------
    {
        const schema::KindSchema* s = schema::engine_schemas().latest("ctx:audio-bus");
        CHECK(s != nullptr);
        CHECK(s->version == 1);
    }

    // --- schema validation: invalid classes ------------------------------------------------------
    {
        // Missing the required root property (buses).
        schema::ValidationReport r = validate(R"({"$schema": "ctx:audio-bus", "version": 1})");
        CHECK(!r.ok);
        CHECK(has(r, "schema.required_missing"));
    }
    {
        // An undeclared property (additionalProperties: false everywhere).
        schema::ValidationReport r = validate(R"({"$schema": "ctx:audio-bus", "version": 1,
            "buses": [], "bogus": 7})");
        CHECK(!r.ok);
        CHECK(has(r, "schema.unknown_property"));
    }

    // --- semantic analysis: schema-valid but referentially-broken bus graphs ----------------------
    {
        // duplicate bus id.
        const serializer::JsonValue doc = parse(R"({
          "$schema": "ctx:audio-bus", "version": 1, "buses": [
            {"id": "master", "gain": 1.0}, {"id": "master", "gain": 0.5}]})");
        const auto diags = kinds::analyze_audio_bus(doc);
        CHECK(kinds::has_code(diags, "audio_bus.duplicate_bus"));
    }
    {
        // a parent names no declared bus.
        const serializer::JsonValue doc = parse(R"({
          "$schema": "ctx:audio-bus", "version": 1, "buses": [
            {"id": "sfx", "gain": 0.8, "parent": "ghost"}]})");
        const auto diags = kinds::analyze_audio_bus(doc);
        CHECK(kinds::has_code(diags, "audio_bus.parent_unknown"));
    }
    {
        // a two-node parent cycle (a -> b -> a).
        const serializer::JsonValue doc = parse(R"({
          "$schema": "ctx:audio-bus", "version": 1, "buses": [
            {"id": "a", "gain": 1.0, "parent": "b"},
            {"id": "b", "gain": 1.0, "parent": "a"}]})");
        const auto diags = kinds::analyze_audio_bus(doc);
        CHECK(kinds::has_code(diags, "audio_bus.parent_cycle"));
    }
    {
        // a self-parent (a -> a) is also a cycle.
        const serializer::JsonValue doc = parse(R"({
          "$schema": "ctx:audio-bus", "version": 1, "buses": [
            {"id": "a", "gain": 1.0, "parent": "a"}]})");
        const auto diags = kinds::analyze_audio_bus(doc);
        CHECK(kinds::has_code(diags, "audio_bus.parent_cycle"));
    }

    // --- semantic analysis: the valid bus graph produces NO referential diagnostics ---------------
    {
        const serializer::JsonValue doc = parse(kValidBusGraph);
        const auto diags = kinds::analyze_audio_bus(doc);
        CHECK(diags.empty());
    }

    // ============================================================================================
    // ctx:audio-event
    // ============================================================================================

    // --- schema validation: happy path -----------------------------------------------------------
    {
        schema::ValidationReport r = validate(kValidEvent);
        CHECK(r.schema_bound);
        CHECK(r.schema_id == "ctx:audio-event");
        CHECK(r.version == 1);
        CHECK(r.ok);
        CHECK(r.diagnostics.empty());
    }
    {
        const schema::KindSchema* s = schema::engine_schemas().latest("ctx:audio-event");
        CHECK(s != nullptr);
        CHECK(s->version == 1);
    }

    // --- schema validation: invalid classes ------------------------------------------------------
    {
        // Missing a required root property (bus).
        schema::ValidationReport r =
            validate(R"({"$schema": "ctx:audio-event", "version": 1, "clip": "c"})");
        CHECK(!r.ok);
        CHECK(has(r, "schema.required_missing"));
    }
    {
        // An undeclared property in the spatial block.
        schema::ValidationReport r = validate(R"({"$schema": "ctx:audio-event", "version": 1,
            "clip": "c", "bus": "sfx", "spatial": {"minDistance": 1, "maxDistance": 2, "bogus": 3}})");
        CHECK(!r.ok);
        CHECK(has(r, "schema.unknown_property"));
    }

    // --- semantic analysis: a schema-valid but inconsistent attenuation range ---------------------
    {
        // maxDistance <= minDistance (inverted range).
        const serializer::JsonValue doc = parse(R"({
          "$schema": "ctx:audio-event", "version": 1, "clip": "c", "bus": "sfx",
          "spatial": {"minDistance": 10.0, "maxDistance": 5.0}})");
        const auto diags = kinds::analyze_audio_event(doc);
        CHECK(kinds::has_code(diags, "audio_event.invalid_attenuation"));
    }
    {
        // a negative distance.
        const serializer::JsonValue doc = parse(R"({
          "$schema": "ctx:audio-event", "version": 1, "clip": "c", "bus": "sfx",
          "spatial": {"minDistance": -1.0, "maxDistance": 5.0}})");
        const auto diags = kinds::analyze_audio_event(doc);
        CHECK(kinds::has_code(diags, "audio_event.invalid_attenuation"));
    }
    {
        // integer-literal distances are accepted by number_member (the serializer distinguishes
        // integer / number domains) and still consistency-checked.
        const serializer::JsonValue doc = parse(R"({
          "$schema": "ctx:audio-event", "version": 1, "clip": "c", "bus": "sfx",
          "spatial": {"minDistance": 5, "maxDistance": 2}})");
        const auto diags = kinds::analyze_audio_event(doc);
        CHECK(kinds::has_code(diags, "audio_event.invalid_attenuation"));
    }

    // --- semantic analysis: valid + non-spatial events produce NO diagnostics ---------------------
    {
        const serializer::JsonValue doc = parse(kValidEvent);
        const auto diags = kinds::analyze_audio_event(doc);
        CHECK(diags.empty());
    }
    {
        // a non-spatial (2D/UI) event has no attenuation range to check.
        const serializer::JsonValue doc = parse(R"({
          "$schema": "ctx:audio-event", "version": 1, "clip": "ui_click", "bus": "ui", "gain": 1.0})");
        const auto diags = kinds::analyze_audio_event(doc);
        CHECK(diags.empty());
    }

    KINDS_TEST_MAIN_END();
}
