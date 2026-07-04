// Flatten failure paths: missing scenes, instancing cycles, the R-FILE-011(e) depth cap + fan-in
// diagnostic + entity budget, orphan overrides (L-37), and intra-file id findings surfacing
// through the flatten. (R-QA-013: failure paths are first-class.)

#include "context/editor/compose/flatten.h"

#include "context/editor/serializer/canonical.h"

#include "compose_test.h"

#include <algorithm>
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

[[nodiscard]] std::size_t count_code(const ComposedScene& scene, const char* code)
{
    return static_cast<std::size_t>(
        std::count_if(scene.diagnostics.begin(), scene.diagnostics.end(),
                      [code](const ComposeDiagnostic& d) { return d.code == code; }));
}

} // namespace

int main()
{
    // --- a missing flatten root --------------------------------------------------------------------
    {
        MapResolver r;
        ComposedScene out = flatten("nowhere.scene.json", r);
        CHECK(!out.ok);
        CHECK(out.entities.empty());
        CHECK(count_code(out, "compose.missing_scene") == 1);
    }

    // --- a missing instanced scene: blocking, but the rest still composes -------------------------
    {
        MapResolver r;
        r.add("root.scene.json", R"({
          "$schema": "ctx:scene", "version": 1,
          "entities": [{"id": "eeeeeeeeeeeeeee1", "name": "Here", "components": {}}],
          "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "gone.scene.json"}],
          "overrides": [
            {"path": ["aaaaaaaaaaaaaaa1", "ccccccccccccccc1"], "pointer": "/name", "value": "X"}
          ]})");
        ComposedScene out = flatten("root.scene.json", r);
        CHECK(!out.ok);
        CHECK(out.entities.size() == 1); // best-effort: the local entity composed
        CHECK(count_code(out, "compose.missing_scene") == 1);
        // The override narrowed into the unexpandable subtree is suppressed, not an orphan.
        CHECK(count_code(out, "compose.orphan_override") == 0);
    }

    // --- direct cycle -------------------------------------------------------------------------------
    {
        MapResolver r;
        r.add("self.scene.json", R"({
          "$schema": "ctx:scene", "version": 1,
          "entities": [{"id": "ccccccccccccccc1", "name": "E", "components": {}}],
          "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "self.scene.json"}]})");
        ComposedScene out = flatten("self.scene.json", r);
        CHECK(!out.ok);
        CHECK(count_code(out, "compose.cycle") == 1);
        CHECK(out.entities.size() == 1); // the first visit composed; the cycle did not expand
    }

    // --- indirect cycle (a -> b -> a) ---------------------------------------------------------------
    {
        MapResolver r;
        r.add("a.scene.json", R"({
          "$schema": "ctx:scene", "version": 1,
          "entities": [{"id": "ccccccccccccccc1", "name": "InA", "components": {}}],
          "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "b.scene.json"}]})");
        r.add("b.scene.json", R"({
          "$schema": "ctx:scene", "version": 1,
          "entities": [{"id": "ccccccccccccccc2", "name": "InB", "components": {}}],
          "instances": [{"id": "bbbbbbbbbbbbbbb1", "scene": "a.scene.json"}]})");
        ComposedScene out = flatten("a.scene.json", r);
        CHECK(!out.ok);
        CHECK(count_code(out, "compose.cycle") == 1);
        CHECK(out.entities.size() == 2); // a's entity + b's entity, then the cycle stopped
    }

    // --- depth cap boundary (R-FILE-011(e)) ---------------------------------------------------------
    {
        MapResolver r;
        r.add("d0.scene.json", R"({
          "$schema": "ctx:scene", "version": 1, "entities": [],
          "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "d1.scene.json"}]})");
        r.add("d1.scene.json", R"({
          "$schema": "ctx:scene", "version": 1, "entities": [],
          "instances": [{"id": "aaaaaaaaaaaaaaa2", "scene": "d2.scene.json"}]})");
        r.add("d2.scene.json", R"({
          "$schema": "ctx:scene", "version": 1,
          "entities": [{"id": "ccccccccccccccc1", "name": "Leaf", "components": {}}]})");

        ComposeLimits at_cap;
        at_cap.max_depth = 2; // d0(0) -> d1(1) -> d2(2): exactly at the cap composes
        ComposedScene ok = flatten("d0.scene.json", r, at_cap);
        CHECK(ok.ok);
        CHECK(ok.entities.size() == 1);
        CHECK(count_code(ok, "compose.depth_exceeded") == 0);

        ComposeLimits under;
        under.max_depth = 1; // the d1 -> d2 hop would be depth 2 > 1
        ComposedScene blocked = flatten("d0.scene.json", r, under);
        CHECK(!blocked.ok);
        CHECK(blocked.entities.empty());
        CHECK(count_code(blocked, "compose.depth_exceeded") == 1);
    }

    // --- fan-in threshold (advisory, once per file) -------------------------------------------------
    {
        MapResolver r;
        r.add("leaf.scene.json", R"({
          "$schema": "ctx:scene", "version": 1,
          "entities": [{"id": "ccccccccccccccc1", "name": "L", "components": {}}]})");
        r.add("hub.scene.json", R"({
          "$schema": "ctx:scene", "version": 1, "entities": [],
          "instances": [
            {"id": "aaaaaaaaaaaaaaa1", "scene": "leaf.scene.json"},
            {"id": "aaaaaaaaaaaaaaa2", "scene": "leaf.scene.json"},
            {"id": "aaaaaaaaaaaaaaa3", "scene": "leaf.scene.json"}
          ]})");
        ComposeLimits tight;
        tight.fan_in_threshold = 3;
        ComposedScene out = flatten("hub.scene.json", r, tight);
        CHECK(out.ok); // fan-in is ADVISORY
        CHECK(out.entities.size() == 3);
        CHECK(count_code(out, "compose.fan_in") == 1); // reported once, at the crossing

        ComposeLimits loose;
        loose.fan_in_threshold = 4;
        CHECK(count_code(flatten("hub.scene.json", r, loose), "compose.fan_in") == 0);
    }

    // --- intra-file id findings surface through the flatten (once per file) -----------------------
    {
        MapResolver r;
        r.add("dup.scene.json", R"({
          "$schema": "ctx:scene", "version": 1,
          "entities": [
            {"id": "ccccccccccccccc1", "name": "First", "components": {}},
            {"id": "ccccccccccccccc1", "name": "Dup", "components": {}},
            {"name": "NoId", "components": {}}
          ]})");
        r.add("host.scene.json", R"({
          "$schema": "ctx:scene", "version": 1, "entities": [],
          "instances": [
            {"id": "aaaaaaaaaaaaaaa1", "scene": "dup.scene.json"},
            {"id": "aaaaaaaaaaaaaaa2", "scene": "dup.scene.json"}
          ]})");
        ComposedScene out = flatten("host.scene.json", r);
        CHECK(!out.ok);                                    // duplicate ids are blocking
        CHECK(count_code(out, "compose.duplicate_id") == 1); // once per FILE, not per expansion
        CHECK(count_code(out, "compose.missing_id") == 1);
        CHECK(out.entities.size() == 2); // the first claim composed under each instance
    }

    // --- orphan overrides (L-37: excluded from flatten, advisory) ----------------------------------
    {
        MapResolver r;
        r.add("child.scene.json", R"({
          "$schema": "ctx:scene", "version": 1,
          "entities": [{"id": "ccccccccccccccc1", "name": "E",
                        "components": {"transform": {"position": [0, 0, 0]}}}]})");
        r.add("root.scene.json", R"({
          "$schema": "ctx:scene", "version": 1, "entities": [],
          "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "child.scene.json"}],
          "overrides": [
            {"path": ["ffffffffffffffff", "ccccccccccccccc1"], "pointer": "/name", "value": "X"},
            {"path": ["aaaaaaaaaaaaaaa1", "eeeeeeeeeeeeeee9"], "pointer": "/name", "value": "X"},
            {"path": ["aaaaaaaaaaaaaaa1", "ccccccccccccccc1"],
             "pointer": "/components/transform/position/9", "value": 1},
            {"path": ["aaaaaaaaaaaaaaa1"], "pointer": "/name", "value": "X"}
          ]})");
        ComposedScene out = flatten("root.scene.json", r);
        CHECK(out.ok); // orphans are advisory
        CHECK(out.entities.size() == 1); // c1 composed; the inapplicable override left it as-is
        // Orphans: unknown instance id; unknown entity id; structurally inapplicable pointer
        // (array growth on a LIVE entity); a field override stopping at an instance id.
        CHECK(count_code(out, "compose.orphan_override") == 4);
    }

    // --- an outer field override on a removed entity is an orphan, never a resurrection -----------
    {
        MapResolver r;
        r.add("child.scene.json", R"({
          "$schema": "ctx:scene", "version": 1,
          "entities": [{"id": "ccccccccccccccc1", "name": "E", "components": {}}]})");
        r.add("root.scene.json", R"({
          "$schema": "ctx:scene", "version": 1, "entities": [],
          "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "child.scene.json"}],
          "overrides": [
            {"path": ["aaaaaaaaaaaaaaa1", "ccccccccccccccc1"], "remove": true},
            {"path": ["aaaaaaaaaaaaaaa1", "ccccccccccccccc1"], "pointer": "/name", "value": "X"}
          ]})");
        ComposedScene out = flatten("root.scene.json", r);
        CHECK(out.ok); // the remove is a normal structural op; the shadowed field is advisory
        CHECK(out.entities.empty());
        CHECK(count_code(out, "compose.orphan_override") == 1);
    }

    // --- the composed-entity budget (blocking, reported once) --------------------------------------
    {
        MapResolver r;
        r.add("wide.scene.json", R"({
          "$schema": "ctx:scene", "version": 1,
          "entities": [
            {"id": "ccccccccccccccc1", "name": "A", "components": {}},
            {"id": "ccccccccccccccc2", "name": "B", "components": {}},
            {"id": "ccccccccccccccc3", "name": "C", "components": {}}
          ]})");
        ComposeLimits tiny;
        tiny.max_entities = 2;
        ComposedScene out = flatten("wide.scene.json", r, tiny);
        CHECK(!out.ok);
        CHECK(out.entities.size() == 2);
        CHECK(count_code(out, "compose.too_many_entities") == 1);
    }

    COMPOSE_TEST_MAIN_END();
}
