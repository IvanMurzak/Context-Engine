// Tilemap authoring write-path core (see tilemap_edit.h) — the ONE cell-edit path GUI and CLI share.

#include "context/editor/tilemap/tilemap_edit.h"

#include "context/editor/filesync/atomic_io.h"
#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/sidecar.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/sidecar_ref.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace context::editor::tilemap
{

namespace
{

namespace fsync = context::editor::filesync;
namespace ser = context::editor::serializer;

// Tiny local tree accessors (the kinds-module json_access pattern — that header is module-internal,
// so these are deliberately re-declared here rather than reached across layers).
[[nodiscard]] const ser::JsonValue* member(const ser::JsonValue& object, std::string_view key)
{
    if (object.type != ser::JsonValue::Type::object)
        return nullptr;
    for (const ser::JsonMember& m : object.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

[[nodiscard]] ser::JsonValue* member_mut(ser::JsonValue& object, std::string_view key)
{
    if (object.type != ser::JsonValue::Type::object)
        return nullptr;
    for (ser::JsonMember& m : object.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

// The integer value of a JSON node that parsed as an integer literal (i64 or u64 range); nullopt for
// anything else. Chunk regions are i32x4 per the schema, so i64 always holds them losslessly.
[[nodiscard]] std::optional<std::int64_t> as_int(const ser::JsonValue& v)
{
    if (v.type == ser::JsonValue::Type::integer)
        return v.int_value;
    if (v.type == ser::JsonValue::Type::unsigned_integer &&
        v.uint_value <= static_cast<std::uint64_t>(INT64_MAX))
        return static_cast<std::int64_t>(v.uint_value);
    return std::nullopt;
}

struct Region
{
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t width = 0;
    std::int64_t height = 0;

    [[nodiscard]] bool contains(std::int64_t cx, std::int64_t cy) const noexcept
    {
        return cx >= x && cy >= y && cx < x + width && cy < y + height;
    }
};

// Parse a chunk's "region" [x, y, width, height]; nullopt when malformed or non-positive extent
// (schema-invalid / tilemap.region_invalid territory — this authoring pass edits only well-formed
// chunks; a malformed one simply cannot be painted into, surfacing as cell_out_of_bounds).
[[nodiscard]] std::optional<Region> parse_region(const ser::JsonValue& chunk)
{
    const ser::JsonValue* region = member(chunk, "region");
    if (region == nullptr || region->type != ser::JsonValue::Type::array ||
        region->elements.size() != 4)
        return std::nullopt;
    std::int64_t parts[4] = {0, 0, 0, 0};
    for (std::size_t i = 0; i < 4; ++i)
    {
        const std::optional<std::int64_t> v = as_int(region->elements[i]);
        if (!v)
            return std::nullopt;
        parts[i] = *v;
    }
    Region r{parts[0], parts[1], parts[2], parts[3]};
    if (r.width <= 0 || r.height <= 0)
        return std::nullopt;
    return r;
}

// Is `tile` a known GLOBAL tile id: 0 (empty) or inside some tile-set's [firstTileId, firstTileId +
// tileCount) range? A set without tileCount is treated as open-ended from firstTileId (the schema
// marks tileCount optional).
[[nodiscard]] bool tile_known(const ser::JsonValue& doc, std::uint32_t tile)
{
    if (tile == 0)
        return true;
    const ser::JsonValue* sets = member(doc, "tileSets");
    if (sets == nullptr || sets->type != ser::JsonValue::Type::array)
        return false;
    for (const ser::JsonValue& set : sets->elements)
    {
        const ser::JsonValue* first = member(set, "firstTileId");
        const std::optional<std::int64_t> first_id = first ? as_int(*first) : std::nullopt;
        if (!first_id || *first_id < 0)
            continue;
        const auto lo = static_cast<std::uint64_t>(*first_id);
        std::uint64_t hi = UINT64_MAX; // open-ended when tileCount is absent
        if (const ser::JsonValue* count = member(set, "tileCount"))
        {
            const std::optional<std::int64_t> n = as_int(*count);
            if (n && *n >= 0)
                hi = lo + static_cast<std::uint64_t>(*n);
        }
        if (tile >= lo && tile < hi)
            return true;
    }
    return false;
}

} // namespace

std::string encode_cell_payload(const std::vector<std::uint32_t>& cells)
{
    std::string out;
    out.reserve(cells.size() * 4);
    for (const std::uint32_t c : cells)
    {
        out.push_back(static_cast<char>(c & 0xffU));
        out.push_back(static_cast<char>((c >> 8U) & 0xffU));
        out.push_back(static_cast<char>((c >> 16U) & 0xffU));
        out.push_back(static_cast<char>((c >> 24U) & 0xffU));
    }
    return out;
}

std::vector<std::uint32_t> decode_cell_payload(std::string_view payload)
{
    std::vector<std::uint32_t> cells;
    cells.reserve(payload.size() / 4);
    for (std::size_t i = 0; i + 4 <= payload.size(); i += 4)
    {
        const auto b0 = static_cast<std::uint32_t>(static_cast<unsigned char>(payload[i]));
        const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(payload[i + 1]));
        const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(payload[i + 2]));
        const auto b3 = static_cast<std::uint32_t>(static_cast<unsigned char>(payload[i + 3]));
        cells.push_back(b0 | (b1 << 8U) | (b2 << 16U) | (b3 << 24U));
    }
    return cells;
}

std::vector<CellEdit> expand_fill_rect(std::int64_t x, std::int64_t y, std::int64_t width,
                                       std::int64_t height, std::uint32_t tile)
{
    std::vector<CellEdit> edits;
    if (width <= 0 || height <= 0)
        return edits;
    edits.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (std::int64_t cy = y; cy < y + height; ++cy)
        for (std::int64_t cx = x; cx < x + width; ++cx)
            edits.push_back(CellEdit{cx, cy, tile});
    return edits;
}

EditOutcome apply_cell_edits(const filesync::FileStore& fs, std::string_view root,
                             std::string_view owner_path, const serializer::JsonValue& doc,
                             std::string_view layer_id, const std::vector<CellEdit>& edits)
{
    EditOutcome out;

    if (doc.type != ser::JsonValue::Type::object)
    {
        out.error_code = "file.parse_error";
        out.error_message = "the tilemap document is not a JSON object";
        return out;
    }

    // Locate the addressed layer (a mutable copy of the whole doc is made only after validation).
    const ser::JsonValue* layers = member(doc, "layers");
    const ser::JsonValue* layer = nullptr;
    std::size_t layer_index = 0;
    if (layers != nullptr && layers->type == ser::JsonValue::Type::array)
    {
        for (std::size_t i = 0; i < layers->elements.size(); ++i)
        {
            const ser::JsonValue* id = member(layers->elements[i], "id");
            if (id != nullptr && id->type == ser::JsonValue::Type::string &&
                id->string_value == layer_id)
            {
                layer = &layers->elements[i];
                layer_index = i;
                break;
            }
        }
    }
    if (layer == nullptr)
    {
        out.error_code = kTilemapLayerNotFoundCode;
        out.error_message =
            "no layer with id `" + std::string(layer_id) + "` exists in the tilemap";
        return out;
    }

    // Collect the layer's paintable chunks (well-formed region + well-formed "$sidecar" cells ref).
    struct ChunkSlot
    {
        std::size_t chunk_index = 0;
        Region region;
        std::string relpath;       // the authored "$sidecar" relpath
        std::string resolved_path; // root-relative logical path (jail-checked)
        std::vector<std::uint32_t> cells;
        bool dirty = false;
    };
    std::vector<ChunkSlot> slots;
    const ser::JsonValue* chunks = member(*layer, "chunks");
    if (chunks != nullptr && chunks->type == ser::JsonValue::Type::array)
    {
        for (std::size_t i = 0; i < chunks->elements.size(); ++i)
        {
            const ser::JsonValue& chunk = chunks->elements[i];
            const std::optional<Region> region = parse_region(chunk);
            if (!region)
                continue;
            const ser::JsonValue* cells = member(chunk, "cells");
            if (cells == nullptr || !ser::is_sidecar_ref(*cells))
                continue;
            const ser::JsonValue* rel = member(*cells, "$sidecar");
            const std::optional<std::string> resolved =
                fsync::resolve_sidecar_path(root, owner_path, rel->string_value);
            if (!resolved)
                continue; // an escaping ref is not paintable (R-SEC-008)
            ChunkSlot slot;
            slot.chunk_index = i;
            slot.region = *region;
            slot.relpath = rel->string_value;
            slot.resolved_path = *resolved;
            slots.push_back(std::move(slot));
        }
    }

    // Validate the whole batch BEFORE mutating anything (all-or-nothing: a gesture either commits
    // whole or refuses whole — no partially-painted family is ever staged).
    for (const CellEdit& e : edits)
    {
        if (!tile_known(doc, e.tile))
        {
            out.error_code = kTilemapTileUnknownCode;
            out.error_message = "tile id " + std::to_string(e.tile) +
                                " falls in no tile-set's [firstTileId, firstTileId + tileCount) "
                                "range (0 = empty)";
            return out;
        }
        bool covered = false;
        for (const ChunkSlot& slot : slots)
            if (slot.region.contains(e.x, e.y))
            {
                covered = true;
                break;
            }
        if (!covered)
        {
            out.error_code = kTilemapCellOutOfBoundsCode;
            out.error_message = "cell (" + std::to_string(e.x) + ", " + std::to_string(e.y) +
                                ") lies inside no chunk region of layer `" + std::string(layer_id) +
                                "`";
            return out;
        }
    }

    // Load each touched chunk's current payload lazily; absent/unreadable/mis-sized payloads start
    // from an all-empty grid (painting HEALS an incoherent family by rewriting it whole).
    auto load_cells = [&fs](ChunkSlot& slot) {
        if (!slot.cells.empty())
            return;
        const std::size_t expected = static_cast<std::size_t>(slot.region.width) *
                                     static_cast<std::size_t>(slot.region.height);
        slot.cells.assign(expected, 0U);
        const std::optional<std::string> bytes = fs.read(slot.resolved_path);
        if (!bytes)
            return;
        const fsync::SidecarDecodeResult decoded = fsync::decode_sidecar(*bytes);
        if (!decoded.ok)
            return;
        const std::vector<std::uint32_t> current = decode_cell_payload(decoded.payload);
        if (current.size() == expected)
            slot.cells = current;
    };

    // Apply the batch in order (later edits to the same cell win — the last stroke is what the user
    // sees). An edit inside several overlapping chunk regions writes them ALL (they shadow the same
    // cell; leaving a stale copy would make the visible value depend on draw order).
    for (const CellEdit& e : edits)
    {
        for (ChunkSlot& slot : slots)
        {
            if (!slot.region.contains(e.x, e.y))
                continue;
            load_cells(slot);
            const std::size_t index =
                static_cast<std::size_t>(e.y - slot.region.y) *
                    static_cast<std::size_t>(slot.region.width) +
                static_cast<std::size_t>(e.x - slot.region.x);
            if (slot.cells[index] != e.tile)
            {
                slot.cells[index] = e.tile;
                slot.dirty = true;
                ++out.cells_changed;
            }
        }
    }

    // Re-encode every dirty chunk sidecar + refresh its owner-side "hash" member on a mutable copy.
    ser::JsonValue mutated = doc;
    ser::JsonValue* m_layers = member_mut(mutated, "layers");
    ser::JsonValue* m_layer =
        (m_layers != nullptr && layer_index < m_layers->elements.size())
            ? &m_layers->elements[layer_index]
            : nullptr;
    ser::JsonValue* m_chunks = m_layer != nullptr ? member_mut(*m_layer, "chunks") : nullptr;
    if (m_chunks == nullptr)
    {
        out.error_code = "internal.error";
        out.error_message = "the tilemap layer lost its chunks while staging the edit";
        return out;
    }
    for (ChunkSlot& slot : slots)
    {
        if (!slot.dirty)
            continue;
        StagedWrite write;
        write.path = slot.resolved_path;
        write.bytes = fsync::encode_sidecar(encode_cell_payload(slot.cells));
        write.raw_hash = fsync::content_hash(write.bytes);

        ser::JsonValue* m_chunk = &m_chunks->elements[slot.chunk_index];
        ser::JsonValue* m_cells = member_mut(*m_chunk, "cells");
        ser::JsonValue* m_hash = m_cells != nullptr ? member_mut(*m_cells, "hash") : nullptr;
        if (m_hash == nullptr)
        {
            out.error_code = "internal.error";
            out.error_message = "the chunk's $sidecar ref lost its hash member while staging";
            return out;
        }
        m_hash->type = ser::JsonValue::Type::string;
        m_hash->string_value = fsync::format_sidecar_hash(write.raw_hash);
        out.sidecars.push_back(std::move(write));
    }

    // HEAL pass (R-FILE-003 posture): the L-33 family planner refuses to author a family with a
    // DANGLING ref, and the maintained corpus legitimately ships tilemaps whose cell sidecars are
    // not on disk yet (external files may be temporarily inconsistent). So a paint commit stages an
    // all-empty payload for EVERY absent chunk sidecar across the whole document (all layers) and
    // refreshes those hash members too — the first paint leaves the family fully coherent.
    if (ser::JsonValue* all_layers = member_mut(mutated, "layers");
        all_layers != nullptr && all_layers->type == ser::JsonValue::Type::array)
    {
        for (ser::JsonValue& l : all_layers->elements)
        {
            ser::JsonValue* l_chunks = member_mut(l, "chunks");
            if (l_chunks == nullptr || l_chunks->type != ser::JsonValue::Type::array)
                continue;
            for (ser::JsonValue& chunk : l_chunks->elements)
            {
                ser::JsonValue* cells = member_mut(chunk, "cells");
                if (cells == nullptr || !ser::is_sidecar_ref(*cells))
                    continue;
                const ser::JsonValue* rel = member(*cells, "$sidecar");
                const std::optional<std::string> resolved =
                    fsync::resolve_sidecar_path(root, owner_path, rel->string_value);
                if (!resolved)
                    continue;
                bool already_staged = false;
                for (const StagedWrite& w : out.sidecars)
                    if (w.path == *resolved)
                    {
                        already_staged = true;
                        break;
                    }
                if (already_staged || fs.read(*resolved))
                    continue;
                const std::optional<Region> region = parse_region(chunk);
                const std::size_t cell_count =
                    region ? static_cast<std::size_t>(region->width) *
                                 static_cast<std::size_t>(region->height)
                           : std::size_t{0};
                StagedWrite write;
                write.path = *resolved;
                write.bytes = fsync::encode_sidecar(
                    encode_cell_payload(std::vector<std::uint32_t>(cell_count, 0U)));
                write.raw_hash = fsync::content_hash(write.bytes);
                ser::JsonValue* hash = member_mut(*cells, "hash");
                if (hash == nullptr)
                    continue;
                hash->type = ser::JsonValue::Type::string;
                hash->string_value = fsync::format_sidecar_hash(write.raw_hash);
                out.sidecars.push_back(std::move(write));
            }
        }
    }

    if (!ser::serialize_canonical(mutated, out.owner_bytes))
    {
        out.error_code = "internal.error";
        out.error_message = "the mutated tilemap document could not be canonically serialized";
        out.owner_bytes.clear();
        out.sidecars.clear();
        return out;
    }
    out.ok = true;
    return out;
}

CommitResult commit_edit(filesync::FileStore& fs, std::string_view root,
                         std::string_view owner_path, const EditOutcome& edit)
{
    CommitResult result;
    if (!edit.ok)
    {
        result.error_code = edit.error_code.empty() ? "usage.invalid" : edit.error_code;
        result.error_message = edit.error_message;
        return result;
    }

    std::vector<fsync::StagedSidecar> staged;
    staged.reserve(edit.sidecars.size());
    for (const StagedWrite& w : edit.sidecars)
        staged.push_back(fsync::StagedSidecar{w.path, w.bytes});

    // The L-33 family planner: validates coherence (every staged sidecar referenced with a matching
    // hash, every ref resolvable) and yields the dependency-safe order — sidecars BEFORE the owner.
    const fsync::SidecarPlan plan =
        fsync::plan_sidecar_family_write(fs, root, owner_path, edit.owner_bytes, staged);
    if (!plan.ok)
    {
        result.error_code =
            plan.diagnostics.empty() ? "internal.error" : plan.diagnostics.front().code;
        result.error_message = plan.diagnostics.empty()
                                   ? "the sidecar family write plan was refused"
                                   : plan.diagnostics.front().message;
        return result;
    }
    for (const fsync::PlannedWrite& step : plan.steps)
    {
        if (step.kind != fsync::WriteKind::write)
            continue; // a cell edit only writes; it never removes family members
        if (!fsync::atomic_write(fs, step.path, step.data))
        {
            result.error_code = "internal.error";
            result.error_message = "the atomic write to `" + step.path + "` failed";
            return result;
        }
    }

    result.ok = true;
    result.owner_raw_hash = fsync::content_hash(edit.owner_bytes);
    result.owner_canonical_hash = ser::canonical_hash_of(edit.owner_bytes);
    return result;
}

} // namespace context::editor::tilemap
