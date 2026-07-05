// Tilemap content-kind semantics (R-2D-003): the CONTENT rules the ctx:tilemap schema shape cannot
// express — the L-33 ~1 MB split-nudge over chunk cell payloads, stable-id uniqueness, region sanity.

#pragma once

#include "context/editor/kinds/diagnostic.h"
#include "context/editor/serializer/json_tree.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::kinds
{

// The L-33 soft per-file split ceiling (~1 MB). A chunk whose packed cell payload exceeds it earns a
// split-nudge (tilemap.chunk_oversize) — ADVISORY, never blocking: the file is still valid, but an
// agent/tool is nudged to split the region so authored files stay AI-context-window-sized and diff
// locally at 100k-file scale (L-33 / R-FILE-011).
inline constexpr std::size_t kTilemapSplitCeilingBytes = std::size_t{1} << 20; // 1 MiB

// One packed cell is a u32 global tile id (spanning every tile-set — see the ctx:tilemap schema's
// firstTileId offsets), so a width x height chunk grid packs to width*height*4 bytes.
inline constexpr std::size_t kTilemapBytesPerCell = 4;

// Packed byte size of a `width` x `height` cell grid; 0 for a non-positive extent (reported
// separately as tilemap.region_invalid). Saturates at SIZE_MAX rather than overflowing on absurd
// dimensions, so the oversize comparison is always well-defined.
[[nodiscard]] std::size_t tilemap_chunk_bytes(long long width, long long height) noexcept;

// Semantic analysis of a parsed ctx:tilemap document (BEYOND schema validation — the schema pins the
// SHAPE, this pins the CONTENT rules the dialect cannot):
//   - tilemap.id_duplicate   — two tile-sets, or two layers, share a stable `id`;
//   - tilemap.region_invalid — a chunk `region` has a non-positive width or height;
//   - tilemap.chunk_oversize — a chunk's packed cell payload exceeds the split-nudge ceiling (L-33).
// When `sidecar_sizes` maps a chunk's authored "$sidecar" relpath to the sidecar's ACTUAL on-disk
// payload byte count, the oversize check uses that measured size; otherwise it ESTIMATES from region
// area x kTilemapBytesPerCell. Deterministic; diagnostics are emitted in document order. A
// schema-invalid document (missing/mistyped members) is skipped gracefully — never crashes — because
// schema::validate_document is the gate for shape; this pass only adds the content rules on top.
[[nodiscard]] std::vector<KindDiagnostic>
analyze_tilemap(const serializer::JsonValue& doc,
                const std::map<std::string, std::size_t>* sidecar_sizes = nullptr);

} // namespace context::editor::kinds
