// Chunked content-pack format v1 constants + little-endian byte primitives (docs/chunk-pack-format.md
// §4). The build-side writer (pack_writer.h) and the verification reader (pack_reader.h) share these
// so the on-disk layout has ONE source of truth — a frozen R-ASSET-005 format (GUID-addressed
// content units, deterministic bytes for the R-FILE-010 cache key).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace context::editor::pack
{

// Pack-header magic: "CPAK" (docs/chunk-pack-format.md §3.3 / §4.2).
inline constexpr char kMagic[4] = {'C', 'P', 'A', 'K'};

// The frozen on-disk format version (this document's version). Post-v1 changes are ADDITIVE
// (new codec ids, appended directory columns, header fields behind headerSize) — never a break.
inline constexpr std::uint32_t kFormatVersion = 1;

// Fixed byte sizes of the v1 header + directory entry (docs/chunk-pack-format.md §4.2 / §4.3).
inline constexpr std::uint32_t kHeaderSize = 88;
inline constexpr std::uint32_t kDirectoryEntrySize = 76;

// The default engine-version cache-key input (R-FILE-010). The real build pipeline (a05) supplies
// the shipped engine version via PackWriteOptions; this is the deterministic default for tests and
// standalone use. Same (files, engine version) ⇒ identical bytes.
inline constexpr std::uint64_t kDefaultEngineVersion = 1;

// Payload codec ids (docs/chunk-pack-format.md §4.4). v1 pins `store`; the compressed ids are
// reserved so introducing one later is an additive per-chunk selector, not a format break.
enum class Codec : std::uint32_t
{
    store = 0,   // identity — chunk bytes stored verbatim (v1)
    deflate = 1, // reserved
    zstd = 2,    // reserved
};

// The v1-only platform variant selector (docs/chunk-pack-format.md §3.1) — 0 = common/all. Non-zero
// values are re-homed to the a03 per-platform-variant task.
inline constexpr std::uint32_t kPlatformCommon = 0;

// Directory-entry `flags` bits (docs/chunk-pack-format.md §4.3).
inline constexpr std::uint32_t kFlagIsRoot = 1u << 0;   // the root unit vs a top-level-instance unit
inline constexpr std::uint32_t kFlagHasParent = 1u << 1; // a nested unit carrying a non-zero parentUnit
inline constexpr std::uint32_t kFlagIsSidecar = 1u << 2; // a packed binary sidecar blob, not a unit

// --- little-endian primitives (a fixed byte order ⇒ identical bytes on every host) --------------

// Append `v` to `out` as fixed-width little-endian bytes.
void put_u32(std::string& out, std::uint32_t v);
void put_u64(std::string& out, std::uint64_t v);

// Read a fixed-width little-endian integer at `offset`. Returns false (leaving `out` untouched) if
// `offset + width` exceeds `bytes.size()` — the reader stays total on a truncated pack.
[[nodiscard]] bool read_u32(std::string_view bytes, std::size_t offset, std::uint32_t& out) noexcept;
[[nodiscard]] bool read_u64(std::string_view bytes, std::size_t offset, std::uint64_t& out) noexcept;

} // namespace context::editor::pack
