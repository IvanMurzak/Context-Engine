// Scene-tree panel tests: selection events (the R-HUX-011 selection loop other panels consume),
// the R-BRIDGE-008 derivation.settled / stability handling, deterministic (stable) re-render, and
// selection preservation across a snapshot refresh.

#include "context/editor/gui/panels/scenetree/scene_tree_panel.h"

#include "context/editor/gui/panels/scenetree/scene_tree_model.h"
#include "context/editor/gui/uitree/panel.h"

#include "context/editor/bridge/event_stream.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/serializer/json_tree.h"

#include "scenetree_test.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace context::editor::gui::panels::scenetree;
namespace compose = context::editor::compose;
namespace bridge = context::editor::bridge;
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
                                             std::uint64_t hash)
{
    compose::ComposedEntity e;
    e.id_path = std::move(id_path);
    e.identity_hash = hash;
    e.value = named(name);
    return e;
}

// The standard fixture world: a root entity + an instance boundary with two children.
[[nodiscard]] SceneTreeModel standard_model()
{
    compose::ComposedScene scene;
    scene.root_path = "root.scene.json";
    scene.entities.push_back(entity({"e1"}, "RootEnt", 0x11));
    scene.entities.push_back(entity({"inst1", "c1"}, "Light", 0x22));
    scene.entities.push_back(entity({"inst1", "c2"}, "Prop", 0x33));
    return build_scene_tree(scene);
}

[[nodiscard]] std::size_t count(const std::string& haystack, const std::string& needle)
{
    std::size_t n = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
    {
        ++n;
    }
    return n;
}

} // namespace

int main()
{
    // --- the panel registers under its R-EDIT-001 contribution id -------------------------------
    CHECK(std::string(SceneTreePanel::kContributionId) == "builtin.scene-tree");

    // --- selection drives a selection event other panels consume --------------------------------
    {
        SceneTreePanel panel;
        panel.set_model(standard_model());

        SceneSelection last;
        int notifications = 0;
        panel.add_selection_listener(
            [&](const SceneSelection& s)
            {
                last = s;
                ++notifications;
            });

        CHECK(panel.select("inst1/c1"));
        CHECK(notifications == 1);
        CHECK(last.identity == "inst1/c1");
        CHECK(last.identity_hash == 0x22);
        CHECK(panel.selection().identity == "inst1/c1");

        // An unknown identity is ignored: no selection change, no notification.
        CHECK(!panel.select("ghost"));
        CHECK(notifications == 1);
        CHECK(panel.selection().identity == "inst1/c1");

        // Clearing notifies with an empty selection.
        panel.clear_selection();
        CHECK(notifications == 2);
        CHECK(last.identity.empty());
        CHECK(panel.selection().identity.empty());
    }

    // --- build_panel is a11y-conformant + keyboard-reachable (the primary CI assertion) ---------
    {
        SceneTreePanel panel;
        panel.set_model(standard_model());
        const uitree::Panel ui = panel.build_panel();

        CHECK(uitree::audit_a11y(ui).empty()); // no a11y violations by construction

        // Every tree row is focusable -> a complete keyboard path (R-A11Y-001 / R-CLI-001).
        const std::vector<std::string> order = uitree::focus_order(ui);
        CHECK(order.size() == 4);
        CHECK(order[0] == "scenetree.item.e1");
        CHECK(ui.has_command(kSelectCommand));
    }

    // --- selection is visible in the rendered tree ----------------------------------------------
    {
        SceneTreePanel panel;
        panel.set_model(standard_model());
        CHECK(panel.select("inst1/c1"));
        const std::string html = uitree::render_html(panel.build_panel());
        CHECK(count(html, "(selected)") == 1);        // exactly the selected row
        CHECK(html.find("(overridden)") == std::string::npos); // no override marker in this fixture
        CHECK(html.find("role=\"tree\"") != std::string::npos);
    }

    // --- deterministic (stable) re-render: identical state -> byte-identical HTML ----------------
    {
        SceneTreePanel panel;
        panel.set_model(standard_model());
        panel.select("e1");
        const std::string first = uitree::render_html(panel.build_panel());
        const std::string second = uitree::render_html(panel.build_panel());
        CHECK(first == second);
    }

    // --- R-BRIDGE-008: derivation.settled advances generation + records stability ---------------
    {
        SceneTreePanel panel;
        panel.set_model(standard_model());
        CHECK(panel.generation() == 0);
        CHECK(panel.stability() == bridge::Stability::stable);

        panel.on_derivation_settled(7, bridge::Stability::settling);
        CHECK(panel.generation() == 7);
        CHECK(panel.stability() == bridge::Stability::settling);
        const std::string settling = uitree::render_html(panel.build_panel());
        CHECK(settling.find("settling") != std::string::npos);
        CHECK(settling.find("generation 7") != std::string::npos);

        panel.on_derivation_settled(8, bridge::Stability::stable);
        const std::string stable = uitree::render_html(panel.build_panel());
        CHECK(stable.find("stable") != std::string::npos);
        CHECK(stable.find("generation 8") != std::string::npos);
        CHECK(stable != settling); // the re-render reflects the new generation/stability
    }

    // --- selection preserved across a snapshot refresh that still holds it -----------------------
    {
        SceneTreePanel panel;
        panel.set_model(standard_model());
        CHECK(panel.select("inst1/c1"));

        int notifications = 0;
        panel.add_selection_listener([&](const SceneSelection&) { ++notifications; });

        // The refreshed world still contains inst1/c1 -> selection preserved, no clear notification.
        panel.set_model(standard_model());
        CHECK(panel.selection().identity == "inst1/c1");
        CHECK(notifications == 0);
    }

    // --- selection cleared (and listeners notified) when it vanishes from the new world ---------
    {
        SceneTreePanel panel;
        panel.set_model(standard_model());
        CHECK(panel.select("inst1/c1"));

        int notifications = 0;
        SceneSelection last{"sentinel", 99};
        panel.add_selection_listener(
            [&](const SceneSelection& s)
            {
                last = s;
                ++notifications;
            });

        compose::ComposedScene other;
        other.root_path = "other.scene.json";
        other.entities.push_back(entity({"x1"}, "Other", 0x99));
        panel.set_model(build_scene_tree(other));

        CHECK(panel.selection().identity.empty());
        CHECK(notifications == 1);
        CHECK(last.identity.empty());
    }

    // --- an empty model renders an a11y-clean panel with no exposed command ----------------------
    {
        SceneTreePanel panel; // no model set
        const uitree::Panel ui = panel.build_panel();
        CHECK(uitree::audit_a11y(ui).empty()); // no orphan/unreachable command on an empty tree
        CHECK(!ui.has_command(kSelectCommand));
        CHECK(uitree::focus_order(ui).empty());
    }

    SCENETREE_TEST_MAIN_END();
}
