// The built-in editor-panel registry the a11y harness scans (see registry.h).
//
// M9 e05b: registered_panels() is REGENERATED from the single built-in roster
// (gui/contract/builtin_roster.h) rather than hand-maintained. This file now owns exactly ONE thing —
// the binding from a roster id to the headless FACTORY that instantiates that panel — because a
// factory needs the panel's C++ type, which the data-only roster deliberately does not link. Roster
// order is the scan order.

#include "context/editor/gui/a11y/registry.h"

#include "context/editor/gui/contract/builtin_roster.h"
#include "context/editor/gui/contract/extension.h"

#include "context/editor/gui/help/help_panel.h"
#include "context/editor/gui/panels/inspector/inspector_panel.h"
#include "context/editor/gui/panels/problems/problems_panel.h"
#include "context/editor/gui/panels/scenetree/scene_tree_panel.h"
#include "context/editor/gui/panels/tilemap/tilemap_paint_panel.h"
#include "context/editor/gui/playbar/playbar_model.h"
#include "context/editor/gui/playbar/playbar_panel.h"
#include "context/editor/gui/session/undo/undo_journal.h"
#include "context/editor/gui/uitree/builtin.h"
#include "context/editor/gui/uitree/panel.h"
#include "context/editor/gui/viewport/viewport_edit_panel.h"
#include "context/editor/gui/viewport/viewport_panel.h"

#include <string>
#include <utility>
#include <vector>

namespace context::editor::gui::a11y
{

namespace
{

namespace guic = context::editor::gui::contract;
namespace undo = context::editor::gui::session::undo;

// One roster id bound to the headless factory that builds its DEFAULT (empty-state) panel — the state
// the harness scans, exactly as each panel's own gui-*-test_a11y ctest covers its populated states.
//
// Every id here is taken from the panel class's OWN kContributionId constant (never a re-typed
// literal), so a rename cannot drift this table from the panel; and the gui-a11y-coverage ctest
// asserts this table matches the roster in both directions, so it cannot drift from the roster
// either.
std::vector<std::pair<std::string, PanelFactory>> panel_factories()
{
    std::vector<std::pair<std::string, PanelFactory>> factories;

    // M5-F0b — the built-in placeholder panel the CEF editor host boots (gui/uitree/builtin.h).
    factories.emplace_back("placeholder", &uitree::make_placeholder_panel);

    // M5-F2 — the scene-tree observer panel (gui/panels/scenetree/).
    factories.emplace_back(panels::scenetree::SceneTreePanel::kContributionId,
                           []() { return panels::scenetree::SceneTreePanel{}.build_panel(); });

    // M5-F3 — the inspector panel (gui/panels/inspector/).
    factories.emplace_back(panels::inspector::InspectorPanel::kContributionId,
                           []() { return panels::inspector::InspectorPanel{}.build_panel(); });

    // M5-F1 — the native viewport observer panel (gui/viewport/).
    factories.emplace_back(viewport::ViewportPanel::kContributionId,
                           []() { return viewport::ViewportPanel{}.build_panel(); });

    // M5-F5 — the play-in-editor playbar panel (gui/playbar/).
    factories.emplace_back(playbar::PlaybarModel::kContributionId,
                           []() { return playbar::build_playbar_panel(playbar::PlaybarModel{}); });

    // M5-F4 — the Problems observer panel (gui/panels/problems/).
    factories.emplace_back(panels::problems::ProblemsPanel::kContributionId,
                           []() { return panels::problems::ProblemsPanel{}.build_panel(); });

    // M8.5 a18 — the tilemap-painter authoring panel (gui/panels/tilemap/).
    factories.emplace_back(panels::tilemap::TilemapPaintPanel::kContributionId,
                           []() { return panels::tilemap::TilemapPaintPanel{}.build_panel(); });

    // M8.5 a19 — the in-context viewport override-editing panel (gui/viewport/).
    factories.emplace_back(viewport::ViewportEditPanel::kContributionId,
                           []() { return viewport::ViewportEditPanel{}.build_panel(); });

    // M8.5 a20 — the in-editor contextual Help panel (gui/help/).
    factories.emplace_back(help::HelpPanel::kContributionId,
                           []() { return help::HelpPanel{}.build_panel(); });

    // M5-F7 — the Ctrl+Z/Y session history surface (gui/session/undo/). ADDED BY M9 e05b (A-F2): the
    // journal shipped an a11y-clean headless panel in M5 but was absent from both the host registry
    // and this scan list, so its keyboard surface was never gated. Its DEFAULT (empty journal) state
    // exposes no command and no focusable node — a11y-clean, and accepted by tools/a11y_scan.py, which
    // only requires a focus order when a panel exposes commands.
    factories.emplace_back(undo::UndoJournal::kContributionId,
                           []() { return undo::UndoJournal{}.build_panel(); });

    return factories;
}

} // namespace

std::vector<std::string> panel_factory_ids()
{
    std::vector<std::string> ids;
    for (const std::pair<std::string, PanelFactory>& f : panel_factories())
    {
        ids.push_back(f.first);
    }
    return ids;
}

std::vector<RegisteredPanel> registered_panels()
{
    const std::vector<std::pair<std::string, PanelFactory>> factories = panel_factories();

    // Derived from the ROSTER, in roster order — this list cannot name a panel the roster does not
    // declare. The reverse direction (a roster entry with no factory here) would silently drop out of
    // the scan, so the gui-a11y-coverage ctest asserts it separately via panel_factory_ids().
    std::vector<RegisteredPanel> panels;
    for (const guic::Contribution& contribution : guic::builtin_contributions())
    {
        for (const std::pair<std::string, PanelFactory>& factory : factories)
        {
            if (factory.first == contribution.id)
            {
                panels.push_back(RegisteredPanel{contribution.id, factory.second});
                break;
            }
        }
    }
    return panels;
}

} // namespace context::editor::gui::a11y
