// The tilemap-painter editor panel (M8.5 a18, R-2D-003 GUI half / R-EDIT-001 / R-A11Y-001 / L-20):
// projects the headless TilemapPaintModel — the 2D orthographic viewport-authoring mode, the tile
// palette, the paint/erase/fill tools, and the keyboard-cursor authoring path — into a headless
// context_gui_uitree Panel. Every affordance is a command-bound focusable node, so the whole
// authoring loop is keyboard-complete (R-A11Y-001; R-CLI-001 as structural accessibility: each
// command's action is exactly a model call whose commit runs the SAME write path as `context
// tilemap paint|fill`). The whole panel is CI-assertable WITHOUT booting CEF.

#pragma once

#include "context/editor/gui/panels/tilemap/tilemap_paint_model.h"

#include "context/editor/gui/uitree/panel.h"

namespace context::editor::gui::panels::tilemap
{

// The panel command vocabulary (each bound to a focusable node so every action has a keyboard path).
inline constexpr const char* kToolPaintCommand = "tilemap.tool-paint";
inline constexpr const char* kToolEraseCommand = "tilemap.tool-erase";
inline constexpr const char* kToolFillCommand = "tilemap.tool-fill";
inline constexpr const char* kSelectLayerCommand = "tilemap.select-layer";
inline constexpr const char* kSelectTileSetCommand = "tilemap.select-tileset";
inline constexpr const char* kTilePrevCommand = "tilemap.tile-prev";
inline constexpr const char* kTileNextCommand = "tilemap.tile-next";
inline constexpr const char* kCursorLeftCommand = "tilemap.cursor-left";
inline constexpr const char* kCursorRightCommand = "tilemap.cursor-right";
inline constexpr const char* kCursorUpCommand = "tilemap.cursor-up";
inline constexpr const char* kCursorDownCommand = "tilemap.cursor-down";
inline constexpr const char* kPaintAtCursorCommand = "tilemap.paint-at-cursor";
inline constexpr const char* kZoomInCommand = "tilemap.zoom-in";
inline constexpr const char* kZoomOutCommand = "tilemap.zoom-out";

class TilemapPaintPanel
{
public:
    // The R-EDIT-001 contribution id this built-in panel registers under (a11y registry +
    // coverage.manifest.jsonl carry the matching entries).
    static constexpr const char* kContributionId = "builtin.tilemap-painter";

    TilemapPaintPanel() = default;
    explicit TilemapPaintPanel(TilemapPaintModel model) : model_(std::move(model)) {}

    [[nodiscard]] TilemapPaintModel& model() noexcept { return model_; }
    [[nodiscard]] const TilemapPaintModel& model() const noexcept { return model_; }

    // Build the headless uitree Panel for the model's current state. Deterministic: identical state
    // produces a byte-identical Panel (uitree::render_html). a11y-conformant by construction —
    // uitree::audit_a11y returns no violations for any model state, loaded or empty (the palette /
    // layer selection commands register only when a row binds them, so no command is unreachable).
    [[nodiscard]] uitree::Panel build_panel() const;

private:
    TilemapPaintModel model_;
};

} // namespace context::editor::gui::panels::tilemap
