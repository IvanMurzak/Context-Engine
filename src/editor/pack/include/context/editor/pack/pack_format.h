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

// `linux` is a PREDEFINED MACRO (= 1) in the GNU dialects (-std=gnu++NN) on Linux hosts, which would
// mangle the `linux` enumerator below into `PlatformVariant::1`. Context compiles with
// CMAKE_CXX_EXTENSIONS OFF (strict -std=c++NN), where it is NOT defined — but this is a PUBLIC header,
// so a consumer building in a GNU dialect would otherwise fail to compile it, and the break would be
// invisible to a Windows/GCC dev gate (it reds only the ubuntu leg). Undefine the non-standard macro
// defensively: nothing in the engine reads it — platform checks use __linux__ (platform_profile.cpp).
#ifdef linux
#undef linux
#endif

// The per-platform variant selector for the directory `platform` column (docs/chunk-pack-format.md
// §3.1). 0 = common/all (a platform-neutral chunk); the v1 platform set gets stable non-zero ids so a
// pack can carry the target's variant of an asset (task a03, R-BUILD-003 / L-36). The ids are FROZEN +
// APPEND-ONLY — never renumbered — because they are written into the on-disk directory. The string ids
// MIRROR import::platform_profiles() (windows/linux/macos/web); the pack format owns its own numeric
// namespace so context_pack_format stays dependency-free (the runtime loader links it without
// context_import). Android/iOS ids are reserved for when their platform legs activate (R-BUILD-001).
enum class PlatformVariant : std::uint32_t
{
    common = 0,
    windows = 1,
    linux = 2,
    macos = 3,
    web = 4,
    // 5+ reserved (android, ios) — added append-only when their legs land.
};

// The v1 platform-neutral selector — kept as a named constant (the a01 default; unchanged value).
inline constexpr std::uint32_t kPlatformCommon = static_cast<std::uint32_t>(PlatformVariant::common);

// Map a platform id string (import::platform_profiles() ids) to its frozen directory selector; returns
// PlatformVariant::common for an empty / unknown id (a platform-neutral chunk — never guessed).
[[nodiscard]] PlatformVariant platform_variant_for(std::string_view platform_id) noexcept;

// The stable lowercase id for a selector (the inverse of platform_variant_for) — "" for common,
// "windows"/"linux"/"macos"/"web" for the v1 set. Diagnostic + round-trip use.
[[nodiscard]] std::string_view platform_variant_name(PlatformVariant variant) noexcept;

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
