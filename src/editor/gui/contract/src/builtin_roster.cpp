// The built-in editor-UI roster (see builtin_roster.h) — the single source of truth for the panels
// the editor ships, as panel-manifest-v3 Contributions.
//
// ADDING A BUILT-IN PANEL is a FOUR-anchor edit, mechanically cross-checked by two standing ctests:
//   1. append its Contribution HERE (the roster / listing order),
//   2. bind its headless factory in gui/a11y/registry.cpp + link its library into context_gui_a11y,
//   3. append its line to gui/a11y/coverage.manifest.jsonl,
//   4. append its PanelHelp entry to help::panel_topics() (gui/help/src/help_model.cpp).
// ⚠ A `ContentType::local` panel (M9 e06d — editor-core renders it from the kit; there IS no C++
// model) skips anchor 2 BY CONSTRUCTION and pays for it on anchor 3: its coverage.manifest.jsonl line
// MUST declare `ts-a11y`, which is what routes its a11y gate to the `webui-ts-*` browser tier over the
// real DOM. gui-a11y-coverage enforces exactly that split — a local panel with a C++ factory, or
// without the `ts-a11y` marker, is a red build. Anchors 1 and 4 are unchanged for it.
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

// One built-in panel manifest, as a NAMED-FIELD aggregate rather than a positional factory.
//
// Built-ins are all `uitree` content (their C++ panel model IS the content) at the read/query
// capability baseline; a built-in that needs more states it explicitly. `content` is a member here
// because the ONE exception (M9 e06d's Settings panel, whose model lives in the RENDERER —
// `ContentType::local`) differs from its neighbours in exactly that one field, so it says so rather
// than reaching a second factory. See extension.h's `ContentType::local` for when that is the right
// answer and when it is not.
//
// WHY DESIGNATED INITIALIZERS. This started as an 8-positional factory, and manifest v3 grew it by
// two — `InstanceMode mode, std::string path` — leaving `icon` and `path` as two `std::string`s
// three slots apart and `min_width`/`min_height` as two adjacent `int`s, i.e. a swapped pair that
// compiles silently. v3 also added `selection` and `events` that the old signature could not express
// AT ALL, so no roster entry could exercise the built-in carve-out `names_defect` grants it (naming
// an unnamespaced contract-owned subject). Named fields fix both: the next manifest member is a
// defaulted field and ZERO call-site churn, and every call site states what it sets.
//
// `package_id` stays EMPTY for every entry here — these ARE the editor's own contributions, which is
// what buys a built-in that carve-out. `commands` stays empty ON PURPOSE: a C++ panel declares its
// commands on its uitree::Panel model (which the a11y audit already proves keyboard-reachable), and
// duplicating them here would create a second source of truth free to drift; the manifest `commands`
// array exists for iframe contributions, which have no C++ model to read them from (04 §3/§5).
// contract_version + sandbox keep their defaults (kContractMajor + least privilege).
struct BuiltinPanel
{
    std::string id;
    std::string title;
    std::string icon;
    DockZone zone = DockZone::center;
    // manifest v3 (04 §2): `instances` replaced v2's `bool singleton`; `path` is the Window menu's
    // grouping (d1), display text only and inert until that menu lands, which is expected.
    InstanceMode mode = InstanceMode::singleton;
    std::string path;
    int min_width = 0;
    int min_height = 0;
    std::vector<std::string> capabilities;
    ContentType content = ContentType::uitree;
    // ⚠ The `{}` on these two is LOAD-BEARING, not decoration. No built-in declares either today, so
    // every call site omits them — and under `-Werror=missing-field-initializers` (ContextWarnings)
    // GCC refuses a designated-initializer list that leaves a member with NO default member
    // initializer unmentioned. A bare `SelectionSpec selection;` reds all three build legs; `= {}`
    // is what makes "omit what you do not set" legal. Any member added below needs one too.
    SelectionSpec selection = {};
    EventSpec events = {};
};

