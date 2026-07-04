// The scene composition model: id-space rules (L-33), override-entry shapes (L-35), the
// scene-root entity, and kind binding. (R-QA-013: happy path, edge cases, AND failure paths.)

#include "context/editor/compose/scene_model.h"

#include "context/editor/serializer/canonical.h"

#include "compose_test.h"

#include <algorithm>
#include <string>

using namespace context::editor::compose;
namespace serializer = context::editor::serializer;
using serializer::JsonValue;

namespace
{

[[nodiscard]] JsonValue parse(const char* json)
{
    serializer::CanonicalizeResult r = serializer::canonicalize(json);
    CHECK(r.is_json);
    return r.root;
}

[[nodiscard]] std::size_t count_code(const SceneDoc& doc, const char* code)
{
    return static_cast<std::size_t>(
        std::count_if(doc.diagnostics.begin(), doc.diagnostics.end(),
                      [code](const ComposeDiagnostic& d) { return d.code == code; }));
}

} // namespace

int main()
{
    // --- kind binding: only ctx:scene documents enter composition ---------------------------------
    {
        CHECK(!build_scene_doc("a.json", parse(R"({"entities": []})")).has_value());
        CHECK(!build_scene_doc("a.json", parse(R"({"$schema": "ctx:project"})")).has_value());
        CHECK(!build_scene_doc("a.json", parse(R"([1, 2])")).has_value());
        CHECK(build_scene_doc("a.json", parse(R"({"$schema": "ctx:scene"})")).has_value());
    }

    // --- the full happy path -----------------------------------------------------------------------
    {
        const char* kScene = R"({
          "$schema": "ctx:scene",
          "version": 1,
          "root": {"id": "beefbeefbeefbeef", "composable": true, "components": {"sky": {}}},
          "entities": [
            {"id": "aaaaaaaaaaaaaaa1", "name": "Light", "components": {}},
            {"id": "aaaaaaaaaaaaaaa2", "name": "Camera", "components": {}}
          ],
          "instances": [
            {"id": "bbbbbbbbbbbbbbb1", "scene": "sub/child.scene.json"}
          ],
          "overrides": [
            {"path": ["bbbbbbbbbbbbbbb1", "ccccccccccccccc1"], "pointer": "/name", "value": "Renamed"},
            {"path": ["bbbbbbbbbbbbbbb1"], "add": {"id": "ddddddddddddddd1", "name": "Extra", "components": {}}},
            {"path": ["bbbbbbbbbbbbbbb1", "ccccccccccccccc2"], "remove": true},
            {"path": ["bbbbbbbbbbbbbbb1", "$root"], "pointer": "/components/sky/on", "value": true}
          ]
        })";
        std::optional<SceneDoc> doc = build_scene_doc("main.scene.json", parse(kScene));
        CHECK(doc.has_value());
        CHECK(doc->path == "main.scene.json");
        CHECK(doc->diagnostics.empty());
        CHECK(doc->root.present && doc->root.composable && doc->root.id == "beefbeefbeefbeef");
        CHECK(doc->root.pointer == "/root");
        CHECK(doc->entities.size() == 2 && doc->entities[0].id == "aaaaaaaaaaaaaaa1");
        CHECK(doc->entities[1].pointer == "/entities/1");
        CHECK(doc->instances.size() == 1 && doc->instances[0].scene == "sub/child.scene.json");
        CHECK(doc->overrides.size() == 4);
        CHECK(doc->overrides[0].kind == OverrideKind::field &&
              doc->overrides[0].field_pointer == "/name");
        CHECK(doc->overrides[1].kind == OverrideKind::add);
        CHECK(doc->overrides[2].kind == OverrideKind::remove);
        CHECK(doc->overrides[3].path.back() == "$root"); // the root token addresses sub-roots
        CHECK(doc->participates_in_composition());
    }

    // --- a plain M1-style scene neither participates nor diagnoses --------------------------------
    {
        std::optional<SceneDoc> doc = build_scene_doc(
            "plain.scene.json",
            parse(R"({"$schema": "ctx:scene", "version": 1,
                      "entities": [{"id": "aaaaaaaaaaaaaaa1", "name": "E", "components": {}}]})"));
        CHECK(doc.has_value());
        CHECK(!doc->participates_in_composition());
        CHECK(doc->diagnostics.empty());
    }

    // --- id-space rules: missing / invalid / duplicate (first claim wins) --------------------------
    {
        const char* kScene = R"({
          "$schema": "ctx:scene",
          "entities": [
            {"name": "NoId", "components": {}},
            {"id": "UPPERCASE0000001", "name": "BadId", "components": {}},
            {"id": "aaaaaaaaaaaaaaa1", "name": "First", "components": {}},
            {"id": "aaaaaaaaaaaaaaa1", "name": "Dup", "components": {}}
          ],
          "instances": [
            {"id": "aaaaaaaaaaaaaaa1", "scene": "x.scene.json"},
            {"id": "bbbbbbbbbbbbbbb1", "scene": "x.scene.json"},
            {"scene": "y.scene.json"}
          ]
        })";
        std::optional<SceneDoc> doc = build_scene_doc("ids.scene.json", parse(kScene));
        CHECK(doc.has_value());
        CHECK(doc->entities.size() == 1 && doc->entities[0].value.members.size() > 0);
        CHECK(doc->entities[0].id == "aaaaaaaaaaaaaaa1"); // the FIRST claim wins
        CHECK(doc->instances.size() == 1 && doc->instances[0].id == "bbbbbbbbbbbbbbb1");
        CHECK(count_code(*doc, "compose.missing_id") == 2);   // the id-less entity + instance
        CHECK(count_code(*doc, "compose.invalid_id") == 1);   // uppercase hex
        CHECK(count_code(*doc, "compose.duplicate_id") == 2); // dup entity + entity/instance clash
        // Duplicate ids are the BLOCKING class; the rest are advisory.
        for (const ComposeDiagnostic& d : doc->diagnostics)
            CHECK(d.blocking == (d.code == "compose.duplicate_id"));
    }

    // --- the root entity without an explicit id is addressable as $root ---------------------------
    {
        std::optional<SceneDoc> doc = build_scene_doc(
            "root.scene.json", parse(R"({"$schema": "ctx:scene", "entities": [],
                                         "root": {"components": {"sky": {}}}})"));
        CHECK(doc.has_value());
        CHECK(doc->root.present && doc->root.id.empty() && !doc->root.composable);
        CHECK(!doc->participates_in_composition()); // a non-composable root alone is inert
    }

    // --- malformed override entries: excluded, advisory-diagnosed ---------------------------------
    {
        const char* kScene = R"({
          "$schema": "ctx:scene",
          "entities": [],
          "instances": [{"id": "bbbbbbbbbbbbbbb1", "scene": "c.scene.json"}],
          "overrides": [
            {"pointer": "/name", "value": 1},
            {"path": [], "pointer": "/name", "value": 1},
            {"path": ["not an id"], "pointer": "/name", "value": 1},
            {"path": [42], "pointer": "/name", "value": 1},
            {"path": ["bbbbbbbbbbbbbbb1"], "pointer": "/name", "value": 1, "remove": true},
            {"path": ["bbbbbbbbbbbbbbb1"]},
            {"path": ["bbbbbbbbbbbbbbb1"], "remove": false},
            {"path": ["bbbbbbbbbbbbbbb1"], "add": "not an object"},
            {"path": ["bbbbbbbbbbbbbbb1"], "pointer": "/name"},
            {"path": ["bbbbbbbbbbbbbbb1"], "pointer": "not-a-pointer", "value": 1},
            {"path": ["bbbbbbbbbbbbbbb1", "ccccccccccccccc1"], "pointer": "/id", "value": "x"},
            {"path": ["bbbbbbbbbbbbbbb1", "ccccccccccccccc1"], "pointer": "/$schema", "value": "x"}
          ]
        })";
        std::optional<SceneDoc> doc = build_scene_doc("bad.scene.json", parse(kScene));
        CHECK(doc.has_value());
        CHECK(doc->overrides.empty());
        CHECK(count_code(*doc, "compose.override_malformed") == 12);
        for (const ComposeDiagnostic& d : doc->diagnostics)
            CHECK(!d.blocking);
    }

    // --- malformed instance entries: excluded, advisory-diagnosed ---------------------------------
    {
        const char* kScene = R"({
          "$schema": "ctx:scene",
          "entities": [],
          "instances": [
            {"id": "bbbbbbbbbbbbbbb1"},
            {"id": "bbbbbbbbbbbbbbb2", "scene": 42},
            {"id": "bbbbbbbbbbbbbbb3", "scene": ""},
            {"id": "bbbbbbbbbbbbbbb4", "scene": "ok.scene.json"}
          ]
        })";
        std::optional<SceneDoc> doc = build_scene_doc("inst.scene.json", parse(kScene));
        CHECK(doc.has_value());
        CHECK(doc->instances.size() == 1 && doc->instances[0].id == "bbbbbbbbbbbbbbb4");
        CHECK(count_code(*doc, "compose.override_malformed") == 3); // no / non-string / empty `scene`
        for (const ComposeDiagnostic& d : doc->diagnostics)
            CHECK(!d.blocking);
    }

    COMPOSE_TEST_MAIN_END();
}
