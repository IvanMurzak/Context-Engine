// Dual-form reference check + heal tests (R-QA-013): dangling-$ref, path-only acceptance +
// resolution, stale/absent hint healing, idempotence (healed output re-heals to itself),
// union-nested refs, entity-ref skipping, and the entity-ref shape helpers.

#include "context/editor/assetdb/ref_heal.h"

#include "context/editor/filesync/file_store.h"
#include "context/editor/schema/kind_schema.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include "assetdb_test.h"

#include <string>
#include <string_view>
#include <vector>

using namespace context::editor::assetdb;
namespace filesync = context::editor::filesync;
namespace schema = context::editor::schema;
namespace serializer = context::editor::serializer;

namespace
{

constexpr std::string_view kGuidMesh = "00000000000000000000000000000bbb";
constexpr std::string_view kGuidTexture = "00000000000000000000000000000ccc";

void put_asset(filesync::FileStore& fs, std::string_view path, std::string_view guid,
               std::string_view kind)
{
    fs.write(path, "{}\n");
    AssetMeta meta;
    meta.guid = std::string(guid);
    meta.kind = std::string(kind);
    fs.write(meta_path_for(path), serialize_meta(meta));
}

const schema::SchemaSet& test_set()
{
    static const schema::SchemaSet set = []
    {
        schema::SchemaSet s;
        std::vector<std::string> problems;
        auto kind = schema::compile_kind_schema(R"({
            "$id": "test:scene",
            "version": 1,
            "type": "object",
            "properties": {
                "notes": {"description": "blessed"},
                "mesh": {"x-ctx-ref": "ctx:mesh"},
                "layers": {"type": "array", "items": {"x-ctx-ref": "ctx:texture"}},
                "surface": {"x-ctx-union": {
                    "mat:textured": {"type": "object", "properties": {
                        "albedo": {"x-ctx-ref": "ctx:texture"}}},
                    "mat:flat": {"type": "object"}}},
                "parent": {"x-ctx-ref": "ctx:scene"}
            }
        })",
                                                problems);
        if (kind.has_value() && problems.empty())
            s.add(std::move(*kind));
        return s;
    }();
    return set;
}

bool has_finding(const std::vector<RefFinding>& findings, std::string_view code,
                 std::string_view pointer = "")
{
    for (const RefFinding& f : findings)
        if (f.code == code && (pointer.empty() || f.pointer == pointer))
            return true;
    return false;
}

bool has_action(const std::vector<RefHealAction>& actions, std::string_view action,
                std::string_view pointer = "")
{
    for (const RefHealAction& a : actions)
        if (a.action == action && (pointer.empty() || a.pointer == pointer))
            return true;
    return false;
}

[[nodiscard]] std::string canonical(const serializer::JsonValue& root)
{
    std::string out;
    CHECK(serializer::serialize_canonical(root, out));
    return out;
}

} // namespace

