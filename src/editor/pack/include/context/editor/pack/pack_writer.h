// Build-side chunked content-pack WRITER (R-ASSET-005 / L-35 / L-37, docs/chunk-pack-format.md §4):
// packs flatten-emitted content units (src/editor/compose) into a v1 chunked, GUID-addressed archive
// with a directory (GUID→chunk index, per-chunk hash, format/engine version header). Deterministic —
// the same (content units, sidecars, engine version) produce byte-identical output, so a pack is a
// cache-keyable pure function (R-FILE-010) and the double-run byte-compare gate holds.
//
// This is the M8 WRITER only. The runtime async streaming loader (load/instantiate/unload by GUID
// with a memory-budgeted scheduler) is the a02 task (R-ASSET-003); pack_reader.h here is a
// synchronous build-side verification reader, not that loader.

#pragma once

#include "context/editor/compose/content_unit.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/pack/pack_format.h"

#include <cstdint>
#include <string>
#include <vector>

namespace context::editor::pack
{

// A binary sidecar blob to embed in the pack (L-33: textures, meshes, audio referenced from composed
// entity JSON as {"$sidecar": "<relpath>", "hash": "<raw hash>"}). Packed as a first-class chunk so
// the runtime loads it by GUID alongside its unit (docs/chunk-pack-format.md §4.5). The build
// pipeline supplies the bytes it already has on disk; the writer never touches the filesystem.
struct PackSidecar
{
    std::string relpath;            // owner-relative path (the readable hint, stored in sourceScene)
    std::uint64_t raw_hash = 0;     // the declared raw-byte hash — the sidecar's load-by-GUID key
    std::string bytes;              // the sidecar payload, stored verbatim (codec = store)
};

// Writer knobs. `engine_version` is the R-FILE-010 cache-key input recorded in the pack header —
// same (files, engine version) ⇒ identical bytes.
struct PackWriteOptions
{
    std::uint64_t engine_version = kDefaultEngineVersion;
};

// The outcome of a pack write. `ok == true` ⇒ `bytes` is the complete v1 pack stream; `ok == false`
// ⇒ `bytes` is empty and `error` names the failure ("pack.noncanonical_payload" when a composed
// entity value cannot serialize to canonical JSON — R-FILE-001 bans NaN/Infinity, so this cannot
// arise from a well-formed composed scene).
struct PackWriteResult
{
    bool ok = false;
    std::string bytes;
    std::string error;
};

// Build a v1 pack from a content-unit partition + the composed scene it partitions, plus optional
// binary sidecar blobs. Deterministic and total. Chunk bodies are canonical JSON of the frozen
// identity/id-path/payload triple (docs/chunk-pack-format.md §3.2 / §4.4); each directory entry
// carries the L-37 composed identity as its unit id, the per-chunk FNV-1a-64 content hash, and the
// format/engine version header — composed-identity addressing preserved end-to-end.
[[nodiscard]] PackWriteResult write_pack(const compose::ContentUnitSet& units,
                                         const compose::ComposedScene& scene,
                                         const std::vector<PackSidecar>& sidecars = {},
                                         const PackWriteOptions& options = {});

} // namespace context::editor::pack
