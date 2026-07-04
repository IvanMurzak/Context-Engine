// Flatten happy paths (L-35/R-DATA-002): instancing, innermost-out override precedence (the
// outermost scene wins), structural add/remove, the scene-root entity, deterministic composed
// identity (L-37), and winning-value-first provenance chains (R-CLI-006 read side).

#include "context/editor/compose/flatten.h"

#include "context/editor/compose/json_pointer.h"
#include "context/editor/compose/stable_id.h"
#include "context/editor/serializer/canonical.h"

#include "compose_test.h"

#include <map>
#include <string>
#include <vector>

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

class MapResolver final : public SceneResolver
{
public:
    void add(const char* path, const char* json)
    {
        std::optional<SceneDoc> doc = build_scene_doc(path, parse(json));
        CHECK(doc.has_value());
        docs_[path] = std::move(*doc);
    }
    [[nodiscard]] const SceneDoc* resolve(std::string_view path) const override
    {
        auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, SceneDoc, std::less<>> docs_;
};

[[nodiscard]] const ComposedEntity* find_entity(const ComposedScene& scene,
                                                const std::vector<std::string>& id_path)
{
    for (const ComposedEntity& e : scene.entities)
        if (e.id_path == id_path)
            return &e;
    return nullptr;
}

// Stable ids used across the fixtures (16 lowercase hex chars each).
const std::string kInstA = "aaaaaaaaaaaaaaa1";  // root -> mid
const std::string kInstB = "bbbbbbbbbbbbbbb1";  // mid -> child
const std::string kEntC1 = "ccccccccccccccc1";  // child entity (overridden)
const std::string kEntC2 = "ccccccccccccccc2";  // child entity (untouched)
const std::string kEntR1 = "eeeeeeeeeeeeeee1";  // root's own entity
const std::string kAdded = "ddddddddddddddd1";  // structurally added entity

} // namespace

