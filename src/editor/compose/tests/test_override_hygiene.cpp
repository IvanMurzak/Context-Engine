// Advisory override hygiene (R-CLI-006, issue #58): `context query --overrides diverged | redundant`.
// A redundant override restates the current template value (changes nothing); a diverged override
// carries a recorded `base` that no longer matches the current template value (the template moved
// underneath it). Both are advisory — never auto-pruned. Happy + edge coverage (R-QA-013): flagged,
// not-flagged, no-recorded-base, and a new-field override (neither).

#include "context/editor/compose/compose_write.h"

#include "context/editor/compose/scene_model.h"
#include "context/editor/serializer/canonical.h"

#include "compose_test.h"

#include <map>
#include <optional>
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

class MapWriteResolver final : public WriteResolver
{
public:
    void add(const std::string& path, const char* json)
    {
        JsonValue tree = parse(json);
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

// child{C1 position [0,0,0]} instanced under the root as A; the root's single override (given as
// `override_json`) addresses [A, C1]'s position. Returns the findings for `kind`.
[[nodiscard]] std::vector<OverrideFinding> hygiene_with(const char* override_json, HygieneKind kind)
{
    MapWriteResolver r;
    r.add("child.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [
        {"id": "ccccccccccccccc1", "name": "Light",
         "components": {"transform": {"position": [0, 0, 0]}}}
      ]})");
    std::string root = std::string(R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [],
      "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "child.scene.json"}],
      "overrides": [)") + override_json + "]}";
    r.add("root.scene.json", root.c_str());
    return override_hygiene("root.scene.json", r, kind);
}

} // namespace

int main()
{
    const char* kPtr = R"("pointer": "/components/transform/position")";
    const std::string prefix = std::string(R"({"path": ["aaaaaaaaaaaaaaa1", "ccccccccccccccc1"], )") +
                               kPtr + ", ";

    // --- redundant: the override value equals the current template value -------------------------
    {
        const std::string entry = prefix + R"("value": [0, 0, 0]})";
        std::vector<OverrideFinding> found = hygiene_with(entry.c_str(), HygieneKind::redundant);
        CHECK(found.size() == 1);
        CHECK(found[0].file == "root.scene.json");
        CHECK(found[0].entry_pointer == "/overrides/0");
        CHECK(found[0].field_pointer == "/components/transform/position");
        // ... and it is NOT diverged (no recorded base to compare).
        CHECK(hygiene_with(entry.c_str(), HygieneKind::diverged).empty());
    }

    // --- NOT redundant: the override value differs from the template ------------------------------
    {
        const std::string entry = prefix + R"("value": [1, 1, 1]})";
        CHECK(hygiene_with(entry.c_str(), HygieneKind::redundant).empty());
    }

    // --- diverged: a recorded base that no longer matches the current template value -------------
    {
        // base [7,7,7] != current template [0,0,0] -> the template moved under this override.
        const std::string entry = prefix + R"("value": [1, 1, 1], "base": [7, 7, 7]})";
        std::vector<OverrideFinding> found = hygiene_with(entry.c_str(), HygieneKind::diverged);
        CHECK(found.size() == 1);
        CHECK(found[0].file == "root.scene.json");
        CHECK(found[0].field_pointer == "/components/transform/position");
        // ... and NOT redundant (value [1,1,1] != template [0,0,0]).
        CHECK(hygiene_with(entry.c_str(), HygieneKind::redundant).empty());
    }

    // --- NOT diverged: the recorded base still matches the current template value -----------------
    {
        const std::string entry = prefix + R"("value": [1, 1, 1], "base": [0, 0, 0]})";
        CHECK(hygiene_with(entry.c_str(), HygieneKind::diverged).empty());
    }

    // --- a hand-authored override with NO recorded base is not divergence-checkable --------------
    {
        const std::string entry = prefix + R"("value": [1, 1, 1]})";
        CHECK(hygiene_with(entry.c_str(), HygieneKind::diverged).empty());
    }

    // --- a NEW-field override (no template value at the pointer) is neither diverged nor redundant
    {
        const std::string entry =
            R"({"path": ["aaaaaaaaaaaaaaa1", "ccccccccccccccc1"], "pointer": "/components/tag", "value": "hero", "base": "old"})";
        CHECK(hygiene_with(entry.c_str(), HygieneKind::redundant).empty());
        CHECK(hygiene_with(entry.c_str(), HygieneKind::diverged).empty());
    }

    // --- an orphan override (path resolves to nothing) is the flatten's finding, not hygiene's ----
    {
        const std::string entry =
            R"({"path": ["ffffffffffffffff", "ccccccccccccccc1"], "pointer": "/components/transform/position", "value": [0, 0, 0]})";
        CHECK(hygiene_with(entry.c_str(), HygieneKind::redundant).empty());
        CHECK(hygiene_with(entry.c_str(), HygieneKind::diverged).empty());
    }

    // --- an override onto a NESTED scene-root that is INERT (not `composable`) is a de-facto orphan:
    // --- flatten leaves such a root out of the composition, so hygiene must not evaluate it as a
    // --- live target (same treatment as the !found orphan above). A `composable:true` root IS live.
    {
        const auto hygiene_root = [](bool composable, HygieneKind kind) {
            MapWriteResolver r;
            const std::string gadget =
                std::string(R"({"$schema": "ctx:scene", "version": 1, "entities": [], "root": {)") +
                (composable ? R"("composable": true, )" : "") +
                R"("components": {"physics": {"gravity": [0, -9, 0]}}}})";
            r.add("gadget.scene.json", gadget.c_str());
            r.add("host.scene.json", R"({
              "$schema": "ctx:scene", "version": 1, "entities": [],
              "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "gadget.scene.json"}],
              "overrides": [
                {"path": ["aaaaaaaaaaaaaaa1", "$root"], "pointer": "/components/physics/gravity",
                 "value": [1, 1, 1], "base": [7, 7, 7]}
              ]})");
            return override_hygiene("host.scene.json", r, kind);
        };
        // inert nested root -> de-facto orphan -> reported by NEITHER kind (like the !found orphan).
        CHECK(hygiene_root(false, HygieneKind::redundant).empty());
        CHECK(hygiene_root(false, HygieneKind::diverged).empty());
        // composable nested root -> a live target: base [7,7,7] != template [0,-9,0] -> diverged.
        CHECK(hygiene_root(true, HygieneKind::diverged).size() == 1);
    }

    COMPOSE_TEST_MAIN_END();
}
