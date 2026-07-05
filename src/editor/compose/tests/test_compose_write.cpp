// Composed WRITE path (L-35 / R-CLI-006, issue #58): the write-target selection matrix
// (default-outermost / --edit-template / --at-instance across nesting depths), idempotent override
// update, base snapshotting, the immutable-identity guard, failure paths (unknown target, bad
// --at-instance prefix), and a read-your-writes round-trip (apply the plan, re-flatten, assert the
// composed value moved). Happy + edge + failure coverage (R-QA-013).

#include "context/editor/compose/compose_write.h"

#include "context/editor/compose/flatten.h"
#include "context/editor/compose/json_pointer.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/serializer/canonical.h"

#include "compose_test.h"

#include <map>
#include <optional>
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

// A WriteResolver over an in-memory map of scene documents — the write counterpart of
// test_flatten.cpp's MapResolver, additionally exposing the raw parsed tree the write path splices.
class MapWriteResolver final : public WriteResolver
{
public:
    void add(const std::string& path, const char* json)
    {
        set_tree(path, parse(json));
    }
    // Replace a scene's tree (rebuilding its doc) — used to APPLY a plan and re-flatten (RYOW).
    void set_tree(const std::string& path, JsonValue tree)
    {
        std::optional<SceneDoc> doc = build_scene_doc(path, tree);
        CHECK(doc.has_value());
        trees_[path] = std::move(tree);
        docs_[path] = std::move(*doc);
    }
    [[nodiscard]] const SceneDoc* resolve(std::string_view path) const override
    {
        const auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const JsonValue* tree(std::string_view path) const override
    {
        const auto it = trees_.find(std::string(path));
        return it == trees_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, JsonValue, std::less<>> trees_;
    std::map<std::string, SceneDoc, std::less<>> docs_;
};

const std::string kInstA = "aaaaaaaaaaaaaaa1"; // root -> mid
const std::string kInstB = "bbbbbbbbbbbbbbb1"; // mid -> child
const std::string kEntC1 = "ccccccccccccccc1"; // child entity
const std::string kEntR1 = "eeeeeeeeeeeeeee1"; // root's own entity

// A fresh three-level scene set: root -> (instance A) mid -> (instance B) child{entity C1}.
void seed(MapWriteResolver& r)
{
    r.add("child.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [
        {"id": "ccccccccccccccc1", "name": "Light",
         "components": {"transform": {"position": [0, 0, 0]}}}
      ]})");
    r.add("mid.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [],
      "instances": [{"id": "bbbbbbbbbbbbbbb1", "scene": "child.scene.json"}]})");
    r.add("root.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "root": {"components": {"sky": {"color": "blue"}}},
      "entities": [{"id": "eeeeeeeeeeeeeee1", "name": "RootEnt", "components": {}}],
      "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "mid.scene.json"}]})");
}

[[nodiscard]] const JsonValue* at(const JsonValue& root, const std::string& pointer)
{
    return resolve_json_pointer(root, pointer);
}

// True iff `v` is a 3-element integer array [a, b, c] (the transform.position shape). Structural,
// not string-based: the canonical serializer emits multi-line indented JSON, so comparing values
// by canonical dump would be brittle.
[[nodiscard]] bool is_vec3(const JsonValue* v, long long a, long long b, long long c)
{
    if (v == nullptr || v->type != JsonValue::Type::array || v->elements.size() != 3)
        return false;
    const auto as_int = [](const JsonValue& e) -> long long {
        if (e.type == JsonValue::Type::integer)
            return e.int_value;
        if (e.type == JsonValue::Type::unsigned_integer)
            return static_cast<long long>(e.uint_value);
        return 0;
    };
    return as_int(v->elements[0]) == a && as_int(v->elements[1]) == b &&
           as_int(v->elements[2]) == c;
}

} // namespace

