// T1 for the panel composition root (M9 e05d1): what binds, what deliberately stays unhosted, and
// the end-to-end path from the real roster through a real provider to a rendered panel.
//
// THE UNHOSTED ASSERTIONS ARE THE POINT, not an omission. `builtin.scene-tree` and
// `builtin.inspector` are LISTED and NOT hosted in this build, because their libraries link
// `context_compose`, which the D10 shell-boundary gate forbids. Asserting that here pins the current
// state so e05d3 — whose whole job is to make them hostable — sees this test go red in exactly the
// place that means "you succeeded", rather than discovering the boundary by configure failure.

#include "context/editor/shell/panels/builtin_panels.h"

#include "context/editor/gui/contract/builtin_roster.h"
#include "context/editor/gui/panels/problems/problems_panel.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/problems_feed.h" // ProblemsFeed complete type: builtin_panels.h
                                                        // only forward-declares it now, and this file
                                                        // calls ProblemsFeed methods on `bound.problems`.

#include "panels_test.h"

#include <optional>
#include <string>

namespace shell = context::editor::shell;
namespace panels = context::editor::shell::panels;
namespace gc = context::editor::gui::contract;
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

    // TWO panels, from two different libraries — see builtin_panels.h on why one would not do.
    CHECK(panels::hostable_panel_ids().size() == 2);
    CHECK(host.hosts("placeholder"));
    CHECK(host.hosts(context::editor::gui::panels::problems::ProblemsPanel::kContributionId));

    // The feed owner came back, so the Problems provider's captures stay alive.
    CHECK(bound.problems != nullptr);

    // --- the deliberate NOT-hosted set (see the file header).
    CHECK(host.knows("builtin.scene-tree"));
    CHECK(!host.hosts("builtin.scene-tree"));
    CHECK(host.knows("builtin.inspector"));
    CHECK(!host.hosts("builtin.inspector"));

    // The whole roster is still LISTED — an unhostable panel is visible and honestly flagged, never
    // hidden.
    const Json listing = host.list();
    CHECK(listing.at("panels").size() == gc::builtin_contributions().size());
    const Json* scenetree = find_panel(listing, "builtin.scene-tree");
    CHECK(scenetree != nullptr && !scenetree->at("hosted").as_bool());
    const Json* problems_entry =
        find_panel(listing, context::editor::gui::panels::problems::ProblemsPanel::kContributionId);
    CHECK(problems_entry != nullptr && problems_entry->at("hosted").as_bool());
    // Problems is a read-only observer: no gestures, nothing persisted. REPORTED, not stubbed.
    CHECK(problems_entry != nullptr && !problems_entry->at("gestures").as_bool());
    CHECK(problems_entry != nullptr && !problems_entry->at("persists").as_bool());
}

void renders_both_hosted_panels_through_the_bridge()
{
    shell::PanelHost host;
    const panels::BuiltinPanels bound = panels::install_builtin_panels(host);
    CHECK(bound.bound == 2);

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

    // A rostered-but-unhosted panel refuses with the honest code.
    std::string unhosted_code;
    CHECK(!host.render("builtin.inspector", unhosted_code).has_value());
    CHECK(unhosted_code == shell::kErrPanelNotHosted);
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

} // namespace

int main()
{
    binds_every_hostable_panel_and_nothing_else();
    renders_both_hosted_panels_through_the_bridge();
    a_daemon_event_reaches_the_rendered_panel();
    PANELS_TEST_MAIN_END();
}
