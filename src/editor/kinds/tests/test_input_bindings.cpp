// Input-bindings content kind (R-SYS-007 / L-45): schema-fixture validation against the REAL
// engine_schemas() ctx:input-bindings registration (valid + invalid classes) + the referential-integrity
// semantic analyzer (unique action ids, unique context ids, bindings resolving to a declared action).
// (R-QA-013: happy path, edge cases, AND failure paths.) The per-kind SCHEMA lives in
// src/editor/schema/ (engine_schemas()); this exercises both the registration and the
// src/editor/kinds/ semantics on top.

#include "context/editor/kinds/input_bindings.h"

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

// A minimal, schema-valid + referentially-consistent bindings doc: gameplay (move/fire) + a modal
// pause menu that captures.
constexpr std::string_view kValidBindings = R"({
  "$schema": "ctx:input-bindings",
  "version": 1,
  "actions": [
    {"id": "move_x", "layer": "gameplay"},
    {"id": "move_y", "layer": "gameplay"},
    {"id": "fire", "layer": "gameplay"},
    {"id": "ui_menu", "layer": "ui"}
  ],
  "contexts": [
    {"id": "gameplay", "layer": "gameplay", "bindings": [
      {"device": "keyboard", "code": "D", "action": "move_x"},
      {"device": "keyboard", "code": "W", "action": "move_y"},
      {"device": "mouse", "code": "MouseLeft", "action": "fire"}
    ]},
    {"id": "pause", "layer": "ui", "capture": true, "bindings": [
      {"device": "keyboard", "code": "Escape", "action": "ui_menu"}
    ]}
  ]
})";
} // namespace

int main()
{
    // --- schema validation: happy path -----------------------------------------------------------
    {
        schema::ValidationReport r = validate(kValidBindings);
        CHECK(r.schema_bound);
        CHECK(r.schema_id == "ctx:input-bindings");
        CHECK(r.version == 1);
        CHECK(r.ok);
        CHECK(r.diagnostics.empty());
    }

    // --- the kind is registered in the engine schema set (introspectable) -------------------------
    {
        const schema::KindSchema* s = schema::engine_schemas().latest("ctx:input-bindings");
        CHECK(s != nullptr);
        CHECK(s->version == 1);
    }

    // --- schema validation: invalid classes ------------------------------------------------------
    {
        // Missing a required root property (contexts).
        schema::ValidationReport r =
            validate(R"({"$schema": "ctx:input-bindings", "version": 1, "actions": []})");
        CHECK(!r.ok);
        CHECK(has(r, "schema.required_missing"));
    }
    {
        // An undeclared property (additionalProperties: false everywhere).
        schema::ValidationReport r = validate(R"({"$schema": "ctx:input-bindings", "version": 1,
            "actions": [], "contexts": [], "bogus": 7})");
        CHECK(!r.ok);
        CHECK(has(r, "schema.unknown_property"));
    }
    {
        // An out-of-enum device.
        schema::ValidationReport r = validate(R"({"$schema": "ctx:input-bindings", "version": 1,
            "actions": [{"id": "a"}],
            "contexts": [{"id": "c", "layer": "gameplay",
              "bindings": [{"device": "brain", "code": "X", "action": "a"}]}]})");
        CHECK(!r.ok);
        CHECK(has(r, "schema.enum_mismatch"));
    }

    // --- semantic analysis: schema-valid but referentially-broken documents -----------------------
    {
        // duplicate action id.
        const serializer::JsonValue doc = parse(R"({
          "$schema": "ctx:input-bindings", "version": 1,
          "actions": [{"id": "fire"}, {"id": "fire"}], "contexts": []})");
        const auto diags = kinds::analyze_input_bindings(doc);
        CHECK(kinds::has_code(diags, "input_bindings.duplicate_action"));
    }
    {
        // duplicate context id.
        const serializer::JsonValue doc = parse(R"({
          "$schema": "ctx:input-bindings", "version": 1,
          "actions": [{"id": "fire"}],
          "contexts": [{"id": "g", "layer": "gameplay"}, {"id": "g", "layer": "ui"}]})");
        const auto diags = kinds::analyze_input_bindings(doc);
        CHECK(kinds::has_code(diags, "input_bindings.duplicate_context"));
    }
    {
        // a binding names an action that was never declared.
        const serializer::JsonValue doc = parse(R"({
          "$schema": "ctx:input-bindings", "version": 1,
          "actions": [{"id": "move_x"}],
          "contexts": [{"id": "g", "layer": "gameplay",
            "bindings": [{"device": "keyboard", "code": "Space", "action": "jump"}]}]})");
        const auto diags = kinds::analyze_input_bindings(doc);
        CHECK(kinds::has_code(diags, "input_bindings.binding_unknown_action"));
    }

    // --- semantic analysis: the valid doc produces NO referential diagnostics ---------------------
    {
        const serializer::JsonValue doc = parse(kValidBindings);
        const auto diags = kinds::analyze_input_bindings(doc);
        CHECK(diags.empty());
    }

    KINDS_TEST_MAIN_END();
}
