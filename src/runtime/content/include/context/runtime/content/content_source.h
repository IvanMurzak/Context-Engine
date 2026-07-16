// The RuntimeKernel content loading SEAM (R-FILE-009 / L-24, R-ASSET-005): the one derived-artifact
// consumer that materializes packed content units by GUID. RuntimeKernel never parses authored
// files — it reads derivation-graph output through this seam, fed two ways:
//   - PackContentSource  — a v1 chunked pack baked into a shipped build (docs/chunk-pack-format.md);
//   - EditorContentSource — the live in-memory derived units EditorKernel feeds in play-in-editor.
// One data format, two feeds ⇒ editor==build fidelity by construction (L-24). The chunked format is
// the a01 deliverable (frozen v1); THIS module is the a02 runtime loader over it.

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <string>
#include <vector>

namespace context::runtime::content
{

namespace serializer = context::editor::serializer;

// One materialized entity of a loaded content unit — the frozen identity + id-path + payload triple
// (docs/chunk-pack-format.md §3.2). `value` is the composed component payload as a canonical-JSON
// tree; the runtime addresses an entity inside a resident unit by its composed identity (L-37), the
// same key player saves use to re-address entities after a re-derivation (R-DATA-005).
struct UnitEntity
{
    std::uint64_t identity = 0;       // composed identity (L-37) — the addressing key
    std::vector<std::string> id_path; // the L-35 id-path from the flatten root
    serializer::JsonValue value;      // the composed entity object (overrides applied)
};

// A content unit materialized ("instantiated") through the seam: its entities (or a sidecar's bytes)
// plus the residency cost the streaming budget accounts against. A sidecar (L-33 texture/mesh/audio)
// carries `sidecar_bytes` and no entities; a unit carries `entities` and no sidecar bytes.
struct LoadedUnit
{
    std::uint64_t unit_id = 0;
    std::uint64_t parent_unit = 0;
    bool is_root = false;
    bool is_sidecar = false;
    std::vector<UnitEntity> entities; // empty for a sidecar
    std::string sidecar_bytes;        // non-empty only for a sidecar
    std::uint64_t resident_bytes = 0; // memory cost while resident (the unit's stored chunk length)
};

// One directory row — the cheap index a source exposes WITHOUT decoding any chunk body, so the loader
// / scheduler can plan load/unload by GUID in O(1) before paying the decode cost of materializing.
struct UnitDescriptor
{
    std::uint64_t unit_id = 0;
    std::uint64_t parent_unit = 0;
    bool is_root = false;
    bool is_sidecar = false;
    std::uint64_t entity_count = 0;
    std::uint64_t resident_bytes = 0; // the unit's memory cost once resident (its chunk length)
};

// The loading seam. A source enumerates its content units cheaply (`directory`) and materializes one
// by GUID on demand (`load`) — the async streaming loader never touches a chunk it did not ask for.
class ContentSource
{
public:
    virtual ~ContentSource() = default;

    // Every unit id + its cheap metadata, WITHOUT decoding any chunk body. Built once up front.
    [[nodiscard]] virtual const std::vector<UnitDescriptor>& directory() const = 0;

    // Materialize one unit by GUID (decode + verify its chunk, or copy the in-memory unit). Returns
    // false and sets `*error` (when non-null) for an unknown id or a corrupt/undecodable chunk — the
    // loader refuses a bad unit rather than instantiating garbage (R-SEC-009 discipline).
    [[nodiscard]] virtual bool load(std::uint64_t unit_id, LoadedUnit& out,
                                    std::string* error) const = 0;

    // O(1) presence check against the directory index.
    [[nodiscard]] virtual bool contains(std::uint64_t unit_id) const = 0;
};

} // namespace context::runtime::content
