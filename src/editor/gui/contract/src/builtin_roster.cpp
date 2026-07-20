// The built-in editor-UI roster (see builtin_roster.h) — the single source of truth for the panels
// the editor ships, as panel-manifest-v2 Contributions.
//
// ADDING A BUILT-IN PANEL is a FOUR-anchor edit, mechanically cross-checked by two standing ctests:
//   1. append its Contribution HERE (the roster / listing order),
//   2. bind its headless factory in gui/a11y/registry.cpp + link its library into context_gui_a11y,
//   3. append its line to gui/a11y/coverage.manifest.jsonl,
//   4. append its PanelHelp entry to help::panel_topics() (gui/help/src/help_model.cpp).
// Anchors 1-3 are guarded by gui-a11y-coverage; anchor 4 by gui-help-contextual (and the
// m85-exit-4c-contextual-help milestone gate) — a DIFFERENT ctest, which is why a roster addition that
// skips the help topic passes the a11y guard and still reds the build. Both fail on the default 3-OS
// build matrix AND the local dev gate, each naming the anchor you missed.

#include "context/editor/gui/contract/builtin_roster.h"

#include "context/editor/gui/contract/extension.h"
#include "context/editor/gui/contract/registry.h"

#include <string>
#include <utility>
#include <vector>

namespace context::editor::gui::contract
{

namespace
{

// One built-in panel manifest. Built-ins are all `uitree` content (their C++ panel model IS the
// content) at the read/query capability baseline; a built-in that needs more states it explicitly.
Contribution builtin_panel(std::string id, std::string title, std::string icon, DockZone zone,
                           bool singleton, int min_width, int min_height,
                           std::vector<std::string> capabilities)
{
    Contribution c;
    c.id = std::move(id);
    c.kind = ContributionKind::panel;
    c.title = std::move(title);
    c.icon = std::move(icon);
    c.dock.default_zone = zone;
    c.dock.singleton = singleton;
    c.dock.min_width = min_width;
    c.dock.min_height = min_height;
    c.content.type = ContentType::uitree;
    c.state.schema_version = 1;
    c.capabilities = std::move(capabilities);
    // `commands` stays EMPTY for a built-in ON PURPOSE: a C++ panel declares its commands on its
    // uitree::Panel model (which the a11y audit already proves keyboard-reachable), and duplicating
    // them here would create a second source of truth free to drift. The manifest `commands` array
    // exists for iframe contributions, which have no C++ model to read them from (04 §3/§5).
    // contract_version + sandbox keep their defaults (kContractMajor + least privilege).
    return c;
}

std::vector<Contribution> build_roster()
{
    using Caps = std::vector<std::string>;

    std::vector<Contribution> roster;

    // M5-F0b — the built-in placeholder panel the CEF editor host boots (gui/uitree/builtin.h).
    roster.push_back(builtin_panel("placeholder", "Context Editor", "logo", DockZone::center, true,
                                   320, 200, Caps{kCapabilityReadQuery}));

    // M5-F2 — the scene-tree observer panel (gui/panels/scenetree/).
    roster.push_back(builtin_panel("builtin.scene-tree", "Scene Tree", "tree", DockZone::left, true,
                                   240, 200, Caps{kCapabilityReadQuery}));

    // M5-F3 — the inspector panel (gui/panels/inspector/). Authors composed overrides through the
    // ONE L-30 write path, so it declares the file_write grant explicitly (never ambient).
    roster.push_back(builtin_panel("builtin.inspector", "Inspector", "inspect", DockZone::right,
                                   true, 280, 200,
                                   Caps{kCapabilityReadQuery, kCapabilityFileWrite}));

    // M5-F1 — the native viewport observer panel (gui/viewport/). Read-only observer.
    roster.push_back(builtin_panel("builtin.viewport", "Viewport", "viewport", DockZone::center,
                                   false, 320, 240, Caps{kCapabilityReadQuery}));

    // M5-F5 — the play-in-editor playbar panel (gui/playbar/). Drives the live session.
    roster.push_back(builtin_panel("builtin.playbar", "Play Bar", "play", DockZone::top, true, 240,
                                   48, Caps{kCapabilityReadQuery, kCapabilitySessionControl}));

    // M5-F4 — the Problems observer panel (gui/panels/problems/).
    roster.push_back(builtin_panel("builtin.problems", "Problems", "warning", DockZone::bottom,
                                   true, 320, 120, Caps{kCapabilityReadQuery}));

    // M8.5 a18 — the tilemap-painter authoring panel (gui/panels/tilemap/, R-2D-003).
    roster.push_back(builtin_panel("builtin.tilemap-painter", "Tilemap Painter", "brush",
                                   DockZone::right, true, 280, 240,
                                   Caps{kCapabilityReadQuery, kCapabilityFileWrite}));

    // M8.5 a19 — the in-context viewport override-editing panel (gui/viewport/, R-HUX-006).
    roster.push_back(builtin_panel("builtin.viewport-edit", "Viewport Edit", "gizmo",
                                   DockZone::right, true, 280, 200,
                                   Caps{kCapabilityReadQuery, kCapabilityFileWrite}));

    // M8.5 a20 — the in-editor contextual Help / getting-started panel (gui/help/, R-HUX-010).
    roster.push_back(builtin_panel("builtin.help", "Help", "help", DockZone::right, true, 280, 200,
                                   Caps{kCapabilityReadQuery}));

    // M5-F7 — the Ctrl+Z/Y session history surface (gui/session/undo/, R-HUX-001). PROMOTED into the
    // roster by M9 e05b (A-F2): it shipped with a headless, a11y-clean uitree panel but was absent
    // from BOTH the host registry and the a11y scan list, so its keyboard surface was never gated.
    // Its undo/redo replays are override writes through the ONE L-30 path, hence the file_write grant.
    roster.push_back(builtin_panel("builtin.session.undo", "Session History", "history",
                                   DockZone::bottom, true, 240, 120,
                                   Caps{kCapabilityReadQuery, kCapabilityFileWrite}));

    return roster;
}

} // namespace

const std::vector<Contribution>& builtin_contributions()
{
    static const std::vector<Contribution> roster = build_roster();
    return roster;
}

ExtensionRegistry make_builtin_registry(bool* all_ok)
{
    ExtensionRegistry registry;
    bool ok = true;
    for (const Contribution& c : builtin_contributions())
    {
        if (!registry.register_contribution(c).ok)
        {
            ok = false;
        }
    }
    if (all_ok != nullptr)
    {
        *all_ok = ok;
    }
    return registry;
}

} // namespace context::editor::gui::contract
