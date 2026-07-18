// Tile-painting model implementation (see tilemap_paint_model.h).

#include "context/editor/gui/panels/tilemap/tilemap_paint_model.h"

#include "context/editor/serializer/canonical.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace context::editor::gui::panels::tilemap
{

namespace
{

namespace ser = context::editor::serializer;

[[nodiscard]] const ser::JsonValue* member(const ser::JsonValue& object, std::string_view key)
{
    if (object.type != ser::JsonValue::Type::object)
        return nullptr;
    for (const ser::JsonMember& m : object.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

[[nodiscard]] std::optional<std::int64_t> as_int(const ser::JsonValue& v)
{
    if (v.type == ser::JsonValue::Type::integer)
        return v.int_value;
    if (v.type == ser::JsonValue::Type::unsigned_integer &&
        v.uint_value <= static_cast<std::uint64_t>(INT64_MAX))
        return static_cast<std::int64_t>(v.uint_value);
    return std::nullopt;
}

[[nodiscard]] double as_number(const ser::JsonValue& v, double fallback)
{
    switch (v.type)
    {
    case ser::JsonValue::Type::integer:
        return static_cast<double>(v.int_value);
    case ser::JsonValue::Type::unsigned_integer:
        return static_cast<double>(v.uint_value);
    case ser::JsonValue::Type::number:
        return v.number_value;
    default:
        return fallback;
    }
}

} // namespace

const char* tool_name(Tool tool)
{
    switch (tool)
    {
    case Tool::paint:
        return "paint";
    case Tool::erase:
        return "erase";
    case Tool::fill:
        return "fill";
    }
    return "paint";
}

// --- Ortho2DViewport -----------------------------------------------------------------------------

double Ortho2DViewport::world_x_at(double px) const noexcept
{
    return center_x + (px - static_cast<double>(width_px) / 2.0) / zoom;
}

double Ortho2DViewport::world_y_at(double py) const noexcept
{
    return center_y + (py - static_cast<double>(height_px) / 2.0) / zoom;
}

std::int64_t Ortho2DViewport::cell_x_at(double px) const noexcept
{
    return static_cast<std::int64_t>(std::floor(world_x_at(px) / tile_width));
}

std::int64_t Ortho2DViewport::cell_y_at(double py) const noexcept
{
    return static_cast<std::int64_t>(std::floor(world_y_at(py) / tile_height));
}

void Ortho2DViewport::center_on_cell(std::int64_t cx, std::int64_t cy) noexcept
{
    center_x = (static_cast<double>(cx) + 0.5) * tile_width;
    center_y = (static_cast<double>(cy) + 0.5) * tile_height;
}

void Ortho2DViewport::pan(double dx_m, double dy_m) noexcept
{
    center_x += dx_m;
    center_y += dy_m;
}

void Ortho2DViewport::zoom_in() noexcept
{
    zoom = std::min(zoom * kZoomStep, kMaxZoom);
}

void Ortho2DViewport::zoom_out() noexcept
{
    zoom = std::max(zoom / kZoomStep, kMinZoom);
}

// --- TilemapPaintModel ---------------------------------------------------------------------------

bool TilemapPaintModel::open(const filesync::FileStore& fs, std::string_view root,
                             std::string_view owner_path)
{
    (void)root; // the FileStore is already rooted; paths are root-relative logical paths
    const std::optional<std::string> bytes = fs.read(owner_path);
    if (!bytes)
        return false;
    ser::CanonicalizeResult canonical = ser::canonicalize(*bytes);
    if (!canonical.is_json)
        return false;

    loaded_ = true;
    owner_path_ = std::string(owner_path);
    doc_ = std::move(canonical.root);
    tile_sets_.clear();
    layers_.clear();

    if (const ser::JsonValue* size = member(doc_, "tileSize");
        size != nullptr && size->type == ser::JsonValue::Type::array && size->elements.size() == 2)
    {
        const double w = as_number(size->elements[0], 1.0);
        const double h = as_number(size->elements[1], 1.0);
        if (w > 0.0)
            viewport_.tile_width = w;
        if (h > 0.0)
            viewport_.tile_height = h;
    }

    if (const ser::JsonValue* sets = member(doc_, "tileSets");
        sets != nullptr && sets->type == ser::JsonValue::Type::array)
    {
        for (const ser::JsonValue& set : sets->elements)
        {
            const ser::JsonValue* id = member(set, "id");
            const ser::JsonValue* first = member(set, "firstTileId");
            if (id == nullptr || id->type != ser::JsonValue::Type::string || first == nullptr)
                continue;
            const std::optional<std::int64_t> first_id = as_int(*first);
            if (!first_id || *first_id <= 0)
                continue;
            PaletteTileSet entry;
            entry.id = id->string_value;
            entry.first_tile = static_cast<std::uint32_t>(*first_id);
            if (const ser::JsonValue* name = member(set, "name");
                name != nullptr && name->type == ser::JsonValue::Type::string)
                entry.name = name->string_value;
            if (entry.name.empty())
                entry.name = entry.id;
            if (const ser::JsonValue* count = member(set, "tileCount"))
                if (const std::optional<std::int64_t> n = as_int(*count); n && *n > 0)
                    entry.tile_count = static_cast<std::uint32_t>(*n);
            tile_sets_.push_back(std::move(entry));
        }
    }

    if (const ser::JsonValue* layers = member(doc_, "layers");
        layers != nullptr && layers->type == ser::JsonValue::Type::array)
    {
        for (const ser::JsonValue& layer : layers->elements)
        {
            const ser::JsonValue* id = member(layer, "id");
            if (id == nullptr || id->type != ser::JsonValue::Type::string)
                continue;
            LayerEntry entry;
            entry.id = id->string_value;
            if (const ser::JsonValue* name = member(layer, "name");
                name != nullptr && name->type == ser::JsonValue::Type::string)
                entry.name = name->string_value;
            if (entry.name.empty())
                entry.name = entry.id;
            if (const ser::JsonValue* visible = member(layer, "visible");
                visible != nullptr && visible->type == ser::JsonValue::Type::boolean)
                entry.visible = visible->boolean_value;
            layers_.push_back(std::move(entry));
        }
    }

    // Default selection: the first layer, the first tile-set's first tile.
    if (active_layer_id_.empty() || !select_layer(active_layer_id_))
        active_layer_id_ = layers_.empty() ? std::string() : layers_.front().id;
    if (selected_tile_ == 0 && !tile_sets_.empty())
        selected_tile_ = tile_sets_.front().first_tile;

    cancel_gesture();
    return true;
}

bool TilemapPaintModel::select_layer(std::string_view layer_id)
{
    for (const LayerEntry& l : layers_)
        if (l.id == layer_id)
        {
            active_layer_id_ = l.id;
            return true;
        }
    return false;
}

bool TilemapPaintModel::select_tile(std::uint32_t tile)
{
    if (tile == 0)
    {
        selected_tile_ = 0;
        return true;
    }
    for (const PaletteTileSet& set : tile_sets_)
    {
        const std::uint64_t lo = set.first_tile;
        const std::uint64_t hi =
            set.tile_count == 0 ? UINT64_MAX : lo + static_cast<std::uint64_t>(set.tile_count);
        if (tile >= lo && tile < hi)
        {
            selected_tile_ = tile;
            return true;
        }
    }
    return false;
}

void TilemapPaintModel::select_next_tile()
{
    if (tile_sets_.empty())
        return;
    if (select_tile(selected_tile_ + 1))
        return;
    // Jump to the first tile of the NEXT set holding ids above the selection; wrap to the first.
    const PaletteTileSet* best = nullptr;
    for (const PaletteTileSet& set : tile_sets_)
        if (set.first_tile > selected_tile_ &&
            (best == nullptr || set.first_tile < best->first_tile))
            best = &set;
    selected_tile_ = best != nullptr ? best->first_tile : tile_sets_.front().first_tile;
}

void TilemapPaintModel::select_prev_tile()
{
    if (tile_sets_.empty())
        return;
    if (selected_tile_ > 1 && select_tile(selected_tile_ - 1))
        return;
    // Jump to the last tile of the PREVIOUS set below the selection; wrap to the highest known.
    const PaletteTileSet* best = nullptr;
    for (const PaletteTileSet& set : tile_sets_)
    {
        if (set.tile_count == 0)
            continue; // an open-ended set has no last tile to wrap to
        if (set.first_tile + set.tile_count <= selected_tile_ &&
            (best == nullptr || set.first_tile > best->first_tile))
            best = &set;
    }
    if (best == nullptr)
        for (const PaletteTileSet& set : tile_sets_)
            if (set.tile_count != 0 && (best == nullptr || set.first_tile > best->first_tile))
                best = &set;
    if (best != nullptr)
        selected_tile_ = best->first_tile + best->tile_count - 1;
}

void TilemapPaintModel::move_cursor(std::int64_t dx, std::int64_t dy)
{
    cursor_x_ += dx;
    cursor_y_ += dy;
    viewport_.center_on_cell(cursor_x_, cursor_y_); // the camera follows the keyboard cursor
    if (gesture_active_)
        extend_gesture_to_cell(cursor_x_, cursor_y_);
}

std::size_t TilemapPaintModel::pending_cells() const noexcept
{
    if (!gesture_active_)
        return 0;
    if (tool_ == Tool::fill)
    {
        const std::int64_t w = (last_x_ > anchor_x_ ? last_x_ - anchor_x_ : anchor_x_ - last_x_) + 1;
        const std::int64_t h = (last_y_ > anchor_y_ ? last_y_ - anchor_y_ : anchor_y_ - last_y_) + 1;
        return static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    }
    return visited_.size();
}

void TilemapPaintModel::begin_gesture_at_cell(std::int64_t cx, std::int64_t cy)
{
    gesture_active_ = true;
    anchor_x_ = last_x_ = cx;
    anchor_y_ = last_y_ = cy;
    visited_.clear();
    visit_cell(cx, cy);
}

void TilemapPaintModel::begin_gesture_at_pixel(double px, double py)
{
    begin_gesture_at_cell(viewport_.cell_x_at(px), viewport_.cell_y_at(py));
}

void TilemapPaintModel::extend_gesture_to_cell(std::int64_t cx, std::int64_t cy)
{
    if (!gesture_active_)
        return;
    last_x_ = cx;
    last_y_ = cy;
    visit_cell(cx, cy);
}

void TilemapPaintModel::extend_gesture_to_pixel(double px, double py)
{
    extend_gesture_to_cell(viewport_.cell_x_at(px), viewport_.cell_y_at(py));
}

void TilemapPaintModel::cancel_gesture()
{
    gesture_active_ = false;
    visited_.clear();
}

std::vector<tme::CellEdit> TilemapPaintModel::end_gesture()
{
    std::vector<tme::CellEdit> batch;
    if (!gesture_active_)
        return batch;
    if (tool_ == Tool::fill)
    {
        const std::int64_t x0 = std::min(anchor_x_, last_x_);
        const std::int64_t y0 = std::min(anchor_y_, last_y_);
        const std::int64_t x1 = std::max(anchor_x_, last_x_);
        const std::int64_t y1 = std::max(anchor_y_, last_y_);
        batch = tme::expand_fill_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, gesture_tile());
    }
    else
    {
        batch = visited_;
    }
    cancel_gesture();
    return batch;
}

CommitOutcome TilemapPaintModel::commit_gesture(filesync::FileStore& fs, std::string_view root)
{
    CommitOutcome outcome;
    const std::vector<tme::CellEdit> batch = end_gesture();
    if (!loaded_)
    {
        outcome.error_code = "file.not_found";
        outcome.error_message = "no tilemap document is open";
        return outcome;
    }
    if (batch.empty())
    {
        outcome.ok = true; // an empty gesture is a successful no-op (nothing to write)
        return outcome;
    }

    // The ONE shared write path (R-CLI-001): exactly what `context tilemap paint|fill` runs.
    const tme::EditOutcome edit =
        tme::apply_cell_edits(fs, root, owner_path_, doc_, active_layer_id_, batch);
    if (!edit.ok)
    {
        outcome.error_code = edit.error_code;
        outcome.error_message = edit.error_message;
        return outcome;
    }
    const tme::CommitResult committed = tme::commit_edit(fs, root, owner_path_, edit);
    if (!committed.ok)
    {
        outcome.error_code = committed.error_code;
        outcome.error_message = committed.error_message;
        return outcome;
    }
    outcome.ok = true;
    outcome.cells_changed = edit.cells_changed;

    // Reload so the session model reflects the newly-authored state (L-20: files stay the truth).
    const std::string reopened = owner_path_;
    (void)open(fs, root, reopened);
    return outcome;
}

CommitOutcome TilemapPaintModel::apply_at_cursor(filesync::FileStore& fs, std::string_view root)
{
    begin_gesture_at_cell(cursor_x_, cursor_y_);
    return commit_gesture(fs, root);
}

std::string TilemapPaintModel::status_text() const
{
    std::string layer_name = "(none)";
    for (const LayerEntry& l : layers_)
        if (l.id == active_layer_id_)
        {
            layer_name = l.name;
            break;
        }
    std::string text = "Tool: ";
    text += tool_name(tool_);
    text += " | Layer: " + layer_name;
    text += " | Tile: " + std::to_string(selected_tile_);
    text += " | Cursor: (" + std::to_string(cursor_x_) + ", " + std::to_string(cursor_y_) + ")";
    text += " | Pending: " + std::to_string(pending_cells());
    return text;
}

void TilemapPaintModel::visit_cell(std::int64_t cx, std::int64_t cy)
{
    if (tool_ == Tool::fill)
        return; // fill tracks only anchor + last; the rect expands at gesture end
    const std::uint32_t tile = gesture_tile();
    for (tme::CellEdit& e : visited_)
        if (e.x == cx && e.y == cy)
        {
            e.tile = tile; // re-visiting a cell keeps ONE edit, holding the latest stroke
            return;
        }
    visited_.push_back(tme::CellEdit{cx, cy, tile});
}

std::uint32_t TilemapPaintModel::gesture_tile() const noexcept
{
    return tool_ == Tool::erase ? 0U : selected_tile_;
}

} // namespace context::editor::gui::panels::tilemap