int main()
{
    // --- default (outermost): an override lands in the ROOT scene addressing the full id-path -----
    {
        MapWriteResolver r;
        seed(r);
        WriteRequest req;
        req.root_scene = "root.scene.json";
        req.id_path = {kInstA, kInstB, kEntC1};
        req.pointer = "/components/transform/position";
        req.value = parse("[9, 9, 9]");
        req.target = WriteTarget::outermost;

        WritePlan plan = plan_write(req, r);
        CHECK(plan.ok);
        CHECK(plan.file == "root.scene.json");
        CHECK(plan.pointer == "/overrides/0/value");
        CHECK(plan.base_recorded); // the child template authors position -> base snapshot taken

        const JsonValue* entry = at(plan.document, "/overrides/0");
        CHECK(entry != nullptr);
        // path == the full id-path from the root
        const JsonValue* path = at(plan.document, "/overrides/0/path");
        CHECK(path != nullptr && path->type == JsonValue::Type::array && path->elements.size() == 3);
        CHECK(path->elements[0].string_value == kInstA);
        CHECK(path->elements[2].string_value == kEntC1);
        CHECK(is_vec3(at(plan.document, "/overrides/0/value"), 9, 9, 9));
        CHECK(is_vec3(at(plan.document, "/overrides/0/base"), 0, 0, 0)); // the template value
        CHECK(at(plan.document, "/overrides/0/pointer")->string_value ==
              "/components/transform/position");

        // Read-your-writes: apply the plan + re-flatten -> the composed C1 now sits at [9,9,9].
        r.set_tree("root.scene.json", plan.document);
        ComposedScene flat = flatten("root.scene.json", r);
        CHECK(flat.ok);
        const ComposedEntity* c1 = nullptr;
        for (const ComposedEntity& e : flat.entities)
            if (e.id_path == std::vector<std::string>{kInstA, kInstB, kEntC1})
                c1 = &e;
        CHECK(c1 != nullptr);
        CHECK(is_vec3(at(c1->value, "/components/transform/position"), 9, 9, 9));
        // provenance now shows an override winning over the template (R-CLI-006 read side).
        std::vector<ProvenanceEntry> chain =
            provenance_for(*c1, "/components/transform/position");
        CHECK(chain.size() == 2);
        CHECK(chain[0].source == ProvenanceEntry::Source::override_value);
        CHECK(chain[0].file == "root.scene.json");
        CHECK(chain[1].source == ProvenanceEntry::Source::template_value);
    }

    // --- --edit-template: the write lands in the DEFINING scene (deepest), in place ---------------
    {
        MapWriteResolver r;
        seed(r);
        WriteRequest req;
        req.root_scene = "root.scene.json";
        req.id_path = {kInstA, kInstB, kEntC1};
        req.pointer = "/components/transform/position";
        req.value = parse("[5, 5, 5]");
        req.target = WriteTarget::defining_template;

        WritePlan plan = plan_write(req, r);
        CHECK(plan.ok);
        CHECK(plan.file == "child.scene.json");
        CHECK(plan.pointer == "/entities/0/components/transform/position");
        CHECK(!plan.base_recorded); // a template edit records no override base
        CHECK(is_vec3(at(plan.document, "/entities/0/components/transform/position"), 5, 5, 5));

        // Applying it moves the template value for EVERY instance of child (no override entry added).
        r.set_tree("child.scene.json", plan.document);
        ComposedScene flat = flatten("root.scene.json", r);
        const ComposedEntity* c1 = nullptr;
        for (const ComposedEntity& e : flat.entities)
            if (e.id_path == std::vector<std::string>{kInstA, kInstB, kEntC1})
                c1 = &e;
        CHECK(c1 != nullptr);
        CHECK(is_vec3(at(c1->value, "/components/transform/position"), 5, 5, 5));
    }

    // --- --at-instance <prefix>: the override lands in the mid-level scene, suffix-addressed ------
    {
        MapWriteResolver r;
        seed(r);
        WriteRequest req;
        req.root_scene = "root.scene.json";
        req.id_path = {kInstA, kInstB, kEntC1};
        req.pointer = "/components/transform/position";
        req.value = parse("[3, 3, 3]");
        req.target = WriteTarget::at_instance;
        req.at_instance = {kInstA}; // the scene instanced as A (mid.scene.json)

        WritePlan plan = plan_write(req, r);
        CHECK(plan.ok);
        CHECK(plan.file == "mid.scene.json"); // the addressing scene
        CHECK(plan.pointer == "/overrides/0/value");
        const JsonValue* path = at(plan.document, "/overrides/0/path");
        CHECK(path != nullptr && path->elements.size() == 2); // the SUFFIX [B, C1]
        CHECK(path->elements[0].string_value == kInstB);
        CHECK(path->elements[1].string_value == kEntC1);

        // Re-flatten with the mid override applied.
        r.set_tree("mid.scene.json", plan.document);
        ComposedScene flat = flatten("root.scene.json", r);
        const ComposedEntity* c1 = nullptr;
        for (const ComposedEntity& e : flat.entities)
            if (e.id_path == std::vector<std::string>{kInstA, kInstB, kEntC1})
                c1 = &e;
        CHECK(c1 != nullptr);
        CHECK(is_vec3(at(c1->value, "/components/transform/position"), 3, 3, 3));
    }

    // --- a root-native entity (single-segment id-path): edits the root scene in place ------------
    {
        MapWriteResolver r;
        seed(r);
        WriteRequest req;
        req.root_scene = "root.scene.json";
        req.id_path = {kEntR1};
        req.pointer = "/name";
        req.value = parse("\"Renamed\"");
        req.target = WriteTarget::outermost; // native entity -> in-place edit, not an override

        WritePlan plan = plan_write(req, r);
        CHECK(plan.ok);
        CHECK(plan.file == "root.scene.json");
        CHECK(plan.pointer == "/entities/0/name");
        CHECK(!plan.base_recorded);
        CHECK(at(plan.document, "/entities/0/name")->string_value == "Renamed");
        CHECK(at(plan.document, "/overrides") == nullptr); // no override array created
    }

    // --- the scene-root entity ($root, single segment): scene-settings edit in place -------------
    {
        MapWriteResolver r;
        seed(r);
        WriteRequest req;
        req.root_scene = "root.scene.json";
        req.id_path = {std::string("$root")};
        req.pointer = "/components/sky/color";
        req.value = parse("\"red\"");
        req.target = WriteTarget::outermost;

        WritePlan plan = plan_write(req, r);
        CHECK(plan.ok);
        CHECK(plan.file == "root.scene.json");
        CHECK(plan.pointer == "/root/components/sky/color");
        CHECK(at(plan.document, "/root/components/sky/color")->string_value == "red");
    }

    // --- idempotent update: a second set on the SAME path+pointer reuses the entry ---------------
    {
        MapWriteResolver r;
        seed(r);
        WriteRequest req;
        req.root_scene = "root.scene.json";
        req.id_path = {kInstA, kInstB, kEntC1};
        req.pointer = "/components/transform/position";
        req.value = parse("[9, 9, 9]");
        WritePlan first = plan_write(req, r);
        CHECK(first.ok);
        r.set_tree("root.scene.json", first.document); // apply

        req.value = parse("[8, 8, 8]");
        WritePlan second = plan_write(req, r);
        CHECK(second.ok);
        CHECK(second.pointer == "/overrides/0/value"); // SAME entry index (updated, not appended)
        // exactly one override entry after the update
        const JsonValue* overrides = at(second.document, "/overrides");
        CHECK(overrides != nullptr && overrides->elements.size() == 1);
        CHECK(is_vec3(at(second.document, "/overrides/0/value"), 8, 8, 8));
    }

    // --- introducing a NEW field: no template value -> no base recorded --------------------------
    {
        MapWriteResolver r;
        seed(r);
        WriteRequest req;
        req.root_scene = "root.scene.json";
        req.id_path = {kInstA, kInstB, kEntC1};
        req.pointer = "/components/tag"; // absent in the template
        req.value = parse("\"hero\"");
        WritePlan plan = plan_write(req, r);
        CHECK(plan.ok);
        CHECK(!plan.base_recorded);                          // nothing to snapshot
        CHECK(at(plan.document, "/overrides/0/base") == nullptr); // no base member written
        CHECK(at(plan.document, "/overrides/0/value")->string_value == "hero");
    }

    // --- failure: an immutable identity pointer is refused ---------------------------------------
    {
        MapWriteResolver r;
        seed(r);
        WriteRequest req;
        req.root_scene = "root.scene.json";
        req.id_path = {kInstA, kInstB, kEntC1};
        req.pointer = "/id";
        req.value = parse("\"nope\"");
        WritePlan plan = plan_write(req, r);
        CHECK(!plan.ok);
        CHECK(plan.error_code == "compose.immutable_pointer");
    }

    // --- failure: the id-path names no composed entity -------------------------------------------
    {
        MapWriteResolver r;
        seed(r);
        WriteRequest req;
        req.root_scene = "root.scene.json";
        req.id_path = {kInstA, kInstB, "ffffffffffffffff"}; // no such entity in child
        req.pointer = "/name";
        req.value = parse("\"x\"");
        WritePlan plan = plan_write(req, r);
        CHECK(!plan.ok);
        CHECK(plan.error_code == "compose.write_target_not_found");
    }

    // --- failure: --at-instance is not a strict prefix of the id-path ----------------------------
    {
        MapWriteResolver r;
        seed(r);
        WriteRequest req;
        req.root_scene = "root.scene.json";
        req.id_path = {kInstA, kInstB, kEntC1};
        req.pointer = "/name";
        req.value = parse("\"x\"");
        req.target = WriteTarget::at_instance;
        req.at_instance = {kInstB}; // B is not the FIRST segment of the id-path
        WritePlan plan = plan_write(req, r);
        CHECK(!plan.ok);
        CHECK(plan.error_code == "usage.invalid");
    }

    // --- failure: the root scene does not resolve ------------------------------------------------
    {
        MapWriteResolver r;
        seed(r);
        WriteRequest req;
        req.root_scene = "ghost.scene.json";
        req.id_path = {kEntR1};
        req.pointer = "/name";
        req.value = parse("\"x\"");
        WritePlan plan = plan_write(req, r);
        CHECK(!plan.ok);
        CHECK(plan.error_code == "file.not_found");
    }

    // --- is_immutable_pointer covers the three identity pointers ---------------------------------
    CHECK(is_immutable_pointer("/id"));
    CHECK(is_immutable_pointer("/$schema"));
    CHECK(is_immutable_pointer("/version"));
    CHECK(is_immutable_pointer("/id/nested"));
    CHECK(!is_immutable_pointer("/name"));
    CHECK(!is_immutable_pointer("/components/transform/position"));

    COMPOSE_TEST_MAIN_END();
}
