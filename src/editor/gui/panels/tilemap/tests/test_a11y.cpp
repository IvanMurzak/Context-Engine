// The tilemap-painter panel's per-panel a11y scan + keyboard-only navigation assertion (R-A11Y-001
// / R-EDIT-001), headless on the default matrix (no CEF) — the register-with-the-panel coverage the
// M5-F6 harness cross-checks (registry.cpp + coverage.manifest.jsonl carry the matching
// builtin.tilemap-painter entries). Asserts ZERO violations and a complete keyboard path across
// panel states: default/empty, loaded, each active tool, an in-flight gesture, and a keyboard-
// cursor authoring round-trip — every command reachable from a focusable node (R-CLI-001
// CLI-completeness as structural accessibility).

#include "tilemap_test.h"

#include "context/editor/gui/panels/tilemap/tilemap_paint_panel.h"
#include "context/editor/gui/uitree/panel.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace context::editor;
using namespace context::editor::gui::panels::tilemap;
namespace uitree = context::editor::gui::uitree;

namespace
{

// The a11y + keyboard-nav gate for one panel state: zero audit violations AND a non-empty tab
// order that reaches every always-on authoring affordance.
void assert_a11y_clean(const TilemapPaintPanel& panel)
{
    const uitree::Panel ui = panel.build_panel();
    const std::vector<uitree::A11yViolation> violations = uitree::audit_a11y(ui);
    for (const uitree::A11yViolation& v : violations)
        std::fprintf(stderr, "a11y violation: %s %s %s\n", v.node_id.c_str(), v.code.c_str(),
                     v.message.c_str());
    CHECK(violations.empty());

    const std::vector<std::string> order = uitree::focus_order(ui);
    CHECK(!order.empty());
    // Keyboard-only navigation reaches the toolbar, the cursor pad, and the apply affordance.
    const auto reaches = [&order](const char* id) {
        return std::find(order.begin(), order.end(), std::string(id)) != order.end();
    };
    CHECK(reaches("tilemap.tool.paint"));
    CHECK(reaches("tilemap.tool.erase"));
    CHECK(reaches("tilemap.tool.fill"));
    CHECK(reaches("tilemap.cursor.left"));
    CHECK(reaches("tilemap.cursor.right"));
    CHECK(reaches("tilemap.cursor.up"));
    CHECK(reaches("tilemap.cursor.down"));
    CHECK(reaches("tilemap.paint.at-cursor"));
    CHECK(reaches("tilemap.zoom.in"));
    CHECK(reaches("tilemap.zoom.out"));
}

} // namespace

int main()
{
    // Default (empty) state — exactly what the M5-F6 harness scans via registered_panels().
    assert_a11y_clean(TilemapPaintPanel{});

    // Loaded state: palette + layer rows are focusable and every row-bound command is reachable.
    {
        filesync::MemoryFileStore fs;
        TilemapPaintModel model;
        CHECK(tilemappaneltest::stage_and_open(fs, model));
        assert_a11y_clean(TilemapPaintPanel(std::move(model)));
    }

    // Each tool active (the active-state label must never break conformance).
    for (const Tool tool : {Tool::paint, Tool::erase, Tool::fill})
    {
        filesync::MemoryFileStore fs;
        TilemapPaintModel model;
        CHECK(tilemappaneltest::stage_and_open(fs, model));
        model.set_tool(tool);
        assert_a11y_clean(TilemapPaintPanel(std::move(model)));
    }

    // Mid-gesture (pending cells surface in the live status region, still conformant).
    {
        filesync::MemoryFileStore fs;
        TilemapPaintModel model;
        CHECK(tilemappaneltest::stage_and_open(fs, model));
        model.begin_gesture_at_cell(0, 0);
        model.extend_gesture_to_cell(1, 1);
        assert_a11y_clean(TilemapPaintPanel(std::move(model)));
    }

    // After a keyboard-only authoring round-trip (cursor move + apply + commit — the R-A11Y-001
    // "every GUI action has a non-pointer path" property exercised end-to-end).
    {
        filesync::MemoryFileStore fs;
        TilemapPaintModel model;
        CHECK(tilemappaneltest::stage_and_open(fs, model));
        model.move_cursor(1, 1);
        const CommitOutcome outcome = model.apply_at_cursor(fs, ".");
        CHECK(outcome.ok);
        assert_a11y_clean(TilemapPaintPanel(std::move(model)));
    }

    TILEMAP_PANEL_TEST_MAIN_END();
}