Contribution to_contribution(BuiltinPanel panel)
{
    Contribution c;
    c.id = std::move(panel.id);
    c.kind = ContributionKind::panel;
    c.title = std::move(panel.title);
    c.icon = std::move(panel.icon);
    c.dock.default_zone = panel.zone;
    c.dock.min_width = panel.min_width;
    c.dock.min_height = panel.min_height;
    c.instances.mode = panel.mode;
    c.path = std::move(panel.path);
    c.content.type = panel.content;
    c.state.schema_version = 1;
    c.capabilities = std::move(panel.capabilities);
    c.selection = std::move(panel.selection);
    c.events = std::move(panel.events);
    return c;
}

std::vector<Contribution> build_roster()
{
    using Caps = std::vector<std::string>;

    std::vector<Contribution> roster;

    // M5-F0b — the built-in placeholder panel the CEF editor host boots (gui/uitree/builtin.h).
    roster.push_back(to_contribution({.id = "placeholder",
                                      .title = "Context Editor",
                                      .icon = "logo",
                                      .zone = DockZone::center,
                                      .mode = InstanceMode::singleton,
                                      .path = "General",
                                      .min_width = 320,
                                      .min_height = 200,
                                      .capabilities = Caps{kCapabilityReadQuery}}));

    // M5-F2 — the scene-tree observer panel (gui/panels/scenetree/).
    roster.push_back(to_contribution({.id = "builtin.scene-tree",
                                      .title = "Scene Tree",
                                      .icon = "tree",
                                      .zone = DockZone::left,
                                      .mode = InstanceMode::singleton,
                                      .path = "Scene",
                                      .min_width = 240,
                                      .min_height = 200,
                                      .capabilities = Caps{kCapabilityReadQuery}}));

    // M9 e1 — the Files observer panel (gui/panels/files/), D10 read half. Publishes
    // `subject: "file"` selections through the c1 typed-selection surface; read-only (the write
    // half — rename/move/delete — is task e2, which will add file_write).
    roster.push_back(to_contribution({.id = "builtin.files",
                                      .title = "Files",
                                      .icon = "folder",
                                      .zone = DockZone::left,
                                      .mode = InstanceMode::singleton,
                                      .path = "Project",
                                      .min_width = 240,
                                      .min_height = 200,
                                      .capabilities = Caps{kCapabilityReadQuery}}));

    // M5-F3 — the inspector panel (gui/panels/inspector/). Authors composed overrides through the
    // ONE L-30 write path, so it declares the file_write grant explicitly (never ambient).
    roster.push_back(
        to_contribution({.id = "builtin.inspector",
                         .title = "Inspector",
                         .icon = "inspect",
                         .zone = DockZone::right,
                         .mode = InstanceMode::singleton,
                         .path = "Scene",
                         .min_width = 280,
                         .min_height = 200,
                         .capabilities = Caps{kCapabilityReadQuery, kCapabilityFileWrite}}));

    // M5-F1 — the native viewport observer panel (gui/viewport/). Read-only observer.
    //
    // THE ONE NON-SINGLETON BUILT-IN, and it was already declared so under v2 (`singleton: false`) —
    // `unlimited` is that same statement in the v3 vocabulary. Several scene views at once is the
    // point of a viewport, which makes it the natural first proof that c3's instance runtime works.
    roster.push_back(to_contribution({.id = "builtin.viewport",
                                      .title = "Viewport",
                                      .icon = "viewport",
                                      .zone = DockZone::center,
                                      .mode = InstanceMode::unlimited,
                                      .path = "Scene",
                                      .min_width = 320,
                                      .min_height = 240,
                                      .capabilities = Caps{kCapabilityReadQuery}}));

    // M5-F5's docked `builtin.playbar` was RETIRED here by editor-window-chrome e1 (D2): the d1
    // titlebar strip is the Play Bar's ONLY home now, driving the same `PlaybarModel`/`SessionFeed`
    // transport over `session.control` (shell/session_bridge.h). All four anchors went together —
    // this roster entry, the a11y factory + manifest row, and the help topic — so the standing
    // anchor gates stay in lockstep at the smaller set.

    // M5-F4 — the Problems observer panel (gui/panels/problems/).
    roster.push_back(to_contribution({.id = "builtin.problems",
                                      .title = "Problems",
                                      .icon = "warning",
                                      .zone = DockZone::bottom,
                                      .mode = InstanceMode::singleton,
                                      .path = "Diagnostics",
                                      .min_width = 320,
                                      .min_height = 120,
                                      .capabilities = Caps{kCapabilityReadQuery}}));

    // M8.5 a18 — the tilemap-painter authoring panel (gui/panels/tilemap/, R-2D-003).
    roster.push_back(
        to_contribution({.id = "builtin.tilemap-painter",
                         .title = "Tilemap Painter",
                         .icon = "brush",
                         .zone = DockZone::right,
                         .mode = InstanceMode::singleton,
                         .path = "Scene/2D",
                         .min_width = 280,
                         .min_height = 240,
                         .capabilities = Caps{kCapabilityReadQuery, kCapabilityFileWrite}}));

    // M8.5 a19 — the in-context viewport override-editing panel (gui/viewport/, R-HUX-006).
    roster.push_back(
        to_contribution({.id = "builtin.viewport-edit",
                         .title = "Viewport Edit",
                         .icon = "gizmo",
                         .zone = DockZone::right,
                         .mode = InstanceMode::singleton,
                         .path = "Scene",
                         .min_width = 280,
                         .min_height = 200,
                         .capabilities = Caps{kCapabilityReadQuery, kCapabilityFileWrite}}));

    // M8.5 a20 — the in-editor contextual Help / getting-started panel (gui/help/, R-HUX-010).
    roster.push_back(to_contribution({.id = "builtin.help",
                                      .title = "Help",
                                      .icon = "help",
                                      .zone = DockZone::right,
                                      .mode = InstanceMode::singleton,
                                      .path = "Help",
                                      .min_width = 280,
                                      .min_height = 200,
                                      .capabilities = Caps{kCapabilityReadQuery}}));

    // M5-F7 — the Ctrl+Z/Y session history surface (gui/session/undo/, R-HUX-001). PROMOTED into the
    // roster by M9 e05b (A-F2): it shipped with a headless, a11y-clean uitree panel but was absent
    // from BOTH the host registry and the a11y scan list, so its keyboard surface was never gated.
    // Its undo/redo replays are override writes through the ONE L-30 path, hence the file_write grant.
    roster.push_back(
        to_contribution({.id = "builtin.session.undo",
                         .title = "Session History",
                         .icon = "history",
                         .zone = DockZone::bottom,
                         .mode = InstanceMode::singleton,
                         .path = "Session",
                         .min_width = 240,
                         .min_height = 120,
                         .capabilities = Caps{kCapabilityReadQuery, kCapabilityFileWrite}}));

    // M9 e06d — the Settings panel (06 §4 / C-F14): theme picker, keymap-file shortcut, update info.
    // The ONE `local` panel: its subject is the RENDERER's own state (the active theme = the CSS
    // custom properties on the editor-core document), so editor-core renders it from the e06c kit and
    // there is no C++ model to bind. `read_query` only — it changes NOTHING in the project; the user
    // config it does change is written by the Shell through `config.set`, never by this panel.
    roster.push_back(to_contribution({.id = "builtin.settings",
                                      .title = "Settings",
                                      .icon = "settings",
                                      .zone = DockZone::right,
                                      .mode = InstanceMode::singleton,
                                      .path = "General",
                                      .min_width = 280,
                                      .min_height = 200,
                                      .capabilities = Caps{kCapabilityReadQuery},
                                      .content = ContentType::local}));

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
