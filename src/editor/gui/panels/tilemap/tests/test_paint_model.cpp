// TilemapPaintModel tests (R-QA-013 happy + edge + failure): the 2D ortho viewport pixel<->cell
// mapping (grid snap across zoom/pan), palette stepping, the L-20 gesture lifecycle (accumulate /
// last-wins / cancel / fill-rect), the keyboard-cursor authoring path, and the gesture-end commit —
// asserted to run the SAME write path as the CLI core (byte-identical owner + sidecar output), with
// the committed diff canonical and hot-reload-coherent (raw hash moves, refs verify).

#include "tilemap_test.h"

#include "context/editor/filesync/content_hash.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace context::editor;
using namespace context::editor::gui::panels::tilemap;
using tilemappaneltest::kGroundLayerId;
using tilemappaneltest::kOwnerPath;
using tilemappaneltest::kPropsLayerId;
namespace tme = context::editor::tilemap;

namespace
{

void test_viewport_mapping()
{
    Ortho2DViewport vp;
    vp.width_px = 640;
    vp.height_px = 360;
    vp.zoom = 32.0;
    vp.tile_width = 1.0;
    vp.tile_height = 1.0;
    vp.center_x = 0.0;
    vp.center_y = 0.0;

    // The viewport center pixel maps to world (0, 0) -> cell (0, 0) after snap... world 0.0 floors
    // into cell 0; one pixel left of center is world -1/32 m -> cell -1 (the snap boundary is the
    // cell edge, exactly the grid).
    CHECK(vp.cell_x_at(320.0) == 0);
    CHECK(vp.cell_x_at(319.0) == -1);
    CHECK(vp.cell_y_at(180.0) == 0);
    CHECK(vp.cell_y_at(179.0) == -1);
    // One tile right of center = 32 px at this zoom.
    CHECK(vp.cell_x_at(320.0 + 32.0) == 1);

    // Zoom is clamped and pixel->cell respects it.
    vp.zoom_in();
    CHECK(vp.zoom == 40.0); // 32 * 1.25
    for (int i = 0; i < 100; ++i)
        vp.zoom_in();
    CHECK(vp.zoom == Ortho2DViewport::kMaxZoom);
    for (int i = 0; i < 100; ++i)
        vp.zoom_out();
    CHECK(vp.zoom == Ortho2DViewport::kMinZoom);

    // Pan moves the mapping; center_on_cell recenters on the cell's world center.
    vp.zoom = 32.0;
    vp.pan(4.0, 0.0);
    CHECK(vp.cell_x_at(320.0) == 4);
    vp.center_on_cell(10, -3);
    CHECK(vp.cell_x_at(320.0) == 10);
    CHECK(vp.cell_y_at(180.0) == -3);

    // Non-unit tile size: 2 m tiles halve the cell frequency.
    vp.tile_width = 2.0;
    vp.center_x = 0.0;
    CHECK(vp.cell_x_at(320.0 + 32.0) == 0); // 1 m into a 2 m tile -> still cell 0
    CHECK(vp.cell_x_at(320.0 + 65.0) == 1);
}

void test_open_and_palette()
{
    filesync::MemoryFileStore fs;
    TilemapPaintModel model;
    CHECK(tilemappaneltest::stage_and_open(fs, model));
    CHECK(model.loaded());
    CHECK(model.tile_sets().size() == 1);
    CHECK(model.tile_sets()[0].name == "terrain");
    CHECK(model.layers().size() == 2);
    CHECK(model.active_layer_id() == kGroundLayerId); // defaults to the first layer
    CHECK(model.selected_tile() == 1);                // the first set's first tile

    // Palette stepping wraps within the known global ranges.
    model.select_next_tile();
    CHECK(model.selected_tile() == 2);
    CHECK(model.select_tile(64));
    model.select_next_tile();
    CHECK(model.selected_tile() == 1); // wrap
    model.select_prev_tile();
    CHECK(model.selected_tile() == 64); // wrap back
    CHECK(!model.select_tile(1000));    // outside every range
    CHECK(model.select_tile(0));        // 0 (empty) is always selectable

    CHECK(model.select_layer(kPropsLayerId));
    CHECK(model.active_layer_id() == kPropsLayerId);
    CHECK(!model.select_layer("nope"));

    // Opening a missing / non-JSON path fails without loading.
    TilemapPaintModel none;
    CHECK(!none.open(fs, ".", "absent.tilemap.json"));
    CHECK(!none.loaded());
}

void test_gesture_lifecycle()
{
    filesync::MemoryFileStore fs;
    TilemapPaintModel model;
    CHECK(tilemappaneltest::stage_and_open(fs, model));
    CHECK(model.select_tile(5));

    // Paint: visited cells accumulate, re-visits keep ONE edit with the LAST stroke.
    model.begin_gesture_at_cell(0, 0);
    model.extend_gesture_to_cell(1, 0);
    model.extend_gesture_to_cell(1, 0);
    model.extend_gesture_to_cell(2, 1);
    CHECK(model.gesture_active());
    CHECK(model.pending_cells() == 3);
    std::vector<tme::CellEdit> batch = model.end_gesture();
    CHECK(!model.gesture_active());
    CHECK(batch.size() == 3);
    CHECK(batch[0].x == 0 && batch[0].y == 0 && batch[0].tile == 5U);
    CHECK(batch[2].x == 2 && batch[2].y == 1);

    // Erase strokes carry tile 0.
    model.set_tool(Tool::erase);
    model.begin_gesture_at_cell(3, 3);
    batch = model.end_gesture();
    CHECK(batch.size() == 1);
    CHECK(batch[0].tile == 0U);

    // Fill: the anchor..last rectangle expands at gesture end (pixel-driven via the viewport).
    model.set_tool(Tool::fill);
    model.viewport().center_on_cell(0, 0);
    model.begin_gesture_at_pixel(320.0, 180.0); // cell (0, 0)
    model.extend_gesture_to_cell(2, 1);
    CHECK(model.pending_cells() == 6); // 3 x 2 rect
    batch = model.end_gesture();
    CHECK(batch.size() == 6);
    CHECK(batch[0].tile == 5U);

    // Cancel discards without a batch.
    model.set_tool(Tool::paint);
    model.begin_gesture_at_cell(0, 0);
    model.cancel_gesture();
    CHECK(model.end_gesture().empty());
}

void test_keyboard_cursor_path()
{
    filesync::MemoryFileStore fs;
    TilemapPaintModel model;
    CHECK(tilemappaneltest::stage_and_open(fs, model));
    CHECK(model.select_tile(9));

    model.move_cursor(2, 0);
    model.move_cursor(0, 3);
    CHECK(model.cursor_x() == 2);
    CHECK(model.cursor_y() == 3);
    // The camera follows the cursor (the cursor cell stays the one under the viewport center).
    CHECK(model.viewport().cell_x_at(model.viewport().width_px / 2.0) == 2);
    CHECK(model.viewport().cell_y_at(model.viewport().height_px / 2.0) == 3);

    // Keyboard-only authoring: apply the tool at the cursor as a complete committed gesture.
    const CommitOutcome outcome = model.apply_at_cursor(fs, ".");
    CHECK(outcome.ok);
    CHECK(outcome.cells_changed == 1);

    // The authored file now holds the painted cell (re-open a FRESH model — files are the truth).
    TilemapPaintModel fresh;
    CHECK(fresh.open(fs, ".", kOwnerPath));
    const std::optional<std::string> sidecar = fs.read("tilemaps/map.ground.cells.bin");
    CHECK(sidecar.has_value());
    const filesync::SidecarDecodeResult decoded = filesync::decode_sidecar(*sidecar);
    CHECK(decoded.ok);
    const std::vector<std::uint32_t> cells = tme::decode_cell_payload(decoded.payload);
    CHECK(cells.size() == 64);
    CHECK(cells[3 * 8 + 2] == 9U);
}

void test_commit_is_the_cli_write_path()
{
    // Drive the SAME edit through (a) the GUI gesture commit and (b) the core directly (what the
    // CLI verb runs), on two identical stores: the resulting owner + sidecar bytes must be
    // BYTE-IDENTICAL — R-CLI-001 parity by construction, asserted.
    filesync::MemoryFileStore fs_gui;
    TilemapPaintModel model;
    CHECK(tilemappaneltest::stage_and_open(fs_gui, model));
    const std::optional<std::string> owner_before = fs_gui.read(kOwnerPath);
    CHECK(owner_before.has_value());
    const std::uint64_t hash_before = filesync::content_hash(*owner_before);

    CHECK(model.select_tile(4));
    model.set_tool(Tool::fill);
    model.begin_gesture_at_cell(1, 1);
    model.extend_gesture_to_cell(2, 2);
    const CommitOutcome outcome = model.commit_gesture(fs_gui, ".");
    CHECK(outcome.ok);
    CHECK(outcome.cells_changed == 4);

    filesync::MemoryFileStore fs_cli;
    TilemapPaintModel probe; // only to stage the identical fixture
    CHECK(tilemappaneltest::stage_and_open(fs_cli, probe));
    const serializer::CanonicalizeResult canonical =
        serializer::canonicalize(*fs_cli.read(kOwnerPath));
    const tme::EditOutcome edit = tme::apply_cell_edits(
        fs_cli, ".", kOwnerPath, canonical.root, kGroundLayerId,
        tme::expand_fill_rect(1, 1, 2, 2, 4U));
    CHECK(edit.ok);
    CHECK(tme::commit_edit(fs_cli, ".", kOwnerPath, edit).ok);

    CHECK(fs_gui.read(kOwnerPath) == fs_cli.read(kOwnerPath));
    CHECK(fs_gui.read("tilemaps/map.ground.cells.bin") ==
          fs_cli.read("tilemaps/map.ground.cells.bin"));
    CHECK(fs_gui.read("tilemaps/map.props.cells.bin") ==
          fs_cli.read("tilemaps/map.props.cells.bin")); // the healed dangling sibling too

    // The committed diff is canonical + hot-reload-coherent: canonical fixpoint, raw hash moved
    // (the L-22 watch trigger), every "$sidecar" ref verifies against disk.
    const std::optional<std::string> owner_after = fs_gui.read(kOwnerPath);
    CHECK(owner_after.has_value());
    const serializer::CanonicalizeResult recanon = serializer::canonicalize(*owner_after);
    CHECK(recanon.is_json);
    CHECK(recanon.bytes == *owner_after);
    CHECK(filesync::content_hash(*owner_after) != hash_before);
    const filesync::SidecarScan scan = filesync::scan_sidecar_refs(".", kOwnerPath, *owner_after);
    CHECK(scan.diagnostics.empty());
    CHECK(filesync::verify_sidecar_refs(fs_gui, kOwnerPath, scan.refs).empty());

    // An empty gesture commits as a successful no-op; a failing batch surfaces the core's code.
    const CommitOutcome noop = model.commit_gesture(fs_gui, ".");
    CHECK(noop.ok);
    CHECK(noop.cells_changed == 0);
    model.set_tool(Tool::paint);
    model.begin_gesture_at_cell(99, 99); // outside every chunk region
    const CommitOutcome refused = model.commit_gesture(fs_gui, ".");
    CHECK(!refused.ok);
    CHECK(refused.error_code == tme::kTilemapCellOutOfBoundsCode);
}

} // namespace

int main()
{
    test_viewport_mapping();
    test_open_and_palette();
    test_gesture_lifecycle();
    test_keyboard_cursor_path();
    test_commit_is_the_cli_write_path();
    TILEMAP_PANEL_TEST_MAIN_END();
}
