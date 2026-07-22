// T1 for the panel composition root (M9 e05d1/e05d3): what binds, what deliberately stays unhosted,
// and the end-to-end path from the real roster through a real provider to a rendered panel.
//
// e05d1 pinned `builtin.scene-tree` / `builtin.inspector` as LISTED-BUT-UNHOSTED (their libraries
// linked `context_compose`, which the D10 shell-boundary gate forbids) — and pinned it HERE so that
// e05d3, whose whole job was to make them hostable, saw this test go red in exactly the place that
// means "you succeeded". e05d3 landed: the kernel-typed builders moved daemon-side, the panel
// libraries are boundary-clean, and this file now asserts the HOSTED state — all four hostable
// panels bound, and the Scene tree's selection wired to the Inspector's fetch (R-HUX-011).

#include "context/editor/shell/panels/builtin_panels.h"

#include "context/editor/gui/contract/builtin_roster.h"
#include "context/editor/gui/panels/problems/problems_panel.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/inspector_feed.h" // complete feed types: builtin_panels.h only
#include "context/editor/shell/panels/problems_feed.h"  // forward-declares them, and this file calls
#include "context/editor/shell/panels/scenetree_feed.h" // methods on the bag's members.

#include "panels_test.h"

#include <optional>
#include <string>
#include <utility>

namespace shell = context::editor::shell;
namespace panels = context::editor::shell::panels;
namespace gc = context::editor::gui::contract;
namespace scenetree = context::editor::gui::panels::scenetree;
using Json = context::editor::contract::Json;

namespace
{

const Json* find_panel(const Json& listing, const std::string& id)
{
    const Json& list = listing.at("panels");
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        if (list.at(i).at("id").as_string() == id)
        {
            return &list.at(i);
        }
    }
    return nullptr;
}

void binds_every_hostable_panel_and_nothing_else()
{
    shell::PanelHost host;
    const panels::BuiltinPanels bound = panels::install_builtin_panels(host);

    // Every id the module CLAIMS is hostable actually bound — the list and the bindings cannot drift.
    CHECK(bound.bound == panels::hostable_panel_ids().size());
    CHECK(host.hosted_count() == panels::hostable_panel_ids().size());
    for (const std::string& id : panels::hostable_panel_ids())
    {
        CHECK(host.hosts(id));
    }

    // FIVE panels, from four different libraries (uitree / problems / the e05d3 pair / the e08b
    // playbar) — the panel-agnosticism claim exercised across every hosted shape.
    CHECK(panels::hostable_panel_ids().size() == 5);
    CHECK(host.hosts("placeholder"));
    CHECK(host.hosts(context::editor::gui::panels::problems::ProblemsPanel::kContributionId));
    CHECK(host.hosts("builtin.scene-tree"));
    CHECK(host.hosts("builtin.inspector"));
    CHECK(host.hosts("builtin.playbar"));

    // The feed owners came back, so every provider's captures stay alive.
    CHECK(bound.problems != nullptr);
    CHECK(bound.scenetree != nullptr);
    CHECK(bound.inspector != nullptr);
    CHECK(bound.session != nullptr);

    // The whole roster is still LISTED — an unhostable panel is visible and honestly flagged, never
    // hidden.
    const Json listing = host.list();
    CHECK(listing.at("panels").size() == gc::builtin_contributions().size());
    const Json* scenetree_entry = find_panel(listing, "builtin.scene-tree");
    CHECK(scenetree_entry != nullptr && scenetree_entry->at("hosted").as_bool());
    const Json* inspector_entry = find_panel(listing, "builtin.inspector");
    CHECK(inspector_entry != nullptr && inspector_entry->at("hosted").as_bool());
    const Json* problems_entry =
        find_panel(listing, context::editor::gui::panels::problems::ProblemsPanel::kContributionId);
    CHECK(problems_entry != nullptr && problems_entry->at("hosted").as_bool());
    // The observers expose no gestures and persist nothing. REPORTED, not stubbed.
    CHECK(problems_entry != nullptr && !problems_entry->at("gestures").as_bool());
    CHECK(problems_entry != nullptr && !problems_entry->at("persists").as_bool());
    CHECK(scenetree_entry != nullptr && !scenetree_entry->at("gestures").as_bool());
    CHECK(inspector_entry != nullptr && !inspector_entry->at("persists").as_bool());
}

