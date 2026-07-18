// Tilemap-painter panel builder (see tilemap_paint_panel.h).

#include "context/editor/gui/panels/tilemap/tilemap_paint_panel.h"

#include "context/editor/gui/uitree/node.h"

#include <string>

namespace context::editor::gui::panels::tilemap
{

namespace
{

using uitree::Role;
using uitree::UiNode;

[[nodiscard]] UiNode tool_button(const char* id, const char* label, const char* command,
                                 bool active)
{
    // The active tool is surfaced in the LABEL (not color alone — R-A11Y-001): "Paint (active)".
    std::string text = label;
    if (active)
        text += " (active)";
    return UiNode(Role::button, id).set_label(text).set_focusable(true).set_command(command);
}

} // namespace

uitree::Panel TilemapPaintPanel::build_panel() const
{
    uitree::Panel panel(kContributionId, "Tilemap Painter");

    // Commands: the always-rendered affordances register unconditionally; the palette/layer row
    // commands register ONLY when at least one row binds them (an unreachable command would be an
    // R-A11Y-001 violation the audit rejects).
    panel.add_command(kToolPaintCommand, "Select the paint tool");
    panel.add_command(kToolEraseCommand, "Select the erase tool");
    panel.add_command(kToolFillCommand, "Select the fill tool");
    panel.add_command(kCursorLeftCommand, "Move the paint cursor left");
    panel.add_command(kCursorRightCommand, "Move the paint cursor right");
    panel.add_command(kCursorUpCommand, "Move the paint cursor up");
    panel.add_command(kCursorDownCommand, "Move the paint cursor down");
    panel.add_command(kPaintAtCursorCommand, "Apply the active tool at the cursor");
    panel.add_command(kZoomInCommand, "Zoom the 2D viewport in");
    panel.add_command(kZoomOutCommand, "Zoom the 2D viewport out");
    if (!model_.tile_sets().empty())
    {
        // The palette affordances exist only when a palette exists — registering their commands
        // with no bound focusable node would be an unreachable-command a11y violation.
        panel.add_command(kSelectTileSetCommand, "Select a palette tile-set");
        panel.add_command(kTilePrevCommand, "Select the previous palette tile");
        panel.add_command(kTileNextCommand, "Select the next palette tile");
    }
    if (!model_.layers().empty())
        panel.add_command(kSelectLayerCommand, "Select the active layer");

    UiNode root(Role::region, "tilemap.root");
    root.set_label("Tilemap Painter");
    root.add_child(UiNode(Role::heading, "tilemap.title").set_label("Tilemap Painter"));

    // --- toolbar: paint / erase / fill -----------------------------------------------------------
    UiNode toolbar(Role::group, "tilemap.toolbar");
    toolbar.add_child(tool_button("tilemap.tool.paint", "Paint", kToolPaintCommand,
                                  model_.tool() == Tool::paint));
    toolbar.add_child(tool_button("tilemap.tool.erase", "Erase", kToolEraseCommand,
                                  model_.tool() == Tool::erase));
    toolbar.add_child(
        tool_button("tilemap.tool.fill", "Fill", kToolFillCommand, model_.tool() == Tool::fill));
    root.add_child(std::move(toolbar));

    // --- the tile palette (tile-sets + tile stepper) ---------------------------------------------
    UiNode palette(Role::region, "tilemap.palette");
    palette.set_label("Tile palette");
    if (model_.tile_sets().empty())
    {
        palette.add_child(
            UiNode(Role::text, "tilemap.palette.empty").set_text("No tilemap is open."));
    }
    else
    {
        UiNode sets(Role::list, "tilemap.palette.sets");
        for (const PaletteTileSet& set : model_.tile_sets())
        {
            const std::string range =
                set.tile_count == 0
                    ? "tiles " + std::to_string(set.first_tile) + "+"
                    : "tiles " + std::to_string(set.first_tile) + "-" +
                          std::to_string(set.first_tile + set.tile_count - 1);
            sets.add_child(UiNode(Role::listitem, "tilemap.palette.set." + set.id)
                               .set_label(set.name + " (" + range + ")")
                               .set_focusable(true)
                               .set_command(kSelectTileSetCommand));
        }
        palette.add_child(std::move(sets));
        palette.add_child(UiNode(Role::button, "tilemap.tile.prev")
                              .set_label("Previous tile")
                              .set_focusable(true)
                              .set_command(kTilePrevCommand));
        palette.add_child(UiNode(Role::button, "tilemap.tile.next")
                              .set_label("Next tile")
                              .set_focusable(true)
                              .set_command(kTileNextCommand));
        palette.add_child(UiNode(Role::status, "tilemap.tile.selected")
                              .set_label("Selected tile")
                              .set_text("Tile " + std::to_string(model_.selected_tile())));
    }
    root.add_child(std::move(palette));

    // --- the layer list --------------------------------------------------------------------------
    UiNode layers(Role::region, "tilemap.layers");
    layers.set_label("Layers");
    if (model_.layers().empty())
    {
        layers.add_child(UiNode(Role::text, "tilemap.layers.empty").set_text("No layers."));
    }
    else
    {
        UiNode rows(Role::list, "tilemap.layers.list");
        for (const LayerEntry& layer : model_.layers())
        {
            std::string label = layer.name;
            if (layer.id == model_.active_layer_id())
                label += " (active)";
            if (!layer.visible)
                label += " (hidden)";
            rows.add_child(UiNode(Role::listitem, "tilemap.layers." + layer.id)
                               .set_label(label)
                               .set_focusable(true)
                               .set_command(kSelectLayerCommand));
        }
        layers.add_child(std::move(rows));
    }
    root.add_child(std::move(layers));

    // --- the 2D orthographic authoring viewport (R-2D-003) ---------------------------------------
    // The pointer path paints through the SAME gesture calls the keyboard buttons drive; the canvas
    // node surfaces the camera/cursor state as text, and every viewport action is a command-bound
    // button so keyboard-only authoring is complete (R-A11Y-001).
    UiNode viewport(Role::region, "tilemap.viewport");
    viewport.set_label("2D viewport (orthographic authoring)");
    {
        const Ortho2DViewport& vp = model_.viewport();
        viewport.add_child(
            UiNode(Role::status, "tilemap.viewport.camera")
                .set_label("Viewport camera")
                .set_text("Zoom " + std::to_string(static_cast<long long>(vp.zoom)) +
                          " px/m, cursor cell (" + std::to_string(model_.cursor_x()) + ", " +
                          std::to_string(model_.cursor_y()) + ")"));
        UiNode controls(Role::group, "tilemap.viewport.controls");
        controls.add_child(UiNode(Role::button, "tilemap.cursor.left")
                               .set_label("Cursor left")
                               .set_focusable(true)
                               .set_command(kCursorLeftCommand));
        controls.add_child(UiNode(Role::button, "tilemap.cursor.right")
                               .set_label("Cursor right")
                               .set_focusable(true)
                               .set_command(kCursorRightCommand));
        controls.add_child(UiNode(Role::button, "tilemap.cursor.up")
                               .set_label("Cursor up")
                               .set_focusable(true)
                               .set_command(kCursorUpCommand));
        controls.add_child(UiNode(Role::button, "tilemap.cursor.down")
                               .set_label("Cursor down")
                               .set_focusable(true)
                               .set_command(kCursorDownCommand));
        controls.add_child(UiNode(Role::button, "tilemap.paint.at-cursor")
                               .set_label("Apply tool at cursor")
                               .set_focusable(true)
                               .set_command(kPaintAtCursorCommand));
        controls.add_child(UiNode(Role::button, "tilemap.zoom.in")
                               .set_label("Zoom in")
                               .set_focusable(true)
                               .set_command(kZoomInCommand));
        controls.add_child(UiNode(Role::button, "tilemap.zoom.out")
                               .set_label("Zoom out")
                               .set_focusable(true)
                               .set_command(kZoomOutCommand));
        viewport.add_child(std::move(controls));
    }
    root.add_child(std::move(viewport));

    // --- the live status line (tool | layer | tile | cursor | pending) ---------------------------
    root.add_child(UiNode(Role::status, "tilemap.status")
                       .set_label("Painter status")
                       .set_text(model_.status_text()));

    panel.set_root(std::move(root));
    return panel;
}

} // namespace context::editor::gui::panels::tilemap
