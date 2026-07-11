// Scene-tree view-model builder tests: flat composed entities woven into a hierarchy by L-35 id-path
// (instances synthesized as boundary nodes, overrides visible), deterministic order, and the same
// projection over a REAL flatten() of authored scenes.

#include "context/editor/gui/panels/scenetree/scene_tree_model.h"

#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_tree.h"

#include "scenetree_test.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace context::editor::gui::panels::scenetree;
namespace compose = context::editor::compose;
namespace serializer = context::editor::serializer;

namespace
{

// An object JsonValue carrying a single "name" string member (the entity's display name source).
[[nodiscard]] serializer::JsonValue named(const std::string& name)
{
    serializer::JsonValue value;
    value.type = serializer::JsonValue::Type::object;
    serializer::JsonMember member;
    member.key = "name";
    member.value.type = serializer::JsonValue::Type::string;
    member.value.string_value = name;
    value.members.push_back(std::move(member));
    return value;
}

// Attach an L-35 override contributor to an entity's provenance so build_scene_tree marks it
// overridden.
void mark_overridden(compose::ComposedEntity& entity)
{
    compose::FieldProvenance fp;
    fp.pointer = "/components/transform/position";
    compose::ProvenanceEntry pe;
    pe.source = compose::ProvenanceEntry::Source::override_value;
    pe.file = "root.scene.json";
    fp.chain.push_back(pe);
    entity.field_provenance.push_back(std::move(fp));
}

[[nodiscard]] compose::ComposedEntity make_entity(std::vector<std::string> id_path,
                                                  const std::string& name, std::uint64_t hash)
{
    compose::ComposedEntity entity;
    entity.id_path = std::move(id_path);
    entity.identity_hash = hash;
    entity.value = named(name);
    return entity;
}

// The MapResolver idiom from src/editor/compose/tests/test_flatten.cpp — a real flatten fixture.
[[nodiscard]] serializer::JsonValue parse(const char* json)
{
    serializer::CanonicalizeResult r = serializer::canonicalize(json);
    CHECK(r.is_json);
    return r.root;
}

class MapResolver final : public compose::SceneResolver
{
public:
    void add(const char* path, const char* json)
    {
        std::optional<compose::SceneDoc> doc = compose::build_scene_doc(path, parse(json));
        CHECK(doc.has_value());
        docs_[path] = std::move(*doc);
    }
    [[nodiscard]] const compose::SceneDoc* resolve(std::string_view path) const override
    {
        auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, compose::SceneDoc, std::less<>> docs_;
};

// Flatten the roots to their identities in tree (depth-first) order — a deterministic signature.
void collect_identities(const SceneTreeNode& node, std::vector<std::string>& out)
{
    out.push_back(node.identity);
    for (const SceneTreeNode& child : node.children)
    {
        collect_identities(child, out);
    }
}

[[nodiscard]] std::vector<std::string> tree_signature(const SceneTreeModel& model)
{
    std::vector<std::string> out;
    for (const SceneTreeNode& root : model.roots)
    {
        collect_identities(root, out);
    }
    return out;
}

} // namespace

int main()
{
    // --- empty composed scene -> empty tree -----------------------------------------------------
    {
        compose::ComposedScene scene;
        scene.root_path = "empty.scene.json";
        const SceneTreeModel model = build_scene_tree(scene);
        CHECK(model.root_scene == "empty.scene.json");
        CHECK(model.roots.empty());
        CHECK(model.entity_count == 0);
        CHECK(model.ok);
    }

    // --- hand-built flat scene: root entity + an instance boundary with two children ------------
    {
        compose::ComposedScene scene;
        scene.root_path = "root.scene.json";
        scene.entities.push_back(make_entity({"e1"}, "RootEnt", 0x11));
        compose::ComposedEntity light = make_entity({"inst1", "c1"}, "Light", 0x22);
        mark_overridden(light);
        scene.entities.push_back(std::move(light));
        scene.entities.push_back(make_entity({"inst1", "c2"}, "Prop", 0x33));

        const SceneTreeModel model = build_scene_tree(scene);
        CHECK(model.entity_count == 3);

        // Two top-level nodes, in expansion order: the root entity, then the instance boundary.
        CHECK(model.roots.size() == 2);
        CHECK(model.roots[0].identity == "e1");
        CHECK(model.roots[1].identity == "inst1");

        // The root entity: a leaf entity with its authored name, not overridden.
        const SceneTreeNode* root_ent = find_node(model, "e1");
        CHECK(root_ent != nullptr);
        CHECK(root_ent->kind == NodeKind::entity);
        CHECK(root_ent->display_name == "RootEnt");
        CHECK(root_ent->identity_hash == 0x11);
        CHECK(!root_ent->overridden);
        CHECK(root_ent->children.empty());

        // The instance boundary: synthetic (no composed entity of its own), named by its instance id.
        const SceneTreeNode* boundary = find_node(model, "inst1");
        CHECK(boundary != nullptr);
        CHECK(boundary->kind == NodeKind::instance);
        CHECK(boundary->display_name == "inst1");
        CHECK(boundary->identity_hash == 0);
        CHECK(boundary->children.size() == 2);

        // The overridden child is marked (L-35 override visible); its sibling is not.
        const SceneTreeNode* c1 = find_node(model, "inst1/c1");
        CHECK(c1 != nullptr);
        CHECK(c1->kind == NodeKind::entity);
        CHECK(c1->display_name == "Light");
        CHECK(c1->overridden);
        CHECK(c1->identity_hash == 0x22);

        const SceneTreeNode* c2 = find_node(model, "inst1/c2");
        CHECK(c2 != nullptr);
        CHECK(c2->display_name == "Prop");
        CHECK(!c2->overridden);

        // Deterministic: rebuilding the same scene yields an identical tree signature.
        CHECK(tree_signature(model) == tree_signature(build_scene_tree(scene)));

        // A missing identity resolves to nothing.
        CHECK(find_node(model, "nope") == nullptr);
    }

    // --- entity with no "name" member falls back to the last id-path segment ---------------------
    {
        compose::ComposedScene scene;
        scene.root_path = "s.json";
        compose::ComposedEntity e;
        e.id_path = {"abc123"};
        e.value.type = serializer::JsonValue::Type::object; // object, but no "name" member
        scene.entities.push_back(std::move(e));
        const SceneTreeModel model = build_scene_tree(scene);
        CHECK(model.roots.size() == 1);
        CHECK(model.roots[0].display_name == "abc123");
    }

    // --- the SAME projection over a REAL flatten() of authored scenes ---------------------------
    {
        MapResolver r;
        r.add("child.scene.json", R"({
          "$schema": "ctx:scene", "version": 1,
          "entities": [
            {"id": "ccccccccccccccc1", "name": "Light",
             "components": {"transform": {"position": [0, 0, 0]}}},
            {"id": "ccccccccccccccc2", "name": "Prop", "components": {}}
          ]})");
        r.add("root.scene.json", R"({
          "$schema": "ctx:scene", "version": 1,
          "entities": [{"id": "eeeeeeeeeeeeeee1", "name": "RootEnt", "components": {}}],
          "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "child.scene.json"}],
          "overrides": [
            {"path": ["aaaaaaaaaaaaaaa1", "ccccccccccccccc1"],
             "pointer": "/components/transform/position", "value": [1, 1, 1]}
          ]})");

