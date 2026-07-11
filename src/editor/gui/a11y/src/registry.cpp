// The built-in editor-panel registry (see registry.h). Append-only: a new M5 panel adds one entry
// here and one line to coverage.manifest.jsonl.

#include "context/editor/gui/a11y/registry.h"

#include "context/editor/gui/panels/inspector/inspector_panel.h"
#include "context/editor/gui/panels/scenetree/scene_tree_panel.h"
#include "context/editor/gui/uitree/builtin.h"
#include "context/editor/gui/uitree/panel.h"
#include "context/editor/gui/viewport/viewport_panel.h"

namespace context::editor::gui::a11y
{

std::vector<RegisteredPanel> registered_panels()
{
    std::vector<RegisteredPanel> panels;

    // M5-F0b — the built-in placeholder panel the CEF editor host boots (gui/uitree/builtin.h).
    panels.push_back(RegisteredPanel{"placeholder", &uitree::make_placeholder_panel});

    // --- M5 fan-out panels append their RegisteredPanel BELOW (one entry each). Keep in lockstep
    //     with coverage.manifest.jsonl — tools/a11y_scan.py cross-checks the two on every PR. ---

    // M5-F2 — the scene-tree observer panel (gui/panels/scenetree/). The harness scans its default
    // (empty-world) rendered state, exactly as it scans the placeholder's default; the panel's own
    // gui-panel-scenetree-test_a11y ctest additionally covers its populated / deep / overridden
    // worlds. Registered here per issue #154's Coordination clause ("if F2 lands its panel first,
    // this harness must scan it") now that M5-F2 (#156) has landed on main.
    panels.push_back(RegisteredPanel{
        panels::scenetree::SceneTreePanel::kContributionId,
        []() { return panels::scenetree::SceneTreePanel{}.build_panel(); }});

    // M5-F3 — the inspector panel (gui/panels/inspector/). The harness scans its default
    // (no-selection) rendered state, exactly as it scans the placeholder / scene-tree defaults; the
    // panel's own gui-panel-inspector-test_a11y ctest additionally covers its populated / overridden /
    // all-readonly worlds. Registered per issue #160's Coordination clause + the M5-F6 harness contract
    // (coverage.manifest.jsonl carries the matching builtin.inspector line).
    panels.push_back(RegisteredPanel{
        panels::inspector::InspectorPanel::kContributionId,
        []() { return panels::inspector::InspectorPanel{}.build_panel(); }});

    // M5-F1 — the native viewport observer panel (gui/viewport/). The harness scans its default
    // (presentable, empty-scene) rendered state, exactly as it scans the placeholder / scene-tree /
    // inspector defaults; the panel's own gui-viewport-test_a11y ctest additionally covers its
    // adapter-absent / surface-unavailable / render-failed / populated states. Registered per issue
    // #164 + the M5-F6 harness contract (coverage.manifest.jsonl carries the matching builtin.viewport
    // line). Read-only observer — no new authoring surface (R-HUX-006 in-viewport editing is M8.5).
    panels.push_back(RegisteredPanel{
        viewport::ViewportPanel::kContributionId,
        []() { return viewport::ViewportPanel{}.build_panel(); }});

    return panels;
}

} // namespace context::editor::gui::a11y
