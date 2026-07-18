// The tile-painting model (M8.5 a18, R-2D-003 GUI half / R-EDIT-001 / R-A11Y-001 / L-20 / L-55):
// the headless, CEF-free logic of the tilemap-painter panel — the 2D orthographic
// viewport-authoring mode (ortho camera, pixel<->cell mapping with implicit grid snapping), the
// tile palette, the paint/erase/fill tools, and the L-20 gesture lifecycle. Session state
// (selection, camera, the in-flight gesture) lives HERE in memory; authored state exists only in
// files — a gesture COMMITS at gesture end as canonical file writes through the ONE
// editor/tilemap write-path core the CLI verbs also use (R-CLI-001: the GUI is sugar over the
// same write path). The whole model is CI-assertable WITHOUT booting CEF (R-EDIT-001).

#pragma once

#include "context/editor/filesync/file_store.h"
#include "context/editor/serializer/json_tree.h"
#include "context/editor/tilemap/tilemap_edit.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::gui::panels::tilemap
{

namespace tme = context::editor::tilemap;

// The authoring tools. Erase is a paint with tile 0 (the write path needs no second mode); fill
// commits the rectangle spanned by the gesture's anchor and last cell.
enum class Tool
{
    paint,
    erase,
    fill,
};

[[nodiscard]] const char* tool_name(Tool tool);

// --- the 2D orthographic viewport-authoring mode (R-2D-003) --------------------------------------
//
// An orthographic 2D camera over the tilemap plane: `center` in world meters, `zoom` in pixels per
// meter, a pixel viewport of `width_px` x `height_px`. screen_to_cell() floors into CELL
// coordinates — grid snapping is inherent to the mapping (a pointer position always lands on
// exactly one cell), which is what makes paint gestures deterministic. Presentation-only state
// (L-20 session state — never serialized into authored files; folds into no state hash).
struct Ortho2DViewport
{
    double center_x = 0.0; // world meters
    double center_y = 0.0;
    double zoom = 32.0; // pixels per meter, clamped to [kMinZoom, kMaxZoom]
    std::uint32_t width_px = 640;
    std::uint32_t height_px = 360;
    double tile_width = 1.0; // world meters per cell (the tilemap's tileSize)
    double tile_height = 1.0;

    static constexpr double kMinZoom = 4.0;
    static constexpr double kMaxZoom = 512.0;
    static constexpr double kZoomStep = 1.25;

    // World-meter position of a viewport pixel (pixel y grows down; world y grows down too — cell
    // rows are authored top-to-bottom, matching the chunk region convention).
    [[nodiscard]] double world_x_at(double px) const noexcept;
    [[nodiscard]] double world_y_at(double py) const noexcept;

    // The CELL under a viewport pixel (floor — implicit grid snap).
    [[nodiscard]] std::int64_t cell_x_at(double px) const noexcept;
    [[nodiscard]] std::int64_t cell_y_at(double py) const noexcept;

    // Center the camera on a cell's world-space center.
    void center_on_cell(std::int64_t cx, std::int64_t cy) noexcept;

    void pan(double dx_m, double dy_m) noexcept;
    void zoom_in() noexcept;
    void zoom_out() noexcept;
};

// One palette entry (a ctx:tilemap tile-set) / one selectable layer, as the panel lists them.
struct PaletteTileSet
{
    std::string id;
    std::string name;
    std::uint32_t first_tile = 1;
    std::uint32_t tile_count = 0; // 0 = open-ended (tileCount absent in the authored file)
};

struct LayerEntry
{
    std::string id;
    std::string name;
    bool visible = true;
};

// The outcome of a gesture-end commit (thin projection of the write-path core's result).
struct CommitOutcome
{
    bool ok = false;
    std::string error_code;
    std::string error_message;
    std::size_t cells_changed = 0;
};

class TilemapPaintModel
{
public:
    TilemapPaintModel() = default;

    // Load a tilemap document from `owner_path` under `fs` (read + canonicalize + extract the
    // palette/layers). Returns false (and stays unloaded) when the file is absent or not JSON.
    bool open(const filesync::FileStore& fs, std::string_view root, std::string_view owner_path);

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] const std::string& owner_path() const noexcept { return owner_path_; }
    [[nodiscard]] const serializer::JsonValue& doc() const noexcept { return doc_; }
    [[nodiscard]] const std::vector<PaletteTileSet>& tile_sets() const noexcept
    {
        return tile_sets_;
    }
    [[nodiscard]] const std::vector<LayerEntry>& layers() const noexcept { return layers_; }

    // --- selection (session state, L-20) ---------------------------------------------------------
    [[nodiscard]] const std::string& active_layer_id() const noexcept { return active_layer_id_; }
    bool select_layer(std::string_view layer_id);
    [[nodiscard]] std::uint32_t selected_tile() const noexcept { return selected_tile_; }
    // Select a specific GLOBAL tile id (must be 0 or inside a palette range).
    bool select_tile(std::uint32_t tile);
    // Step the selection through the palette's global id ranges (wrapping); the keyboard path for
    // palette browsing (R-A11Y-001 — no pointer required).
    void select_next_tile();
    void select_prev_tile();

    [[nodiscard]] Tool tool() const noexcept { return tool_; }
    void set_tool(Tool tool) noexcept { tool_ = tool; }

    // --- the 2D ortho viewport + keyboard cursor -------------------------------------------------
    [[nodiscard]] Ortho2DViewport& viewport() noexcept { return viewport_; }
    [[nodiscard]] const Ortho2DViewport& viewport() const noexcept { return viewport_; }
    [[nodiscard]] std::int64_t cursor_x() const noexcept { return cursor_x_; }
    [[nodiscard]] std::int64_t cursor_y() const noexcept { return cursor_y_; }
    // Move the keyboard cursor by whole cells; the camera follows so the cursor stays visible.
    void move_cursor(std::int64_t dx, std::int64_t dy);

    // --- the L-20 gesture lifecycle --------------------------------------------------------------
    //
    // A gesture accumulates cells IN MEMORY (paint/erase: every visited cell, later strokes over a
    // cell win; fill: the anchor..last rectangle). NOTHING touches disk until end_gesture()'s batch
    // is committed — the GUI commits at gesture end, there is no Save button (L-20).
    [[nodiscard]] bool gesture_active() const noexcept { return gesture_active_; }
    [[nodiscard]] std::size_t pending_cells() const noexcept;
    void begin_gesture_at_cell(std::int64_t cx, std::int64_t cy);
    void begin_gesture_at_pixel(double px, double py); // via the ortho viewport mapping
    void extend_gesture_to_cell(std::int64_t cx, std::int64_t cy);
    void extend_gesture_to_pixel(double px, double py);
    void cancel_gesture();
    // End the gesture and return its cell-edit batch (empty when no gesture / nothing visited).
    [[nodiscard]] std::vector<tme::CellEdit> end_gesture();

    // End the in-flight gesture and COMMIT its batch as canonical file writes through the shared
    // editor/tilemap core (apply_cell_edits + commit_edit — the SAME path `context tilemap paint`
    // runs). On success the model reloads the committed document so successive gestures see the
    // new authored state. An empty batch is a successful no-op.
    CommitOutcome commit_gesture(filesync::FileStore& fs, std::string_view root);

    // The keyboard-only authoring path (R-A11Y-001 / R-CLI-001 structural accessibility): apply the
    // active tool at the keyboard cursor as a complete one-cell gesture (fill: a 1x1 rect) and
    // commit it immediately.
    CommitOutcome apply_at_cursor(filesync::FileStore& fs, std::string_view root);

    // One-line status summary (tool | layer | tile | cursor | pending) for the panel's live status
    // region (R-A11Y-001: state changes surface as text, not color alone).
    [[nodiscard]] std::string status_text() const;

private:
    void visit_cell(std::int64_t cx, std::int64_t cy);
    [[nodiscard]] std::uint32_t gesture_tile() const noexcept;

    bool loaded_ = false;
    std::string owner_path_;
    serializer::JsonValue doc_;
    std::vector<PaletteTileSet> tile_sets_;
    std::vector<LayerEntry> layers_;

    std::string active_layer_id_;
    std::uint32_t selected_tile_ = 0;
    Tool tool_ = Tool::paint;

    Ortho2DViewport viewport_;
    std::int64_t cursor_x_ = 0;
    std::int64_t cursor_y_ = 0;

    bool gesture_active_ = false;
    std::int64_t anchor_x_ = 0; // the fill tool's rectangle anchor
    std::int64_t anchor_y_ = 0;
    std::int64_t last_x_ = 0;
    std::int64_t last_y_ = 0;
    std::vector<tme::CellEdit> visited_; // paint/erase strokes, in visit order
};

} // namespace context::editor::gui::panels::tilemap
