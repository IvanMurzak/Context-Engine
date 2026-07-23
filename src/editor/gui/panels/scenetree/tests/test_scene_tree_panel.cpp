// Scene-tree panel tests: the M9 e08b DAEMON-BACKED selection (write requests out through the
// SelectionGateway, rendered selection in through `apply_selection`), the R-HUX-011 selection loop
// other panels consume, the R-BRIDGE-008 derivation.settled / stability handling, deterministic
// (stable) re-render, and selection behaviour across a snapshot refresh.
//
// ⚠ THE GATEWAY DOUBLE IS DELIBERATELY NO MORE CAPABLE THAN THE DAEMON. It answers with the
// selection the daemon would then hold — exactly what `editor.select`'s reply carries — and `nullopt`
// when it refuses. It never reaches into the panel: the panel applies the ANSWER, which is what makes
// "a selection the daemon never accepted is never rendered" testable rather than asserted.

#include "context/editor/gui/panels/scenetree/scene_tree_panel.h"

#include "context/editor/gui/panels/scenetree/scene_tree_model.h"
#include "context/editor/gui/uitree/panel.h"

#include "context/editor/bridge/event_stream.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/gui/panels/builders/scene_tree_builder.h"
#include "context/editor/serializer/json_tree.h"

#include "scenetree_test.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace context::editor::gui::panels::scenetree;
// The kernel-typed builder moved to context_gui_panel_builders (M9 e05d3, D10); the tests reach it
// through its new home while the panel library under test stays boundary-clean.
using context::editor::gui::panels::builders::build_scene_tree;
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

// Records every write request and answers as the daemon does: the selection it now holds, or nullopt
// on a refusal (see the header note).
class RecordingGateway final : public SelectionGateway
{
public:
    std::vector<std::vector<std::string>> requests;
    std::vector<std::string> selection; // the daemon's state, after every accepted write
    bool refuse = false;                // scope denied / no daemon / transport fault

