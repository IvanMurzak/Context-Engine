// Document validation against registered kind schemas: header binding, every diagnostic class
// with JSON-pointer + line/column, the x-ctx-ref resolver seam (wrong-kind rejection), tagged-union
// enforcement, blessed notes, pointer location, and notes preservation through the canonical
// round-trip (the PR #45 serializer). (R-QA-013: happy path, edge cases, AND failure paths.)

#include "context/editor/schema/validator.h"

#include "context/editor/schema/kind_schema.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include "schema_test.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace context::editor::schema;
namespace serializer = context::editor::serializer;

namespace
{

ValidationReport validate(std::string_view doc, const SchemaSet& set,
                          const RefTargetResolver* resolver = nullptr)
{
    auto parsed = serializer::parse_json(doc);
    CHECK(parsed.ok);
    return validate_document(parsed.root, doc, set, resolver);
}

bool has_code(const ValidationReport& report, std::string_view code)
{
    for (const ValidationDiagnostic& d : report.diagnostics)
        if (d.code == code)
            return true;
    return false;
}

const ValidationDiagnostic* find_code(const ValidationReport& report, std::string_view code)
{
    for (const ValidationDiagnostic& d : report.diagnostics)
        if (d.code == code)
            return &d;
    return nullptr;
}

// A synthetic kind exercising every vocabulary feature.
const SchemaSet& test_set()
{
    static const SchemaSet set = [] {
        SchemaSet s;
        std::vector<std::string> problems;
        auto schema = compile_kind_schema(R"({
            "$id": "test:thing",
            "version": 1,
            "type": "object",
            "additionalProperties": false,
            "required": ["name"],
            "properties": {
                "notes": {"description": "blessed"},
                "name": {"type": "string"},
                "flavor": {"type": "string", "enum": ["sweet", "sour"]},
                "rotation": {"x-ctx-type": "quaternion"},
                "tint": {"x-ctx-type": "color"},
                "ramp": {"x-ctx-type": "gradient"},
                "ease": {"x-ctx-type": "curve"},
                "flags": {"x-ctx-type": "bit-flags"},
                "position": {"type": "array", "items": {"type": "number"},
                             "x-ctx-units": "m", "x-ctx-storage": "f32x3"},
                "mesh": {"x-ctx-ref": "ctx:mesh"},
                "collider": {"x-ctx-union": {
                    "shape:circle": {"type": "object",
                                     "properties": {"radius": {"type": "number",
                                                               "x-ctx-units": "m"}}},
                    "shape:box": {"type": "object"}}}
            }
        })",
                                          problems);
        CHECK(problems.empty());
        CHECK(schema.has_value());
        s.add(std::move(*schema));
        return s;
    }();
    return set;
}

class FakeResolver final : public RefTargetResolver
{
public:
    [[nodiscard]] std::optional<std::string> kind_of(std::string_view guid) const override
    {
        if (guid == "guid-mesh")
            return std::string("ctx:mesh");
        if (guid == "guid-texture")
            return std::string("ctx:texture");
        return std::nullopt; // unknown GUIDs are not enforced (the asset db decides later)
    }
};

} // namespace

