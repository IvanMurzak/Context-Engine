// Build-side chunked content-pack READER / verifier (docs/chunk-pack-format.md §4): parses a v1 pack
// byte stream back into its directory + decoded chunk bodies, verifying each chunk's FNV-1a-64
// content hash on read (self-verifying per R-FILE-010). This is a SYNCHRONOUS build-side reader used
// to prove the writer round-trips + to gate golden fixtures — it is NOT the runtime async streaming
// loader (that is the a02 task, R-ASSET-003), which builds its memory-budgeted scheduler over this
// same frozen format.

#pragma once

#include "context/editor/pack/pack_format.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::pack
{

// One parsed directory entry with its decoded chunk bytes (docs/chunk-pack-format.md §3.1 / §3.2).
struct PackEntry
{
    std::uint64_t unit_id = 0;      // the load-by-GUID key (composed identity, or a sidecar raw hash)
    std::uint64_t parent_unit = 0;  // the containing unit for a nested unit; 0 for top-level / sidecar
    bool is_root = false;
    bool has_parent = false;
    bool is_sidecar = false;
    Codec codec = Codec::store;
    std::uint32_t platform = kPlatformCommon;
    std::uint64_t entity_count = 0; // composed entities in the unit (0 for a sidecar)
    std::string source_scene;       // the instanced scene reference / sidecar owner-relative path
    std::uint64_t content_hash = 0; // the recorded FNV-1a-64 of the stored chunk bytes
    std::string chunk_bytes;        // the decoded chunk bytes (== stored bytes for codec = store)
};

// The parse outcome. `ok == false` ⇒ `errors` names every malformation encountered; a partially
// parsed `entries` is left best-effort for diagnosis but must not be trusted. Error codes:
// "pack.truncated", "pack.bad_magic", "pack.unsupported_version", "pack.bad_header",
// "pack.bad_directory", "pack.string_out_of_range", "pack.chunk_out_of_range",
// "pack.unsupported_codec", "pack.hash_mismatch".
struct ParsedPack
{
    bool ok = false;
    std::vector<std::string> errors;
    std::uint32_t format_version = 0;
    std::uint64_t engine_version = 0;
    std::string root_scene;
    std::vector<PackEntry> entries;
};

// Parse + verify a v1 pack byte stream. Deterministic and total (never throws). Verifies the magic,
// version, region bounds, and every chunk's content hash; a hash mismatch or an out-of-range offset
// flips `ok` and records the code, so a corrupt pack is refused, not used with a warning (R-SEC-009
// discipline). Decodes each chunk per its `codec` (v1: store = verbatim).
[[nodiscard]] ParsedPack read_pack(std::string_view bytes);

// Look up a directory entry by unit id (a linear build-side scan — the O(1) runtime index is the a02
// loader's concern). Returns nullptr for an unknown id.
[[nodiscard]] const PackEntry* find_unit(const ParsedPack& pack, std::uint64_t unit_id);

} // namespace context::editor::pack
