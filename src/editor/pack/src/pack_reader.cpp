// Build-side chunked content-pack reader / verifier (see pack_reader.h): parses a v1 pack byte
// stream back into its directory + decoded chunk bodies, verifying every chunk's content hash on
// read (docs/chunk-pack-format.md §4). Total and deterministic — a corrupt/truncated pack yields
// ok == false + error codes, never a throw or an out-of-bounds read.

#include "context/editor/pack/pack_reader.h"

#include "context/editor/pack/pack_format.h"
#include "context/editor/serializer/canonical.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace context::editor::pack
{

namespace serializer = context::editor::serializer;

ParsedPack read_pack(std::string_view bytes)
{
    ParsedPack pack;
    const auto fail = [&pack](const char* code) { pack.errors.emplace_back(code); };

    if (bytes.size() < kHeaderSize)
    {
        fail("pack.truncated");
        return pack;
    }
    if (bytes[0] != kMagic[0] || bytes[1] != kMagic[1] || bytes[2] != kMagic[2] || bytes[3] != kMagic[3])
    {
        fail("pack.bad_magic");
        return pack;
    }

    bool ok = true;
    std::uint32_t version = 0;
    std::uint32_t header_size = 0;
    std::uint32_t header_flags = 0;
    ok &= read_u32(bytes, 4, version);
    ok &= read_u32(bytes, 8, header_size);
    ok &= read_u32(bytes, 12, header_flags);
    pack.format_version = version;
    if (version != kFormatVersion)
    {
        fail("pack.unsupported_version");
        return pack;
    }

    std::uint32_t root_off = 0;
    std::uint32_t root_len = 0;
    std::uint64_t unit_count = 0;
    std::uint64_t dir_off = 0;
    std::uint64_t dir_size = 0;
    std::uint64_t str_off = 0;
    std::uint64_t str_size = 0;
    std::uint64_t chunk_region_off = 0;
    std::uint64_t chunk_region_size = 0;
    ok &= read_u64(bytes, 16, pack.engine_version);
    ok &= read_u32(bytes, 24, root_off);
    ok &= read_u32(bytes, 28, root_len);
    ok &= read_u64(bytes, 32, unit_count);
    ok &= read_u64(bytes, 40, dir_off);
    ok &= read_u64(bytes, 48, dir_size);
    ok &= read_u64(bytes, 56, str_off);
    ok &= read_u64(bytes, 64, str_size);
    ok &= read_u64(bytes, 72, chunk_region_off);
    ok &= read_u64(bytes, 80, chunk_region_size);
    if (!ok)
    {
        fail("pack.bad_header");
        return pack;
    }

    // Resolve a string ref against the string-table region — bounded both within the table and
    // within the whole stream.
    const auto get_string = [&](std::uint32_t off, std::uint32_t len, std::string& dst) -> bool {
        if (off > str_size || len > str_size - off)
            return false;
        const std::uint64_t start = str_off + off;
        if (start > bytes.size() || bytes.size() - start < len)
            return false;
        dst.assign(bytes.data() + start, len);
        return true;
    };

    if (!get_string(root_off, root_len, pack.root_scene))
    {
        fail("pack.string_out_of_range");
        return pack;
    }

    // Directory region bounds + exact-size check.
    if (dir_off > bytes.size() || bytes.size() - dir_off < dir_size)
    {
        fail("pack.bad_directory");
        return pack;
    }
    // Derive the entry count from the (already bounds-checked) dir_size rather than multiplying
    // unit_count out: unit_count is an unvalidated u64, and unit_count * kDirectoryEntrySize can
    // overflow u64 and wrap to match a small dir_size — which would slip past the check and then
    // drive entries.reserve(unit_count) with a huge value, throwing std::length_error and breaking
    // this reader's total/never-throws contract. dir_size <= bytes.size() here, so the derived count
    // (and thus the reserve below) stays bounded. Acceptance is identical for every well-formed pack,
    // where dir_size == unit_count * kDirectoryEntrySize exactly.
    if (dir_size % static_cast<std::uint64_t>(kDirectoryEntrySize) != 0
        || unit_count != dir_size / static_cast<std::uint64_t>(kDirectoryEntrySize))
    {
        fail("pack.bad_directory");
        return pack;
    }
    (void)chunk_region_off;
    (void)chunk_region_size;
    (void)header_size;
    (void)header_flags;

    pack.entries.reserve(static_cast<std::size_t>(unit_count));
    for (std::uint64_t i = 0; i < unit_count; ++i)
    {
        const std::size_t base =
            static_cast<std::size_t>(dir_off) + static_cast<std::size_t>(i) * kDirectoryEntrySize;

        PackEntry e;
        std::uint32_t eflags = 0;
        std::uint32_t ecodec = 0;
        std::uint32_t eplatform = 0;
        std::uint32_t soff = 0;
        std::uint32_t slen = 0;
        std::uint64_t chunk_off = 0;
        std::uint64_t chunk_len = 0;
        std::uint64_t uncompressed_len = 0;

        bool entry_ok = true;
        entry_ok &= read_u64(bytes, base + 0, e.unit_id);
        entry_ok &= read_u64(bytes, base + 8, e.parent_unit);
        entry_ok &= read_u32(bytes, base + 16, eflags);
        entry_ok &= read_u32(bytes, base + 20, ecodec);
        entry_ok &= read_u32(bytes, base + 24, eplatform);
        entry_ok &= read_u64(bytes, base + 28, e.entity_count);
        entry_ok &= read_u32(bytes, base + 36, soff);
        entry_ok &= read_u32(bytes, base + 40, slen);
        entry_ok &= read_u64(bytes, base + 44, chunk_off);
        entry_ok &= read_u64(bytes, base + 52, chunk_len);
        entry_ok &= read_u64(bytes, base + 60, uncompressed_len);
        entry_ok &= read_u64(bytes, base + 68, e.content_hash);
        if (!entry_ok)
        {
            fail("pack.bad_directory");
            return pack;
        }

        e.is_root = (eflags & kFlagIsRoot) != 0;
        e.has_parent = (eflags & kFlagHasParent) != 0;
        e.is_sidecar = (eflags & kFlagIsSidecar) != 0;
        e.platform = eplatform;

        if (ecodec != static_cast<std::uint32_t>(Codec::store))
        {
            fail("pack.unsupported_codec"); // v1 stores only; a compressed codec is a later additive id
            return pack;
        }
        e.codec = Codec::store;

        if (!get_string(soff, slen, e.source_scene))
        {
            fail("pack.string_out_of_range");
            return pack;
        }

        if (chunk_off > bytes.size() || bytes.size() - chunk_off < chunk_len)
        {
            fail("pack.chunk_out_of_range");
            return pack;
        }
        const std::string_view stored = bytes.substr(static_cast<std::size_t>(chunk_off),
                                                     static_cast<std::size_t>(chunk_len));
        // Self-verifying on read (R-FILE-010 / R-SEC-009 discipline): a mismatch is refused.
        if (serializer::canonical_hash_of(stored) != e.content_hash)
        {
            fail("pack.hash_mismatch");
            return pack;
        }
        // Store codec: the decoded bytes ARE the stored bytes, so the two lengths must agree.
        if (uncompressed_len != chunk_len)
        {
            fail("pack.bad_directory");
            return pack;
        }
        e.chunk_bytes.assign(stored.data(), stored.size());
        pack.entries.push_back(std::move(e));
    }

    pack.ok = true;
    return pack;
}

const PackEntry* find_unit(const ParsedPack& pack, std::uint64_t unit_id)
{
    for (const PackEntry& e : pack.entries)
        if (e.unit_id == unit_id)
            return &e;
    return nullptr;
}

} // namespace context::editor::pack
