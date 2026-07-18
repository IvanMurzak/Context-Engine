// TilemapPaintPanel structure tests (R-QA-013): the built uitree — toolbar (active-tool label),
// palette rows + tile stepper, layer rows (active/hidden labels), the 2D-viewport controls, the
// status line — plus command registration (conditional palette/layer commands) and determinism
// (identical state renders byte-identical HTML).

#include "tilemap_test.h"

#include "context/editor/gui/panels/tilemap/tilemap_paint_panel.h"
#include "context/editor/gui/uitree/node.h"
#include "context/editor/gui/uitree/panel.h"

#include <string>

using namespace context::editor;
using namespace context::editor::gui::panels::tilemap;
namespace uitree = context::editor::gui::uitree;

namespace
{

[[nodiscard]] const uitree::UiNode* find_node(const uitree::UiNode& node, const std::string& id)
{
    if (node.id() == id)
        return &node;
    for (const uitree::UiNode& child : node.children())
        if (const uitree::UiNode* hit = find_node(child, id))
            return hit;
    return nullptr;
}

void test_empty_state()
{
    // The default (no document open) panel still renders the full authoring chrome, and registers
    // NO palette/layer selection command (no row binds them — they would be unreachable).
    const TilemapPaintPanel panel;
    const uitree::Panel ui = panel.build_panel();
    CHECK(ui.id() == std::string(TilemapPaintPanel::kContributionId));
    CHECK(ui.has_root());
    CHECK(ui.has_command(kToolPaintCommand));
    CHECK(ui.has_command(kPaintAtCursorCommand));
    CHECK(!ui.has_command(kSelectLayerCommand));
    CHECK(!ui.has_command(kSelectTileSetCommand));
    CHECK(!ui.has_command(kTilePrevCommand)); // palette stepper commands need a palette to bind to
    CHECK(!ui.has_command(kTileNextCommand));
    CHECK(find_node(ui.root(), "tilemap.palette.empty") != nullptr);
    CHECK(find_node(ui.root(), "tilemap.layers.empty") != nullptr);
    CHECK(find_node(ui.root(), "tilemap.status") != nullptr);
}

void test_loaded_structure()
{
    filesync::MemoryFileStore fs;
    TilemapPaintModel model;
    CHECK(tilemappaneltest::stage_and_open(fs, model));
    model.set_tool(Tool::erase);

    const TilemapPaintPanel panel(std::move(model));
    const uitree::Panel ui = panel.build_panel();

    // Rows exist for both layers and the tile-set; the palette/layer commands register now.
    CHECK(ui.has_command(kSelectLayerCommand));
    CHECK(ui.has_command(kSelectTileSetCommand));
    CHECK(ui.has_command(kTilePrevCommand));
    CHECK(ui.has_command(kTileNextCommand));
    const uitree::UiNode* ground =
        find_node(ui.root(), std::string("tilemap.layers.") + tilemappaneltest::kGroundLayerId);
    CHECK(ground != nullptr);
    CHECK(ground->label().find("(active)") != std::string::npos);
    const uitree::UiNode* props =
        find_node(ui.root(), std::string("tilemap.layers.") + tilemappaneltest::kPropsLayerId);
    CHECK(props != nullptr);
    CHECK(props->label().find("(hidden)") != std::string::npos);
    const uitree::UiNode* set = find_node(ui.root(), "tilemap.palette.set.7777777777777701");
    CHECK(set != nullptr);
    CHECK(set->label().find("terrain") != std::string::npos);
    CHECK(set->label().find("1-64") != std::string::npos);

    // The active tool is surfaced in its LABEL (not color alone).
    const uitree::UiNode* erase = find_node(ui.root(), "tilemap.tool.erase");
    CHECK(erase != nullptr);
    CHECK(erase->label().find("(active)") != std::string::npos);
    const uitree::UiNode* paint = find_node(ui.root(), "tilemap.tool.paint");
    CHECK(paint != nullptr);
    CHECK(paint->label().find("(active)") == std::string::npos);

    // The status line mirrors the model's status text.
    const uitree::UiNode* status = find_node(ui.root(), "tilemap.status");
    CHECK(status != nullptr);
    CHECK(status->text().find("Tool: erase") != std::string::npos);
}

void test_determinism()
{
    // Identical state renders byte-identical HTML (a re-render with an unchanged model is stable).
    filesync::MemoryFileStore fs_a;
    TilemapPaintModel model_a;
    CHECK(tilemappaneltest::stage_and_open(fs_a, model_a));
    filesync::MemoryFileStore fs_b;
    TilemapPaintModel model_b;
    CHECK(tilemappaneltest::stage_and_open(fs_b, model_b));

    const std::string html_a = uitree::render_html(TilemapPaintPanel(std::move(model_a)).build_panel());
    const std::string html_b = uitree::render_html(TilemapPaintPanel(std::move(model_b)).build_panel());
    CHECK(!html_a.empty());
    CHECK(html_a == html_b);
}

} // namespace

int main()
{
    test_empty_state();
    test_loaded_structure();
    test_determinism();
    TILEMAP_PANEL_TEST_MAIN_END();
}