    std::optional<std::vector<std::string>>
    request_selection(const std::vector<std::string>& ids) override
    {
        requests.push_back(ids);
        if (refuse)
        {
            return std::nullopt;
        }
        selection = ids;
        return selection;
    }
};

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

    // --- e08b: `select` WRITES, then renders the DAEMON'S ANSWER ---------------------------------
    {
        RecordingGateway gateway;
        SceneTreePanel panel(&gateway);
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
        CHECK(gateway.requests.size() == 1);
        CHECK(gateway.requests[0] == std::vector<std::string>{"inst1/c1"});
        // The panel renders the daemon's reported selection — reached through the gateway's ANSWER,
        // never through a local decision (the double never touches the panel).
        CHECK(notifications == 1);
        CHECK(last.identity == "inst1/c1");
        CHECK(last.identity_hash == 0x22);
        CHECK(panel.selection().identity == "inst1/c1");

        // Re-selecting the same row: the write lands, the daemon reports the same selection, and
        // nothing churns — the idempotence that keeps a repeated click from re-notifying every panel.
        CHECK(panel.select("inst1/c1"));
        CHECK(gateway.requests.size() == 2);
        CHECK(notifications == 1);

        // An unknown identity is a dead click: refused locally, never sent to the daemon.
        CHECK(!panel.select("ghost"));
        CHECK(gateway.requests.size() == 2);
        CHECK(notifications == 1);
        CHECK(panel.selection().identity == "inst1/c1");

        // A daemon REFUSAL leaves the rendered selection exactly where it was — an unapplied request
        // is never visible, which is the whole reason the panel does not move optimistically.
        gateway.refuse = true;
        CHECK(!panel.select("e1"));
        CHECK(gateway.requests.size() == 3);
        CHECK(panel.selection().identity == "inst1/c1");
        CHECK(notifications == 1);

        // Clearing is a write too: an EMPTY id list.
        gateway.refuse = false;
        CHECK(panel.clear_selection());
        CHECK(gateway.requests.size() == 4);
        CHECK(gateway.requests[3].empty());
        CHECK(notifications == 2);
        CHECK(last.identity.empty());
        CHECK(panel.selection().identity.empty());
    }

    // --- e08b: with NO gateway the panel cannot change a selection it does not own ----------------
    {
        SceneTreePanel panel; // the a11y harness's default-constructed shape
        panel.set_model(standard_model());
        int notifications = 0;
        panel.add_selection_listener([&](const SceneSelection&) { ++notifications; });

        CHECK(!panel.select("e1"));
        CHECK(!panel.clear_selection());
        CHECK(panel.selection().identity.empty());
        CHECK(notifications == 0);

        // ...but it still RENDERS whatever the daemon says, which is the whole subscriber half.
        CHECK(panel.apply_selection({"e1"}));
        CHECK(panel.selection().identity == "e1");
        CHECK(notifications == 1);
    }

    // --- e08b: the daemon's multi-id selection renders its FIRST id (single-select panel) ---------
    {
        SceneTreePanel panel;
        panel.set_model(standard_model());
        CHECK(panel.apply_selection({"inst1/c1", "inst1/c2"}));
        CHECK(panel.selection().identity == "inst1/c1");

        // An id with no row here is still adopted: the daemon's truth does not depend on what this
        // panel has loaded. It simply renders unmarked (no hash) until a model containing it arrives.
        CHECK(panel.apply_selection({"not-in-this-view"}));
        CHECK(panel.selection().identity == "not-in-this-view");
        CHECK(panel.selection().identity_hash == 0);
        CHECK(uitree::render_html(panel.build_panel()).find("(selected)") == std::string::npos);

        // A restatement of what is already rendered notifies nobody (a ring replay can repeat one).
        CHECK(!panel.apply_selection({"not-in-this-view"}));
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
        CHECK(panel.apply_selection({"inst1/c1"}));
        const std::string html = uitree::render_html(panel.build_panel());
        CHECK(count(html, "(selected)") == 1);        // exactly the selected row
        CHECK(html.find("(overridden)") == std::string::npos); // no override marker in this fixture
        CHECK(html.find("role=\"tree\"") != std::string::npos);
    }

    // --- deterministic (stable) re-render: identical state -> byte-identical HTML ----------------
    {
        SceneTreePanel panel;
        panel.set_model(standard_model());
        CHECK(panel.apply_selection({"e1"}));
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
        CHECK(panel.apply_selection({"inst1/c1"}));

        int notifications = 0;
        panel.add_selection_listener([&](const SceneSelection&) { ++notifications; });

        // The refreshed world still contains inst1/c1 -> nothing re-resolved, no notification.
        panel.set_model(standard_model());
        CHECK(panel.selection().identity == "inst1/c1");
        CHECK(panel.selection().identity_hash == 0x22);
        CHECK(notifications == 0);
    }

    // --- e08b: a refresh that DROPS the selected node does NOT deselect --------------------------
    // Selection belongs to the daemon: a node missing from this panel's view of the world is not the
    // daemon deselecting it. Only the L-37 identity hash is re-resolved (to 0 — "no hash"), and the
    // row simply renders unmarked.
    {
        SceneTreePanel panel;
        panel.set_model(standard_model());
        CHECK(panel.apply_selection({"inst1/c1"}));

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

        CHECK(panel.selection().identity == "inst1/c1"); // still the daemon's selection
        CHECK(panel.selection().identity_hash == 0);     // but no row here to hash
        CHECK(notifications == 1);                       // the re-resolution IS a rendered change
        CHECK(last.identity == "inst1/c1");
        CHECK(uitree::render_html(panel.build_panel()).find("(selected)") == std::string::npos);

        // Bring the node back: the hash re-resolves and listeners see it again.
        panel.set_model(standard_model());
        CHECK(panel.selection().identity_hash == 0x22);
        CHECK(notifications == 2);
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
