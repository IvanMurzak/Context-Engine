// Kind-schema compilation (the vocabulary-law gate at DECLARATION time), the versioned
// registration set, the engine kinds, and the introspection projection.
// (R-QA-013: happy path, edge cases, AND failure paths.)

#include "context/editor/schema/kind_schema.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include "schema_test.h"

#include <string>
#include <vector>

using namespace context::editor::schema;

namespace
{
// A minimal valid kind schema to mutate from.
std::string minimal_schema(const std::string& extra_property = "")
{
    return std::string("{\"$id\": \"test:kind\", \"version\": 1, \"type\": \"object\", ") +
           "\"properties\": {\"notes\": {\"description\": \"blessed\"}" +
           (extra_property.empty() ? "" : ", " + extra_property) + "}}";
}

bool compiles(const std::string& schema_json)
{
    std::vector<std::string> problems;
    return compile_kind_schema(schema_json, problems).has_value();
}
} // namespace

int main()
{
    // --- happy path ------------------------------------------------------------------------------
    {
        std::vector<std::string> problems;
        auto schema = compile_kind_schema(minimal_schema(), problems);
        CHECK(schema.has_value());
        CHECK(problems.empty());
        CHECK(schema->id == "test:kind");
        CHECK(schema->version == 1);
        CHECK(!schema->canonical_doc.empty());
        // The published form is canonical (a fixpoint of the canonical serializer).
        CHECK(schema->canonical_doc ==
              context::editor::serializer::canonicalize(schema->canonical_doc).bytes);
    }

    // Vocabulary annotations compile when lawful.
    CHECK(compiles(minimal_schema(
        "\"rotation\": {\"x-ctx-type\": \"quaternion\", \"x-ctx-storage\": \"f32x4\"}")));
    CHECK(compiles(minimal_schema("\"speed\": {\"type\": \"number\", \"x-ctx-units\": \"m/s\"}")));
    CHECK(compiles(minimal_schema(
        "\"collider\": {\"x-ctx-union\": {\"shape:circle\": {\"type\": \"object\","
        " \"properties\": {\"radius\": {\"type\": \"number\", \"x-ctx-units\": \"m\"}}},"
        " \"shape:box\": {\"type\": \"object\"}}}")));
    CHECK(compiles(minimal_schema("\"mesh\": {\"x-ctx-ref\": \"ctx:mesh\"}")));

    // --- failure paths: the LAW rejects lawless declarations --------------------------------------
    // The units law: a non-SI unit CANNOT be declared (this is the whole point of R-DATA-006).
    CHECK(!compiles(minimal_schema("\"fov\": {\"type\": \"number\", \"x-ctx-units\": \"deg\"}")));
    // Unknown semantic type.
    CHECK(!compiles(minimal_schema("\"rot\": {\"x-ctx-type\": \"euler\"}")));
    // Malformed storage layout.
    CHECK(!compiles(minimal_schema("\"pos\": {\"x-ctx-storage\": \"f32x5\"}")));
    // Ad-hoc union tag (no namespace) — the per-package encoding the convention forbids.
    CHECK(!compiles(minimal_schema("\"c\": {\"x-ctx-union\": {\"circle\": {}}}")));
    // Ref without a target kind.
    CHECK(!compiles(minimal_schema("\"m\": {\"x-ctx-ref\": \"\"}")));
    // ref/union/semantic are mutually exclusive on one field.
    CHECK(!compiles(minimal_schema(
        "\"x\": {\"x-ctx-ref\": \"ctx:mesh\", \"x-ctx-type\": \"quaternion\"}")));
    // Unknown schema keyword (the dialect is pinned).
    CHECK(!compiles(minimal_schema("\"x\": {\"minimum\": 3}")));
    // Root must expose the blessed notes field (L-32).
    CHECK(!compiles("{\"$id\": \"test:kind\", \"version\": 1, \"type\": \"object\","
                    " \"properties\": {\"a\": {\"type\": \"string\"}}}"));
    // Root header requirements.
    CHECK(!compiles("{\"version\": 1, \"type\": \"object\","
                    " \"properties\": {\"notes\": {}}}")); // no $id
    CHECK(!compiles("{\"$id\": \"test:kind\", \"type\": \"object\","
                    " \"properties\": {\"notes\": {}}}")); // no version
    CHECK(!compiles("{\"$id\": \"test:kind\", \"version\": 0, \"type\": \"object\","
                    " \"properties\": {\"notes\": {}}}")); // version < 1
    CHECK(!compiles("{\"$id\": \"test:kind\", \"version\": 1, \"type\": \"array\","
                    " \"properties\": {\"notes\": {}}}")); // root is not an object kind
    // `required` names must be declared properties.
    CHECK(!compiles("{\"$id\": \"test:kind\", \"version\": 1, \"type\": \"object\","
                    " \"required\": [\"ghost\"], \"properties\": {\"notes\": {}}}"));
    // Not JSON at all.
    CHECK(!compiles("kind: test"));
    {
        // Problems carry the offending schema-document pointer.
        std::vector<std::string> problems;
        CHECK(!compile_kind_schema(
                   minimal_schema("\"fov\": {\"type\": \"number\", \"x-ctx-units\": \"deg\"}"),
                   problems)
                   .has_value());
        CHECK(problems.size() == 1);
        CHECK(problems[0].find("/properties/fov/x-ctx-units") != std::string::npos);
    }

    // --- the registration set --------------------------------------------------------------------
    {
        SchemaSet set;
        std::vector<std::string> problems;
        set.add(*compile_kind_schema(minimal_schema(), problems));
        set.add(*compile_kind_schema("{\"$id\": \"test:kind\", \"version\": 2,"
                                     " \"type\": \"object\","
                                     " \"properties\": {\"notes\": {}}}",
                                     problems));
        CHECK(problems.empty());
        CHECK(set.all().size() == 2);
        CHECK(set.find("test:kind", 1) != nullptr);
        CHECK(set.find("test:kind", 2) != nullptr);
        CHECK(set.find("test:kind", 3) == nullptr);
        CHECK(set.find("other:kind", 1) == nullptr);
        CHECK(set.latest("test:kind")->version == 2); // versioned selection
        CHECK(set.latest("other:kind") == nullptr);
        // Re-adding an (id, version) replaces in place (idempotent registration).
        set.add(*compile_kind_schema(minimal_schema(), problems));
        CHECK(set.all().size() == 2);
    }

    // --- the engine kinds ------------------------------------------------------------------------
    {
        const SchemaSet& engine = engine_schemas();
        CHECK(engine.all().size() >= 2);
        const KindSchema* scene = engine.latest(kSceneKindId);
        const KindSchema* project = engine.latest(kProjectKindId);
        CHECK(scene != nullptr);
        CHECK(project != nullptr);
        CHECK(scene->version == 1);
        CHECK(project->version == 1);
    }

    // --- introspection projection ----------------------------------------------------------------
    {
        const KindSchema* scene = engine_schemas().latest(kSceneKindId);
        const std::string entry = introspection_json(*scene);
        // The projection is canonical JSON and parses back.
        auto parsed = context::editor::serializer::parse_json(entry);
        CHECK(parsed.ok);
        CHECK(entry == context::editor::serializer::canonicalize(entry).bytes);
        // The units-law metadata is surfaced per field (R-DATA-006: introspection carries
        // x-ctx-units so agents render/reason without guessing).
        CHECK(entry.find("\"units\": \"rad\"") != std::string::npos); // camera fov
        CHECK(entry.find("\"units\": \"m\"") != std::string::npos);   // transform position
        CHECK(entry.find("\"storage\": \"f32x3\"") != std::string::npos);
        CHECK(entry.find("\"id\": \"ctx:scene\"") != std::string::npos);
        CHECK(entry.find("\"pointer\": \"/entities/[]/components/camera/fov\"") !=
              std::string::npos);
        // The blessed notes field is visible in the index.
        CHECK(entry.find("\"type\": \"notes\"") != std::string::npos);
    }

    // Typed-reference and tagged-union metadata are projected per field too (the registry's
    // `describe` fileKinds surface carries ref targets + declared union tags, R-DATA-006).
    {
        std::vector<std::string> problems;
        auto schema = compile_kind_schema(
            minimal_schema("\"mesh\": {\"x-ctx-ref\": \"ctx:mesh\"},"
                           " \"collider\": {\"x-ctx-union\": {"
                           "\"shape:circle\": {\"type\": \"object\","
                           " \"properties\": {\"radius\": {\"type\": \"number\","
                           " \"x-ctx-units\": \"m\"}}},"
                           " \"shape:box\": {\"type\": \"object\"}}}"),
            problems);
        CHECK(problems.empty());
        CHECK(schema.has_value());
        const std::string entry = introspection_json(*schema);
        CHECK(entry.find("\"ref\": \"ctx:mesh\"") != std::string::npos);
        CHECK(entry.find("\"unionTags\"") != std::string::npos);
        CHECK(entry.find("\"shape:circle\"") != std::string::npos);
        CHECK(entry.find("\"shape:box\"") != std::string::npos);
        // Union variant fields are indexed under the "(<tag>)" data-pointer convention.
        CHECK(entry.find("\"pointer\": \"/collider/(shape:circle)/radius\"") != std::string::npos);
    }

    SCHEMA_TEST_MAIN_END();
}
