// The <asset>.meta.json sidecar (L-36): GUID identity + import settings beside the asset.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::assetdb
{

// Sidecar naming: `<asset>.meta.json` sits beside the asset it identifies.
inline constexpr std::string_view kMetaSuffix = ".meta.json";

// The meta document's own kind id + schema version (the L-32 header block). The M2 meta schema
// RESERVES the `platforms` block for per-platform import-setting overrides (L-36): the member is
// written (empty) at creation, carried verbatim by every move/heal, and never interpreted in v1.
inline constexpr std::string_view kMetaKindId = "ctx:meta";
inline constexpr std::int64_t kMetaSchemaVersion = 1;

[[nodiscard]] std::string meta_path_for(std::string_view asset_path);
[[nodiscard]] bool is_meta_path(std::string_view path) noexcept;
// Precondition: is_meta_path(meta_path).
[[nodiscard]] std::string asset_path_for(std::string_view meta_path);

// The parsed identity fields of one sidecar. Only the fields the index needs are extracted
// (R-FILE-011(e): the GUID index holds bounded identity tuples, never payloads); rewriting an
// EXISTING meta at a new path copies the original bytes verbatim, so unknown/extra members and
// import settings always survive a move untouched.
struct AssetMeta
{
    std::string guid; // immutable identity (L-36); the pinned 32-hex form
    std::string kind; // the asset's kind id ("ctx:scene", ...); "" when not (yet) known
};

// Parse sidecar bytes. Hard failures (not JSON, root not an object, `guid` missing or malformed)
// return nullopt; softer shape oddities (a missing or foreign `$schema` header, a wrong-typed
// `kind`) parse with a note appended to `problems` so scan can surface them without dropping
// identity.
[[nodiscard]] std::optional<AssetMeta> parse_meta(std::string_view bytes,
                                                  std::vector<std::string>& problems);

// Serialize a FRESH sidecar (meta creation, the first R-FILE-003 enumerated write) in canonical
// form (L-32): the `$schema`/`version` header, the guid, `kind` (omitted while unknown), an empty
// `importSettings` object, and the RESERVED empty `platforms` block. Output is a canonical
// fixpoint: re-canonicalizing it is byte-identical (the idempotence the write surface requires).
[[nodiscard]] std::string serialize_meta(const AssetMeta& meta);

} // namespace context::editor::assetdb