int main()
{
    const SchemaSet& set = test_set();

    // --- binding ---------------------------------------------------------------------------------
    {
        // No $schema — not schema-bound, no findings (validation does not apply).
        auto r = validate("{\"anything\": 1}", set);
        CHECK(!r.schema_bound);
        CHECK(r.ok);
        CHECK(r.diagnostics.empty());
    }
    {
        // Unknown kind — informational, NON-blocking (kinds register incrementally).
        auto r = validate("{\"$schema\": \"https://example.com/other.json\", \"version\": 1}", set);
        CHECK(!r.schema_bound);
        CHECK(r.ok);
        CHECK(has_code(r, "schema.unknown_kind"));
    }
    {
        // Bound + missing version — BLOCKING (the L-32 header is mandatory for a bound kind).
        auto r = validate("{\"$schema\": \"test:thing\", \"name\": \"x\"}", set);
        CHECK(r.schema_bound);
        CHECK(!r.ok);
        CHECK(has_code(r, "schema.version_missing"));
    }
    {
        // Stamped NEWER than the registered schema — BLOCKING, never best-effort (L-37).
        auto r = validate("{\"$schema\": \"test:thing\", \"version\": 9, \"name\": \"x\"}", set);
        CHECK(r.schema_bound);
        CHECK(!r.ok);
        CHECK(has_code(r, "schema.newer_than_engine"));
        CHECK(find_code(r, "schema.newer_than_engine")->pointer == "/version");
    }

    // --- happy path ------------------------------------------------------------------------------
    {
        auto r = validate(R"({
            "$schema": "test:thing",
            "version": 1,
            "name": "ball",
            "notes": ["authored annotation", "second note"],
            "flavor": "sweet",
            "rotation": [0, 0, 0, 1],
            "tint": {"space": "srgb", "value": [1, 0.5, 0.25]},
            "ramp": {"stops": [{"t": 0, "color": {"space": "srgb", "value": [0,0,0]}}]},
            "ease": {"keys": [{"t": 0, "v": 0}, {"t": 1, "v": 1}]},
            "flags": ["kinematic"],
            "position": [1, 2, 3],
            "mesh": {"$ref": "guid-mesh", "path": "meshes/ball.mesh.json"},
            "collider": {"type": "shape:circle", "radius": 0.5}
        })",
                          set);
        CHECK(r.schema_bound);
        CHECK(r.version == 1);
        CHECK(r.ok);
        CHECK(r.diagnostics.empty());
    }

    // --- structural failures carry pointer + line/column into the SOURCE bytes -------------------
    {
        const std::string doc = "{\n"
                                "  \"$schema\": \"test:thing\",\n"
                                "  \"version\": 1,\n"
                                "  \"name\": 42\n"
                                "}\n";
        auto r = validate(doc, set);
        CHECK(!r.ok);
        const ValidationDiagnostic* d = find_code(r, "schema.type_mismatch");
        CHECK(d != nullptr);
        CHECK(d->pointer == "/name");
        CHECK(d->line == 4);    // the value 42 sits on line 4 ...
        CHECK(d->column == 11); // ... column 11 (1-based, the value's first byte)
    }
    {
        auto r = validate("{\"$schema\": \"test:thing\", \"version\": 1}", set);
        CHECK(!r.ok);
        CHECK(has_code(r, "schema.required_missing")); // name
    }
    {
        auto r = validate(
            "{\"$schema\": \"test:thing\", \"version\": 1, \"name\": \"x\", \"ghost\": 1}", set);
        CHECK(!r.ok);
        const ValidationDiagnostic* d = find_code(r, "schema.unknown_property");
        CHECK(d != nullptr);
        CHECK(d->pointer == "/ghost");
    }
    {
        auto r = validate(
            "{\"$schema\": \"test:thing\", \"version\": 1, \"name\": \"x\", \"flavor\": \"salty\"}",
            set);
        CHECK(!r.ok);
        CHECK(has_code(r, "schema.enum_mismatch"));
    }
    {
        // Bad notes shape — the blessed field still has A shape.
        auto r = validate(
            "{\"$schema\": \"test:thing\", \"version\": 1, \"name\": \"x\", \"notes\": 42}", set);
        CHECK(!r.ok);
        CHECK(has_code(r, "schema.notes_invalid"));
    }

    // --- semantic types --------------------------------------------------------------------------
    {
        auto r = validate("{\"$schema\": \"test:thing\", \"version\": 1, \"name\": \"x\","
                          " \"rotation\": [0, 0, 1]}",
                          set);
        CHECK(!r.ok);
        const ValidationDiagnostic* d = find_code(r, "schema.semantic_invalid");
        CHECK(d != nullptr);
        CHECK(d->pointer == "/rotation");
    }
    {
        // An undeclared color space inside a gradient stop — the pointer digs into the composite.
        auto r = validate("{\"$schema\": \"test:thing\", \"version\": 1, \"name\": \"x\","
                          " \"ramp\": {\"stops\": [{\"t\": 0, \"color\": {\"value\": [0,0,0]}}]}}",
                          set);
        CHECK(!r.ok);
        const ValidationDiagnostic* d = find_code(r, "schema.semantic_invalid");
        CHECK(d != nullptr);
        CHECK(d->pointer == "/ramp/stops/0/color");
        CHECK(d->line > 0); // located in the source bytes
    }

    // --- storage arity (the layout pins the element count) ---------------------------------------
    {
        auto r = validate("{\"$schema\": \"test:thing\", \"version\": 1, \"name\": \"x\","
                          " \"position\": [1, 2]}",
                          set);
        CHECK(!r.ok);
        CHECK(has_code(r, "schema.storage_arity")); // f32x3 holds exactly 3
    }

    // --- typed references (the L-34 dual form + the resolver seam) -------------------------------
    {
        // Shape-only (no resolver): a plain string is NOT a reference.
        auto r = validate("{\"$schema\": \"test:thing\", \"version\": 1, \"name\": \"x\","
                          " \"mesh\": \"meshes/ball.mesh.json\"}",
                          set);
        CHECK(!r.ok);
        CHECK(has_code(r, "schema.ref_shape_invalid"));
    }
    {
        // Same-file entity form is accepted.
        auto r = validate("{\"$schema\": \"test:thing\", \"version\": 1, \"name\": \"x\","
                          " \"mesh\": {\"$entity\": \"e-1\"}}",
                          set);
        CHECK(r.ok);
    }
    {
        // Extra members beyond {$ref, path} are rejected.
        auto r = validate("{\"$schema\": \"test:thing\", \"version\": 1, \"name\": \"x\","
                          " \"mesh\": {\"$ref\": \"guid-mesh\", \"guid\": \"again\"}}",
                          set);
        CHECK(!r.ok);
        CHECK(has_code(r, "schema.ref_shape_invalid"));
    }
    {
        // The meta-lookup seam: a ref to the WRONG kind is a validate error (R-DATA-006).
        FakeResolver resolver;
        auto r = validate("{\"$schema\": \"test:thing\", \"version\": 1, \"name\": \"x\","
                          " \"mesh\": {\"$ref\": \"guid-texture\"}}",
                          set, &resolver);
        CHECK(!r.ok);
        const ValidationDiagnostic* d = find_code(r, "schema.ref_wrong_kind");
        CHECK(d != nullptr);
        CHECK(d->pointer == "/mesh");
        // The right kind passes through the same resolver.
        auto ok = validate("{\"$schema\": \"test:thing\", \"version\": 1, \"name\": \"x\","
                           " \"mesh\": {\"$ref\": \"guid-mesh\"}}",
                           set, &resolver);
        CHECK(ok.ok);
        // An unknown GUID is not enforced (the asset db will know; the seam must not guess).
        auto unknown = validate("{\"$schema\": \"test:thing\", \"version\": 1, \"name\": \"x\","
                                " \"mesh\": {\"$ref\": \"guid-unknown\"}}",
                                set, &resolver);
        CHECK(unknown.ok);
    }

    // --- the pinned tagged-union convention -------------------------------------------------------
    {
        auto r = validate("{\"$schema\": \"test:thing\", \"version\": 1, \"name\": \"x\","
                          " \"collider\": {\"radius\": 1}}",
                          set);
        CHECK(!r.ok);
        CHECK(has_code(r, "schema.union_tag_missing"));
    }
    {
        // An ad-hoc (un-namespaced) tag violates the ONE convention.
        auto r = validate("{\"$schema\": \"test:thing\", \"version\": 1, \"name\": \"x\","
                          " \"collider\": {\"type\": \"Circle\"}}",
                          set);
        CHECK(!r.ok);
        const ValidationDiagnostic* d = find_code(r, "schema.union_tag_invalid");
        CHECK(d != nullptr);
        CHECK(d->pointer == "/collider/type");
    }
    {
        auto r = validate("{\"$schema\": \"test:thing\", \"version\": 1, \"name\": \"x\","
                          " \"collider\": {\"type\": \"shape:torus\"}}",
                          set);
        CHECK(!r.ok);
        CHECK(has_code(r, "schema.union_tag_unknown"));
    }
    {
        // The variant's own schema applies to the tagged object.
        auto r = validate("{\"$schema\": \"test:thing\", \"version\": 1, \"name\": \"x\","
                          " \"collider\": {\"type\": \"shape:circle\", \"radius\": \"wide\"}}",
                          set);
        CHECK(!r.ok);
        const ValidationDiagnostic* d = find_code(r, "schema.type_mismatch");
        CHECK(d != nullptr);
        CHECK(d->pointer == "/collider/radius");
    }

    // --- the engine scene kind accepts the scaffold-shaped template ------------------------------
    {
        auto r = validate(R"json({
            "$schema": "ctx:scene",
            "version": 1,
            "kind": "scene",
            "notes": "annotations live here (L-32)",
            "entities": [
                {"name": "MainCamera",
                 "components": {
                     "transform": {"position": [0, 1, -5]},
                     "camera": {"fov": 1.0471975511965976, "near": 0.1, "far": 1000}}}
            ]
        })json",
                          engine_schemas());
        CHECK(r.schema_bound);
        CHECK(r.ok);
        CHECK(r.diagnostics.empty());
    }

    // --- notes preservation through the canonical round-trip (the PR #45 serializer) -------------
    {
        const std::string authored = "{\n"
                                     "  \"$schema\": \"test:thing\",\n"
                                     "  \"version\": 1,\n"
                                     "  \"name\": \"ball\",\n"
                                     "  \"notes\": [\"why: gameplay tuning\", \"agent: v2\"]\n"
                                     "}\n";
        auto canonical = serializer::canonicalize(authored);
        CHECK(canonical.is_json);
        // The notes survive canonicalization byte-for-byte (values untouched, order stable).
        CHECK(canonical.bytes.find("\"why: gameplay tuning\"") != std::string::npos);
        CHECK(canonical.bytes.find("\"agent: v2\"") != std::string::npos);
        // Re-parse the canonical bytes and validate: still bound, still clean — the annotation
        // affordance is schema-blessed all the way through the round-trip.
        auto reparsed = serializer::parse_json(canonical.bytes);
        CHECK(reparsed.ok);
        auto r = validate_document(reparsed.root, canonical.bytes, set);
        CHECK(r.ok);
        // And the canonical form is a fixpoint (the round-trip changes nothing further).
        CHECK(serializer::canonicalize(canonical.bytes).bytes == canonical.bytes);
    }

    // --- locate_pointer --------------------------------------------------------------------------
    {
        const std::string doc = "{\n"
                                "  \"a~b\": {\"x/y\": [10, 20, 30]},\n"
                                "  \"plain\": true\n"
                                "}\n";
        std::size_t line = 0;
        std::size_t column = 0;
        CHECK(locate_pointer(doc, "", line, column));
        CHECK(line == 1);
        CHECK(column == 1);
        // RFC 6901 escaping: ~0 = '~', ~1 = '/'.
        CHECK(locate_pointer(doc, "/a~0b/x~1y/2", line, column));
        CHECK(line == 2);
        CHECK(column == 27); // the value 30
        CHECK(locate_pointer(doc, "/plain", line, column));
        CHECK(line == 3);
        CHECK(!locate_pointer(doc, "/missing", line, column));
        CHECK(!locate_pointer(doc, "/a~0b/x~1y/9", line, column)); // index out of range
        CHECK(!locate_pointer(doc, "/plain/deeper", line, column)); // descends into a scalar
    }

    SCHEMA_TEST_MAIN_END();
}
