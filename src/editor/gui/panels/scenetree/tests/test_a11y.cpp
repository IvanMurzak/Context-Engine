// The scene-tree panel's per-panel a11y scan + keyboard-only navigation assertion (R-A11Y-001 /
// R-EDIT-001), headless on the default matrix (no CEF). This is the M5-F2 half of the a11y-harness
// coverage the M5-F6 harness reconciles; its ctest name is registered in the defensive coverage
// fragment src/editor/gui/a11y/coverage/scenetree.json. Asserts EVERY node the panel renders has a
// keyboard path and no accessibility violation, across empty / populated / deep / overridden worlds.

#include "context/editor/gui/panels/scenetree/scene_tree_model.h"
#include "context/editor/gui/panels/scenetree/scene_tree_panel.h"
#include "context/editor/gui/uitree/panel.h"

#include "context/editor/compose/flatten.h"
#include "context/editor/serializer/json_tree.h"

#include "scenetree_test.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace context::editor::gui::panels::scenetree;
namespace compose = context::editor::compose;
namespace serializer = context::editor::serializer;
namespace uitree = context::editor::gui::uitree;

namespace
{

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

[[nodiscard]] compose::ComposedEntity entity(std::vector<std::string> id_path, const std::string& name,
                                             bool overridden)
{
    compose::ComposedEntity e;
    e.id_path = std::move(id_path);
    e.value = named(name);
    if (overridden)
    {
        compose::FieldProvenance fp;
        compose::ProvenanceEntry pe;
        pe.source = compose::ProvenanceEntry::Source::override_value;
        fp.chain.push_back(pe);
        e.field_provenance.push_back(std::move(fp));
    }
    return e;
}

// Total node count in the view model (each node becomes exactly one focusable treeitem).
[[nodiscard]] std::size_t node_count(const SceneTreeNode& node)
{
    std::size_t n = 1;
    for (const SceneTreeNode& child : node.children)
    {
        n += node_count(child);
    }
    return n;
}

[[nodiscard]] std::size_t node_count(const SceneTreeModel& model)
{
    std::size_t n = 0;
    for (const SceneTreeNode& root : model.roots)
    {
        n += node_count(root);
    }
    return n;
}

// The a11y + keyboard-nav gate for one model: zero violations AND every node reachable by keyboard.
void assert_a11y_clean(const SceneTreeModel& model)
{
    SceneTreePanel panel;
    panel.set_model(model);
    const uitree::Panel ui = panel.build_panel();

    CHECK(uitree::audit_a11y(ui).empty());
    // Keyboard-only navigation reaches every rendered node (R-A11Y-001 complete keyboard nav).
    CHECK(uitree::focus_order(ui).size() == node_count(model));
}

} // namespace

int main()
{
    // Empty world: an a11y-clean panel with no dangling command and no focusable rows.
    {
        SceneTreeModel model;
        assert_a11y_clean(model);
    }

    // Populated world with a nested instance + a deep, overridden leaf.
    {
        compose::ComposedScene scene;
        scene.root_path = "root.scene.json";
        scene.entities.push_back(entity({"e1"}, "RootEnt", false));
        scene.entities.push_back(entity({"inst1", "c1"}, "Light", true));
        scene.entities.push_back(entity({"inst1", "inst2", "deep"}, "DeepEnt", true));
        const SceneTreeModel model = build_scene_tree(scene);

        // Sanity: the deep synthetic boundary + the deep entity exist and the leaf is overridden.
        CHECK(find_node(model, "inst1/inst2") != nullptr);
        const SceneTreeNode* deep = find_node(model, "inst1/inst2/deep");
        CHECK(deep != nullptr);
        CHECK(deep->overridden);

        assert_a11y_clean(model);

        // The overridden markers are visible in the rendered surface.
        SceneTreePanel panel;
        panel.set_model(model);
        const std::string html = uitree::render_html(panel.build_panel());
        CHECK(html.find("(overridden)") != std::string::npos);
        CHECK(html.find("(instance)") != std::string::npos);
    }

    SCENETREE_TEST_MAIN_END();
}