        const compose::ComposedScene scene = compose::flatten("root.scene.json", r);
        CHECK(scene.ok);
        const SceneTreeModel model = build_scene_tree(scene);
        CHECK(model.ok);
        CHECK(model.entity_count == scene.entities.size());

        // The instance boundary is synthesized from the id-path prefix.
        const SceneTreeNode* boundary = find_node(model, "aaaaaaaaaaaaaaa1");
        CHECK(boundary != nullptr);
        CHECK(boundary->kind == NodeKind::instance);
        CHECK(!boundary->children.empty());

        // The overridden child of the real flatten is marked overridden; the untouched one is not.
        const SceneTreeNode* light = find_node(model, "aaaaaaaaaaaaaaa1/ccccccccccccccc1");
        CHECK(light != nullptr);
        CHECK(light->kind == NodeKind::entity);
        CHECK(light->display_name == "Light");
        CHECK(light->overridden);

        const SceneTreeNode* prop = find_node(model, "aaaaaaaaaaaaaaa1/ccccccccccccccc2");
        CHECK(prop != nullptr);
        CHECK(prop->display_name == "Prop");
        CHECK(!prop->overridden);

        // The root scene's own authored entity is a top-level node.
        const SceneTreeNode* root_ent = find_node(model, "eeeeeeeeeeeeeee1");
        CHECK(root_ent != nullptr);
        CHECK(root_ent->kind == NodeKind::entity);
        CHECK(root_ent->display_name == "RootEnt");
    }

    SCENETREE_TEST_MAIN_END();
}