int main()
{
    // The index the checks and heals resolve against.
    filesync::MemoryFileStore fs;
    put_asset(fs, "proj/meshes/rock.json", kGuidMesh, "ctx:mesh");
    put_asset(fs, "proj/textures/wood.json", kGuidTexture, "ctx:texture");
    SequenceGuidGenerator guids;
    AssetDatabase db(guids);
    CHECK(db.scan(fs, "proj").assets_indexed == 2);
    const schema::SchemaSet& set = test_set();
    CHECK(set.latest("test:scene") != nullptr);

    // --- check: a correct dual-form document is silent ---------------------------------------------
    {
        auto parsed = serializer::parse_json(
            "{\"$schema\": \"test:scene\", \"version\": 1, \"mesh\": "
            "{\"$ref\": \"00000000000000000000000000000bbb\", \"path\": "
            "\"proj/meshes/rock.json\"}}");
        CHECK(parsed.ok);
        CHECK(check_document_refs(parsed.root, set, db).empty());
    }

    // --- check: dangling $ref, path-only (resolvable + not), stale hint ----------------------------
    {
        auto parsed = serializer::parse_json(R"({
            "$schema": "test:scene", "version": 1,
            "mesh": {"$ref": "00000000000000000000000000000fff"},
            "layers": [
                {"path": "proj/textures/wood.json"},
                {"path": "proj/textures/nope.json"}
            ],
            "parent": {"$ref": "00000000000000000000000000000bbb", "path": "proj/old/rock.json"}
        })");
        CHECK(parsed.ok);
        const auto findings = check_document_refs(parsed.root, set, db);
        CHECK(has_finding(findings, "asset.ref_dangling", "/mesh"));      // unknown GUID
        CHECK(has_finding(findings, "asset.ref_path_only", "/layers/0")); // accepted, awaiting fix
        CHECK(has_finding(findings, "asset.ref_dangling", "/layers/1"));  // path resolves nowhere
        CHECK(has_finding(findings, "asset.ref_hint_stale", "/parent"));  // hint drifted
        CHECK(findings.size() == 4);
    }

    // --- check/heal: same-file entity refs are out of scope here -----------------------------------
    {
        auto parsed = serializer::parse_json("{\"$schema\": \"test:scene\", \"version\": 1, "
                                             "\"mesh\": {\"$entity\": \"e42\"}}");
        CHECK(parsed.ok);
        CHECK(check_document_refs(parsed.root, set, db).empty());
        const auto healed = heal_document_refs(parsed.root, set, db);
        CHECK(healed.actions.empty());
        CHECK(healed.findings.empty());
    }

    // --- check: an unbound document (no $schema) is a no-op ----------------------------------------
    {
        auto parsed = serializer::parse_json("{\"mesh\": {\"$ref\": \"deadbeef\"}}");
        CHECK(parsed.ok);
        CHECK(check_document_refs(parsed.root, set, db).empty());
    }

    // --- heal: path-only -> dual form; stale hint refreshed; absent hint added; union nesting ------
    {
        auto parsed = serializer::parse_json(R"({
            "$schema": "test:scene", "version": 1,
            "mesh": {"$ref": "00000000000000000000000000000bbb"},
            "layers": [{"path": "proj/textures/wood.json"}],
            "parent": {"$ref": "00000000000000000000000000000bbb", "path": "proj/old/rock.json"},
            "surface": {"type": "mat:textured",
                        "albedo": {"path": "proj/textures/wood.json"}}
        })");
        CHECK(parsed.ok);
        const auto healed = heal_document_refs(parsed.root, set, db);
        CHECK(healed.findings.empty());
        CHECK(has_action(healed.actions, "hint-added", "/mesh"));
        CHECK(has_action(healed.actions, "guid-resolved", "/layers/0"));
        CHECK(has_action(healed.actions, "hint-updated", "/parent"));
        CHECK(has_action(healed.actions, "guid-resolved", "/surface/albedo")); // union variant
        CHECK(healed.actions.size() == 4);

        const std::string bytes = canonical(parsed.root);
        // The healed document is the L-34 dual form everywhere.
        CHECK(bytes.find("\"$ref\": \"00000000000000000000000000000ccc\"") != std::string::npos);
        CHECK(bytes.find("\"path\": \"proj/meshes/rock.json\"") != std::string::npos);
        CHECK(bytes.find("proj/old/rock.json") == std::string::npos); // stale hint gone

        // Post-heal, the checker is silent — and the validator's dual-form shape holds.
        CHECK(check_document_refs(parsed.root, set, db).empty());

        // IDEMPOTENCE (the R-FILE-003 write-surface law): healed output re-heals to itself.
        auto reparsed = serializer::parse_json(bytes);
        CHECK(reparsed.ok);
        const auto again = heal_document_refs(reparsed.root, set, db);
        CHECK(again.actions.empty());
        CHECK(again.findings.empty());
        CHECK(canonical(reparsed.root) == bytes); // byte-identical fixpoint
    }

    // --- heal: dangling refs are findings, never guesses -------------------------------------------
    {
        auto parsed = serializer::parse_json(R"({
            "$schema": "test:scene", "version": 1,
            "mesh": {"$ref": "00000000000000000000000000000fff", "path": "proj/x.json"},
            "layers": [{"path": "proj/nowhere.json"}]
        })");
        CHECK(parsed.ok);
        const std::string before = canonical(parsed.root);
        const auto healed = heal_document_refs(parsed.root, set, db);
        CHECK(healed.actions.empty());
        CHECK(has_finding(healed.findings, "asset.ref_dangling", "/mesh"));
        CHECK(has_finding(healed.findings, "asset.ref_dangling", "/layers/0"));
        CHECK(canonical(parsed.root) == before); // untouched
    }

    // --- entity-ref shape helpers (L-34's sibling forms) --------------------------------------------
    {
        auto entity = serializer::parse_json("{\"$entity\": \"e1\"}");
        CHECK(entity.ok);
        CHECK(is_entity_ref(entity.root));
        CHECK(!is_entity_id_path(entity.root));

        auto id_path = serializer::parse_json("{\"$entity\": [\"instA\", \"instB\", \"e9\"]}");
        CHECK(id_path.ok);
        CHECK(is_entity_ref(id_path.root)); // the instanced-subtree form
        auto two = serializer::parse_json("[\"instA\", \"e9\"]");
        CHECK(two.ok);
        CHECK(is_entity_id_path(two.root)); // minimum: one instance hop + the entity

        // Failure shapes: empty id, lone element (the string form's job), non-string elements,
        // extra members, wrong types.
        auto empty_id = serializer::parse_json("{\"$entity\": \"\"}");
        CHECK(empty_id.ok);
        CHECK(!is_entity_ref(empty_id.root));
        auto lone = serializer::parse_json("[\"e9\"]");
        CHECK(lone.ok);
        CHECK(!is_entity_id_path(lone.root));
        auto mixed = serializer::parse_json("[\"instA\", 7]");
        CHECK(mixed.ok);
        CHECK(!is_entity_id_path(mixed.root));
        auto extra = serializer::parse_json("{\"$entity\": \"e1\", \"path\": \"x\"}");
        CHECK(extra.ok);
        CHECK(!is_entity_ref(extra.root)); // exactly one member (the validator's same-file shape)
        auto not_ref = serializer::parse_json("{\"$ref\": \"00000000000000000000000000000bbb\"}");
        CHECK(not_ref.ok);
        CHECK(!is_entity_ref(not_ref.root));
    }

    ASSETDB_TEST_MAIN_END();
}
