// The built-in editor-panel registry (see registry.h). Append-only: a new M5 panel adds one entry
// here and one line to coverage.manifest.jsonl.

#include "context/editor/gui/a11y/registry.h"

#include "context/editor/gui/panels/inspector/inspector_panel.h"
#include "context/editor/gui/panels/problems/problems_panel.h"
#include "context/editor/gui/panels/scenetree/scene_tree_panel.h"
#include "context/editor/gui/panels/tilemap/tilemap_paint_panel.h"
#include "context/editor/gui/playbar/playbar_model.h"
#include "context/editor/gui/playbar/playbar_panel.h"
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

    // M5-F5 — the play-in-editor playbar panel (gui/playbar/). The harness scans its default (edit-mode,
    // no live session) rendered state, exactly as it scans the placeholder / scene-tree / inspector /
    // viewport defaults; the panel's own gui-playbar-test_a11y ctest additionally covers its playing /
    // paused / error states. Registered per issue #166 + the M5-F6 harness contract (coverage.manifest.
    // jsonl carries the matching builtin.playbar line). Completes the M5 observer fan-out (F1-F7).
    panels.push_back(RegisteredPanel{
        playbar::PlaybarModel::kContributionId,
        []() { return playbar::build_playbar_panel(playbar::PlaybarModel{}); }});

    // M5-F4 — the Problems observer panel (gui/panels/problems/). The harness scans its default (empty
    // diagnostic set) rendered state, exactly as it scans the other panels' defaults; the panel's own
    // gui-panel-problems-test_a11y ctest additionally covers its navigable / provisional / grouped
    // states. Registered by the M5 EXIT gate (issue #168): M5-F4 (#159) landed the panel but left it
    // uncovered (it landed before the F6 harness), so the exit gate completed the coverage manifest by
    // registering it HERE + adding the matching builtin.problems line to coverage.manifest.jsonl (the
    // gui-a11y-coverage guard + tools/a11y_scan.py cross-check the two). The old defensive per-panel
    // coverage/*.json fragments were removed once the monolithic manifest superseded them (issue #206).
    panels.push_back(RegisteredPanel{
        panels::problems::ProblemsPanel::kContributionId,
        []() { return panels::problems::ProblemsPanel{}.build_panel(); }});

    // M8.5 a18 — the tilemap-painter authoring panel (gui/panels/tilemap/, R-2D-003 GUI half). The
    // harness scans its default (no-document) rendered state, exactly as it scans the other panels'
    // defaults; the panel's own gui-panel-tilemap-test_a11y ctest additionally covers its loaded /
    // per-tool / mid-gesture / keyboard-authoring states. Registered per the register-with-the-panel
    // rule (coverage.manifest.jsonl carries the matching builtin.tilemap-painter line). First
    // AUTHORING panel: its gesture-end commit runs the shared editor/tilemap write path (L-20).
    panels.push_back(RegisteredPanel{
        panels::tilemap::TilemapPaintPanel::kContributionId,
        []() { return panels::tilemap::TilemapPaintPanel{}.build_panel(); }});

    return panels;
}

} // namespace context::editor::gui::a11y