int main()
{
    // --- degenerate flatten: a scene with no composition members ----------------------------------
    {
        MapResolver r;
        r.add("solo.scene.json", R"({
          "$schema": "ctx:scene", "version": 1,
          "entities": [
            {"id": "ccccccccccccccc1", "name": "A", "components": {}},
            {"id": "ccccccccccccccc2", "name": "B", "components": {}}
          ]})");
        ComposedScene out = flatten("solo.scene.json", r);
        CHECK(out.ok);
        CHECK(out.diagnostics.empty());
        CHECK(out.entities.size() == 2);
        CHECK(out.entities[0].id_path == std::vector<std::string>{kEntC1});
        CHECK(out.entities[0].identity_hash != out.entities[1].identity_hash);
        // Un-overridden pointers fall back to the template origin.
        std::vector<ProvenanceEntry> chain = provenance_for(out.entities[0], "/name");
        CHECK(chain.size() == 1);
        CHECK(chain[0].source == ProvenanceEntry::Source::template_value);
        CHECK(chain[0].file == "solo.scene.json");
        CHECK(chain[0].pointer == "/entities/0/name");
        CHECK(chain[0].level == 0);
    }

    // --- the three-level precedence chain: the OUTERMOST instancing scene wins --------------------
    MapResolver r;
    r.add("child.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [
        {"id": "ccccccccccccccc1", "name": "Light",
         "components": {"transform": {"position": [0, 0, 0]}}},
        {"id": "ccccccccccccccc2", "name": "Prop",
         "components": {"transform": {"position": [7, 7, 7]}}}
      ]})");
    r.add("mid.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [],
      "instances": [{"id": "bbbbbbbbbbbbbbb1", "scene": "child.scene.json"}],
      "overrides": [
        {"path": ["bbbbbbbbbbbbbbb1", "ccccccccccccccc1"],
         "pointer": "/components/transform/position", "value": [1, 1, 1]}
      ]})");
    r.add("root.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "root": {"components": {"sky": {"color": "blue"}}},
      "entities": [{"id": "eeeeeeeeeeeeeee1", "name": "RootEnt", "components": {}}],
      "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "mid.scene.json"}],
      "overrides": [
        {"path": ["aaaaaaaaaaaaaaa1", "bbbbbbbbbbbbbbb1", "ccccccccccccccc1"],
         "pointer": "/components/transform/position", "value": [2, 2, 2]},
        {"path": ["aaaaaaaaaaaaaaa1", "bbbbbbbbbbbbbbb1", "ccccccccccccccc1"],
         "pointer": "/name", "value": "Beacon"},
        {"path": ["aaaaaaaaaaaaaaa1", "bbbbbbbbbbbbbbb1"],
         "add": {"id": "ddddddddddddddd1", "name": "Extra", "components": {}}},
        {"path": ["aaaaaaaaaaaaaaa1", "bbbbbbbbbbbbbbb1", "ddddddddddddddd1"],
         "pointer": "/name", "value": "ExtraRenamed"}
      ]})");

    ComposedScene out = flatten("root.scene.json", r);
    CHECK(out.ok);
    CHECK(out.diagnostics.empty());

    // Expansion order: root's $root, root's entity, then the instance subtree depth-first
    // (child entities, then the structural add).
    CHECK(out.entities.size() == 5);
    CHECK(out.entities[0].id_path == std::vector<std::string>{"$root"});
    CHECK(out.entities[1].id_path == std::vector<std::string>{kEntR1});

    // The overridden entity: the OUTERMOST value won.
    const ComposedEntity* light = find_entity(out, {kInstA, kInstB, kEntC1});
    CHECK(light != nullptr);
    const JsonValue* pos = resolve_json_pointer(light->value, "/components/transform/position/0");
    CHECK(pos != nullptr && pos->int_value == 2);
    const JsonValue* name = resolve_json_pointer(light->value, "/name");
    CHECK(name != nullptr && name->string_value == "Beacon");
    CHECK(light->template_file == "child.scene.json");
    CHECK(light->template_pointer == "/entities/0");
    CHECK(light->template_level == 2);

    // The untouched sibling flows through from the template.
    const ComposedEntity* prop = find_entity(out, {kInstA, kInstB, kEntC2});
    CHECK(prop != nullptr);
    CHECK(resolve_json_pointer(prop->value, "/components/transform/position/0")->int_value == 7);

    // Provenance for the twice-overridden pointer: winning-value-first, then the mid-level
    // contributor, then the defining template (R-CLI-006: every contributor visible).
    std::vector<ProvenanceEntry> chain =
        provenance_for(*light, "/components/transform/position");
    CHECK(chain.size() == 3);
    CHECK(chain[0].source == ProvenanceEntry::Source::override_value);
    CHECK(chain[0].file == "root.scene.json" && chain[0].level == 0);
    CHECK(chain[0].pointer == "/overrides/0/value");
    CHECK(chain[1].source == ProvenanceEntry::Source::override_value);
    CHECK(chain[1].file == "mid.scene.json" && chain[1].level == 1);
    CHECK(chain[2].source == ProvenanceEntry::Source::template_value);
    CHECK(chain[2].file == "child.scene.json" && chain[2].level == 2);
    CHECK(chain[2].pointer == "/entities/0/components/transform/position");

    // The structural add composed under the instance subtree, then its own field override
    // applied; its baseline provenance is the add entry itself (an override-sourced template).
    const ComposedEntity* extra = find_entity(out, {kInstA, kInstB, kAdded});
    CHECK(extra != nullptr);
    CHECK(resolve_json_pointer(extra->value, "/name")->string_value == "ExtraRenamed");
    CHECK(extra->template_file == "root.scene.json");
    CHECK(extra->template_pointer == "/overrides/2/add");
    std::vector<ProvenanceEntry> added_chain = provenance_for(*extra, "/name");
    CHECK(added_chain.size() == 2);
    CHECK(added_chain[0].source == ProvenanceEntry::Source::override_value);
    CHECK(added_chain[0].pointer == "/overrides/3/value");
    CHECK(added_chain[1].source == ProvenanceEntry::Source::override_value); // the add baseline
    CHECK(added_chain[1].pointer == "/overrides/2/add/name");

    // --- deterministic composed identity (L-37): stable across re-derivation ----------------------
    {
        ComposedScene again = flatten("root.scene.json", r);
        CHECK(again.entities.size() == out.entities.size());
        for (std::size_t i = 0; i < out.entities.size(); ++i)
        {
            CHECK(again.entities[i].id_path == out.entities[i].id_path);
            CHECK(again.entities[i].identity_hash == out.entities[i].identity_hash);
        }
        CHECK(identity_hash_of("root.scene.json", light->id_path) == light->identity_hash);
        // Identity binds the ROOT scene too: the same id-path under another root differs.
        CHECK(identity_hash_of("other.scene.json", light->id_path) != light->identity_hash);
    }

    // --- structural remove: entity-level and whole-subtree ----------------------------------------
    {
        MapResolver r2;
        r2.add("child.scene.json", R"({
          "$schema": "ctx:scene", "version": 1,
          "entities": [
            {"id": "ccccccccccccccc1", "name": "A", "components": {}},
            {"id": "ccccccccccccccc2", "name": "B", "components": {}}
          ]})");
        r2.add("two.scene.json", R"({
          "$schema": "ctx:scene", "version": 1,
          "entities": [],
          "instances": [
            {"id": "aaaaaaaaaaaaaaa1", "scene": "child.scene.json"},
            {"id": "aaaaaaaaaaaaaaa2", "scene": "child.scene.json"}
          ],
          "overrides": [
            {"path": ["aaaaaaaaaaaaaaa1", "ccccccccccccccc1"], "remove": true},
            {"path": ["aaaaaaaaaaaaaaa2"], "remove": true}
          ]})");
        ComposedScene removed = flatten("two.scene.json", r2);
        CHECK(removed.ok);
        CHECK(removed.diagnostics.empty());
        CHECK(removed.entities.size() == 1); // only [instA, c2] survives
        CHECK(removed.entities[0].id_path == (std::vector<std::string>{"aaaaaaaaaaaaaaa1", kEntC2}));
    }

    // --- the scene-root entity: inert by default, composable opt-in -------------------------------
    {
        MapResolver r3;
        r3.add("inert.scene.json", R"({
          "$schema": "ctx:scene", "version": 1, "entities": [],
          "root": {"components": {"physics": {"gravity": [0, -9.81, 0]}}}})");
        r3.add("optin.scene.json", R"({
          "$schema": "ctx:scene", "version": 1, "entities": [],
          "root": {"composable": true, "components": {"weather": {"rain": true}}}})");
        r3.add("host.scene.json", R"({
          "$schema": "ctx:scene", "version": 1, "entities": [],
          "instances": [
            {"id": "aaaaaaaaaaaaaaa1", "scene": "inert.scene.json"},
            {"id": "aaaaaaaaaaaaaaa2", "scene": "optin.scene.json"}
          ],
          "overrides": [
            {"path": ["aaaaaaaaaaaaaaa2", "$root"], "pointer": "/components/weather/rain",
             "value": false}
          ]})");
        ComposedScene roots = flatten("host.scene.json", r3);
        CHECK(roots.ok);
        CHECK(roots.diagnostics.empty());
        CHECK(roots.entities.size() == 1); // the inert root stayed inert
        const ComposedEntity* weather = find_entity(roots, {"aaaaaaaaaaaaaaa2", "$root"});
        CHECK(weather != nullptr);
        const JsonValue* rain = resolve_json_pointer(weather->value, "/components/weather/rain");
        CHECK(rain != nullptr && !rain->boolean_value); // the $root-addressed override applied
    }

    // --- the canonical-JSON emitters (the R-CLI-006 read-side result shapes) ----------------------
    {
        const std::string chain_json =
            provenance_json(provenance_for(*light, "/components/transform/position"));
        const std::size_t first = chain_json.find("\"source\": \"override\"");
        const std::size_t last = chain_json.find("\"source\": \"template\"");
        CHECK(first != std::string::npos && last != std::string::npos);
        CHECK(first < last); // winning-value-first ordering survives serialization
        CHECK(chain_json.find("\"level\": 0") != std::string::npos);
        CHECK(chain_json.find("\"level\": 2") != std::string::npos);

        const std::string scene_json = composed_scene_json(out);
        CHECK(scene_json.find("\"rootScene\": \"root.scene.json\"") != std::string::npos);
        CHECK(scene_json.find("\"ok\": true") != std::string::npos);
        CHECK(scene_json.find("\"idPath\"") != std::string::npos);
        CHECK(scene_json.find("\"identityHash\"") != std::string::npos);
        CHECK(scene_json.find(format_stable_id(light->identity_hash)) != std::string::npos);
        // The emitted form is canonical JSON — parseable and a fixpoint.
        serializer::CanonicalizeResult reparsed = serializer::canonicalize(scene_json);
        CHECK(reparsed.is_json);
        CHECK(reparsed.bytes == scene_json);
    }

    COMPOSE_TEST_MAIN_END();
}
