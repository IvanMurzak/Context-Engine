// The panel composition root (see builtin_panels.h for the D10 layering rationale).

#include "context/editor/shell/panels/builtin_panels.h"

#include "context/editor/gui/panels/problems/problems_panel.h"
#include "context/editor/gui/uitree/builtin.h"

#include <utility>

namespace context::editor::shell::panels
{

namespace
{

// The roster id of the M5-F0b placeholder panel. A LITERAL because `gui/uitree/builtin.h` declares
// no id constant (builtin_roster.h records the same exception for the same reason) — the a11y
// coverage ctest is what keeps the two spellings honest.
constexpr const char* kPlaceholderPanelId = "placeholder";

} // namespace

const std::vector<std::string>& hostable_panel_ids()
{
    // Built once. Kept in lockstep with the bindings below by `test_builtin_panels.cpp`, which
    // asserts every id here is hosted after install and that nothing else is — so this list cannot
    // drift into a claim the bindings do not honor.
    static const std::vector<std::string> ids = {
        kPlaceholderPanelId,
        gui::panels::problems::ProblemsPanel::kContributionId,
    };
    return ids;
}

BuiltinPanels install_builtin_panels(PanelHost& host)
{
    BuiltinPanels out;

    // --- the placeholder (M5-F0b, context_gui_uitree) -------------------------------------------
    // A pure function of nothing: a static, a11y-conformant tree. It has no model to feed, no
    // commands to dispatch beyond its own, no gestures and no state — so its provider is `build`
    // ALONE, and `panel.list` reports `gestures:false, persists:false` for it. That minimal shape is
    // useful on its own terms: it proves the host and the hydration runtime work for a panel that
    // supplies nothing optional, which is the floor every future panel clears.
    {
        PanelProvider provider;
        provider.build = [] { return gui::uitree::make_placeholder_panel(); };
        if (host.provide(kPlaceholderPanelId, std::move(provider)))
        {
            ++out.bound;
        }
    }

    // --- Problems (M5-F4, context_gui_panel_problems) -------------------------------------------
    // The panel the e05d1 DoD proves the LIVE read path on. The feed owns the model and is driven
    // from the daemon's `diagnostics` topic by the caller (see problems_feed.h); binding the
    // provider here is what publishes it through `panel.*`.
    {
        auto feed =
            std::make_unique<ProblemsFeed>(host, gui::panels::problems::ProblemsPanel::kContributionId);
        if (host.provide(gui::panels::problems::ProblemsPanel::kContributionId,
                         feed->make_provider()))
        {
            ++out.bound;
            out.problems = std::move(feed);
        }
        // On a refused binding the feed is DROPPED rather than kept: a feed nothing routes to would
        // pump daemon events into a model no renderer can ever see.
    }

    return out;
}

} // namespace context::editor::shell::panels
