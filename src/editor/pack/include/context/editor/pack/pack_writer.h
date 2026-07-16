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

// One per-platform variant of a sidecar (task a03, R-BUILD-003): the transcoded payload the pack
// selects when it targets that platform. `platform` is a frozen PlatformVariant id (pack_format.h);
// the build pipeline produces `bytes` via the transcode node (import/transcode.h).
//
// A variant supplies BYTES, never an identity: the packed entry keeps the owning sidecar's declared
// `raw_hash` as its unit id on every target, because that is the GUID the composed entity JSON pins
// in its {"$sidecar", "hash"} reference (L-33) and content units stay platform-neutral. The variant
// bytes' own content-address is recorded (and self-verified by read_pack) in the entry's
// `content_hash`, so a variant needs no id of its own — the pack is single-target, so exactly one
// variant per sidecar is ever emitted.
struct PackSidecarVariant
{
    PlatformVariant platform = PlatformVariant::common; // the platform this variant targets
    std::string bytes;                                  // the transcoded payload, stored verbatim
};

// A binary sidecar blob to embed in the pack (L-33: textures, meshes, audio referenced from composed
// entity JSON as {"$sidecar": "<relpath>", "hash": "<raw hash>"}). Packed as a first-class chunk so
// the runtime loads it by GUID alongside its unit (docs/chunk-pack-format.md §4.5). The build
// pipeline supplies the bytes it already has on disk; the writer never touches the filesystem.
//
// Per-platform variants (a03): when `variants` carries an entry for the write's target platform, the
// writer packs THAT variant's bytes under its platform column instead of the common `bytes` — one
// source asset → each target's optimal format at pack time (R-BUILD-003). A target with no matching
// variant falls back to the common blob, so a platform-invariant asset (e.g. a `meshopt` mesh,
// identical on every v1 target) needs no per-platform variant. `raw_hash` addresses the sidecar on
// EVERY target (variants swap bytes, not identity — see PackSidecarVariant).
struct PackSidecar
{
    std::string relpath;            // owner-relative path (the readable hint, stored in sourceScene)
    std::uint64_t raw_hash = 0;     // the declared raw-byte hash — the sidecar's load-by-GUID key
    std::string bytes;              // the common sidecar payload, stored verbatim (codec = store)
    std::vector<PackSidecarVariant> variants; // per-platform variants (a03); empty ⇒ common only (a01)
};

// Writer knobs. `engine_version` is the R-FILE-010 cache-key input recorded in the pack header —
// same (files, engine version) ⇒ identical bytes.
struct PackWriteOptions
{
    std::uint64_t engine_version = kDefaultEngineVersion;
    // The platform this pack is built FOR. PlatformVariant::common (the a01 default) packs every
    // sidecar's common blob — byte-identical to a pre-a03 pack. A non-common target selects each
    // sidecar's matching variant (R-BUILD-003): the same project packs per target with per-target
    // variant payloads. Enum-typed like its sibling `Codec`: the u32 is the ON-DISK column's type,
    // cast at the byte boundary in the writer, not carried through the API.
    PlatformVariant target_platform = PlatformVariant::common;
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
