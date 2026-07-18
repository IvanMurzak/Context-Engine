// The tilemap authoring WRITE path (M8.5 a18, R-2D-003 GUI half / R-CLI-001 / L-20 / L-33): the ONE
// cell-edit core BOTH the tile-painting GUI (gesture-end commit) and the `context tilemap paint|fill`
// CLI verbs call, so GUI == CLI parity holds BY CONSTRUCTION — the GUI is sugar over this same path.
// The M2 ctx:tilemap kind + sidecar rules are NOT changed here (L-20 note in the task): this module
// only REWRITES existing chunks' packed cell payloads (u32 little-endian, row-major within the chunk
// region — the "width*height u32 values" the ctx:tilemap schema pins) and refreshes the owner's
// "$sidecar" hash members, emitting a canonical owner rewrite plus its re-encoded sidecars.

#pragma once

#include "context/editor/filesync/file_store.h"
#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::tilemap
{

// The a18 `tilemap.*` authoring error-domain block. Owned HERE as string constants (the
// promote-a-local-string pattern of viewport::kViewport*Code) so this module does not link the
// contract catalog; src/editor/contract/src/error_catalog.cpp registers the SAME strings
// (append-only tail) and the catalog test asserts them.
inline constexpr const char* kTilemapLayerNotFoundCode = "tilemap.layer_not_found";
inline constexpr const char* kTilemapCellOutOfBoundsCode = "tilemap.cell_out_of_bounds";
inline constexpr const char* kTilemapTileUnknownCode = "tilemap.tile_unknown";

// One cell edit in CELL coordinates. `tile` is a GLOBAL tile id spanning every tile-set (the
// ctx:tilemap firstTileId offsets); tile 0 means EMPTY — an erase is a paint with tile 0, so the
// erase tool needs no second path.
struct CellEdit
{
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::uint32_t tile = 0;
};

// --- the packed cell-payload codec (the schema's "width*height u32 values") ----------------------
//
// Byte order is pinned LITTLE-ENDIAN (like the sidecar header itself) so sidecar bytes are identical
// across platforms and the whole-file raw-byte hash agrees everywhere (R-FILE-001). Cells are
// row-major within the chunk region: index = (y - region.y) * region.width + (x - region.x).

[[nodiscard]] std::string encode_cell_payload(const std::vector<std::uint32_t>& cells);
[[nodiscard]] std::vector<std::uint32_t> decode_cell_payload(std::string_view payload);

// Expand a rect fill into its per-cell edits (row-major). The GUI fill tool and `context tilemap
// fill` both expand through THIS function and then run the same apply_cell_edits path, so a fill is
// exactly a batch paint — parity by construction. Non-positive extents yield an empty batch.
[[nodiscard]] std::vector<CellEdit> expand_fill_rect(std::int64_t x, std::int64_t y,
                                                     std::int64_t width, std::int64_t height,
                                                     std::uint32_t tile);

// One staged file write of a committed edit (full on-disk bytes; sidecars include the 12-byte
// versioned header — filesync::encode_sidecar output).
struct StagedWrite
{
    std::string path; // project-root-relative logical path
    std::string bytes;
    std::uint64_t raw_hash = 0; // whole-file raw-byte hash of `bytes`
};

// The outcome of applying a cell-edit batch to a parsed ctx:tilemap document. On ok:
//   * owner_bytes — the CANONICAL serialization of the mutated owner (its chunk "$sidecar" hash
//     members refreshed), a canonical fixpoint by construction;
//   * sidecars   — every chunk sidecar whose payload changed, fully re-encoded;
//   * cells_changed — cells whose stored value actually changed (an edit writing the value already
//     present is a no-op and does not count).
// On failure `error_code` is one of the kTilemap*Code constants above (plus the message naming the
// offending layer/cell/tile). Failure is all-or-nothing: a batch with ANY out-of-bounds cell or
// unknown tile refuses whole (no partial paint is ever staged — the gesture either commits or not).
struct EditOutcome
{
    bool ok = false;
    std::string error_code;
    std::string error_message;
    std::string owner_bytes;
    std::vector<StagedWrite> sidecars;
    std::size_t cells_changed = 0;
};

// Apply `edits` to layer `layer_id` of the parsed canonical tilemap `doc` (the owner document at
// `owner_path` under `root` on `fs` — read for the chunks' CURRENT sidecar payloads). Rules:
//   * the layer must exist (tilemap.layer_not_found);
//   * every edit must land inside some chunk `region` of that layer (tilemap.cell_out_of_bounds —
//     v1 authoring rewrites EXISTING chunks; it never invents new chunks, which would be a format-
//     shaping decision the M2 kind owns);
//   * every tile must be 0 (empty) or fall inside some tile-set's [firstTileId, firstTileId +
//     tileCount) global range (tilemap.tile_unknown);
//   * a chunk whose sidecar is absent/unreadable/mis-sized starts from an all-empty grid (painting
//     HEALS a dangling ref by writing a coherent family — the daemon's own write path must leave
//     the family consistent, R-FILE-003 posture);
//   * later edits to the same cell win (the gesture semantics: the last stroke over a cell is what
//     the user sees).
// Purely functional over (doc, fs): stages bytes, writes NOTHING to disk — commit_edit executes.
[[nodiscard]] EditOutcome apply_cell_edits(const filesync::FileStore& fs, std::string_view root,
                                           std::string_view owner_path,
                                           const serializer::JsonValue& doc,
                                           std::string_view layer_id,
                                           const std::vector<CellEdit>& edits);

// The result of committing a staged edit to disk through the L-33 sidecar-first family write.
struct CommitResult
{
    bool ok = false;
    std::string error_code;
    std::string error_message;
    std::uint64_t owner_raw_hash = 0;       // raw-byte hash of the written owner bytes
    std::uint64_t owner_canonical_hash = 0; // canonical-content hash of the written owner bytes
};

// Execute a staged EditOutcome against `fs`: validate the family coherence through filesync's
// plan_sidecar_family_write (the L-33 planner — it REFUSES a dangling/lying family, so this path can
// never durably author an incoherent one), then apply the plan's dependency-safe step order
// (sidecars BEFORE the referencing owner) via filesync::atomic_write. The one-shot commit executor
// shared by the CLI verbs and the GUI gesture-end commit (L-20: the GUI commits at gesture end —
// no Save button, no mid-gesture disk writes).
[[nodiscard]] CommitResult commit_edit(filesync::FileStore& fs, std::string_view root,
                                       std::string_view owner_path, const EditOutcome& edit);

} // namespace context::editor::tilemap