void renders_the_hosted_panels_through_the_bridge()
{
    shell::PanelHost host;
    const panels::BuiltinPanels bound = panels::install_builtin_panels(host);
    CHECK(bound.bound == panels::hostable_panel_ids().size());

    shell::BridgeRouter router;
    CHECK(host.install(router));

    // The placeholder: a panel whose provider supplies `build` ALONE. If the host or the wire
    // envelope assumed any optional capability, this is where it would show.
    std::string placeholder_code;
    const std::optional<shell::PanelRender> placeholder = host.render("placeholder", placeholder_code);
    CHECK(placeholder.has_value());
    if (placeholder.has_value())
    {
        CHECK(!placeholder->html.empty());
        CHECK(!placeholder->focus_order.empty());
        CHECK(!placeholder->commands.empty());
    }

    // Problems: a panel with a live model behind it. Empty right now (no diagnostics have arrived),
    // which must still render a real tree rather than nothing — an empty Problems panel is a
    // legitimate, common state.
    std::string problems_code;
    const std::optional<shell::PanelRender> problems = host.render(
        context::editor::gui::panels::problems::ProblemsPanel::kContributionId, problems_code);
    CHECK(problems.has_value());
    if (problems.has_value())
    {
        CHECK(panelstest::mentions(problems->html, "problems.panel"));
        CHECK(panelstest::mentions(problems->html, "problems.status"));
    }

    // The e05d3 pair render their honest EMPTY states before any daemon read arrives: a scene tree
    // with no scene, an inspector with no selection.
    std::string scenetree_code;
    const std::optional<shell::PanelRender> tree = host.render("builtin.scene-tree", scenetree_code);
    CHECK(tree.has_value());
    if (tree.has_value())
    {
        CHECK(panelstest::mentions(tree->html, "scenetree.panel"));
        CHECK(panelstest::mentions(tree->html, "scenetree.status"));
    }
    std::string inspector_code;
    const std::optional<shell::PanelRender> inspector_render =
        host.render("builtin.inspector", inspector_code);
    CHECK(inspector_render.has_value());
    if (inspector_render.has_value())
    {
        CHECK(panelstest::mentions(inspector_render->html, "inspector.panel"));
        CHECK(panelstest::mentions(inspector_render->html, "No entity selected"));
    }
}

// The live path, end to end through the SAME objects `context_editor` wires: a daemon event reaches
// the feed, the feed touches the host, and the next render carries it.
void a_daemon_event_reaches_the_rendered_panel()
{
    shell::PanelHost host;
    const panels::BuiltinPanels bound = panels::install_builtin_panels(host);
    CHECK(bound.problems != nullptr);
    if (bound.problems == nullptr)
    {
        return;
    }
    const std::string panel_id = context::editor::gui::panels::problems::ProblemsPanel::kContributionId;

    Json payload = Json::object();
    payload.set("code", Json("file.malformed"));
    payload.set("message", Json("unterminated object"));
    payload.set("severity", Json("error"));
    payload.set("file", Json("scenes/level.json"));
    payload.set("line", Json(7));

    const std::uint64_t before = host.revision(panel_id);
    CHECK(bound.problems->apply_event("diagnostics", payload, 3));
    CHECK(host.revision(panel_id) > before);

    std::string error_code;
    const std::optional<shell::PanelRender> rendered = host.render(panel_id, error_code);
    CHECK(rendered.has_value());
    if (rendered.has_value())
    {
        CHECK(panelstest::mentions(rendered->html, "unterminated object"));
        CHECK(panelstest::mentions(rendered->html, "scenes/level.json"));
        CHECK(panelstest::mentions(rendered->html, "data-command=\"problems.navigate\""));
    }
}

// The e05d3 selection loop, through the SAME wiring `context_editor` gets: adopting a scene tree,
// the DAEMON's selection arriving, and the Inspector's fetch turning up PENDING for exactly that
// identity (R-HUX-011).
//
// M9 e08b re-rooted this loop on daemon truth: the tree no longer decides a selection, so the loop
// is driven the way the live editor drives it — a `selection-changed` fact through the session feed.
// That is the whole point: the Inspector cannot tell (and must not care) whether the human clicked
// this panel or a CLI on another terminal.
void a_daemon_selection_schedules_the_inspector_fetch()
{
    shell::PanelHost host;
    const panels::BuiltinPanels bound = panels::install_builtin_panels(host);
    CHECK(bound.scenetree != nullptr);
    CHECK(bound.inspector != nullptr);
    CHECK(bound.session != nullptr);
    if (bound.scenetree == nullptr || bound.inspector == nullptr || bound.session == nullptr)
    {
        return;
    }

    scenetree::SceneTreeModel model;
    scenetree::SceneTreeNode node;
    node.identity = "inst1/ent1";
    node.display_name = "Player";
    model.roots.push_back(std::move(node));
    bound.scenetree->panel().set_model(std::move(model));

    CHECK(!bound.inspector->pending().has_value());

    Json fact = Json::object();
    fact.set("event", Json(std::string("selection-changed")));
    fact.set("origin", Json(std::uint64_t{7})); // another client — not this Shell
    Json ids = Json::array();
    ids.push_back(Json(std::string("inst1/ent1")));
    fact.set("ids", std::move(ids));
    CHECK(panels::apply_session_event(*bound.session, panels::kSessionTopic, fact));
    CHECK(bound.inspector->pending() == std::optional<std::string>("inst1/ent1"));

    // A cleared selection (an empty id list) clears the panel and drops the pending fetch.
    Json cleared = Json::object();
    cleared.set("event", Json(std::string("selection-changed")));
    cleared.set("origin", Json(std::uint64_t{7}));
    cleared.set("ids", Json::array());
    CHECK(panels::apply_session_event(*bound.session, panels::kSessionTopic, cleared));
    CHECK(!bound.inspector->pending().has_value());
    CHECK(!bound.inspector->panel().has_selection());

    // With no daemon bound, a row activation writes NOTHING and moves NOTHING — the panel cannot
    // change a selection it does not own (and the composition root leaves it honestly read-only).
    CHECK(!bound.scenetree->panel().select("inst1/ent1"));
    CHECK(!bound.inspector->pending().has_value());
}

} // namespace

int main()
{
    binds_every_hostable_panel_and_nothing_else();
    renders_the_hosted_panels_through_the_bridge();
    a_daemon_event_reaches_the_rendered_panel();
    a_daemon_selection_schedules_the_inspector_fetch();
    PANELS_TEST_MAIN_END();
}
