// PackContentSource (see pack_source.h): the shipped-build feed of the loading seam over a frozen v1
// pack. Parses header + directory + string table once (the O(1) index), decodes each chunk lazily on
// load() with self-verifying FNV-1a-64 hashing (docs/chunk-pack-format.md §4). Total + deterministic.

#include "context/runtime/content/pack_source.h"

#include "content_detail.h"

#include "context/editor/pack/pack_format.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include <cstdint>
#include <utility>

namespace context::runtime::content
{

namespace pack = context::editor::pack;
namespace serializer = context::editor::serializer;

PackContentSource::PackContentSource(std::string bytes) : bytes_(std::move(bytes))
{
    ok_ = build_index();
    if (!ok_)
    {
        directory_.clear();
        index_.clear();
    }
}

bool PackContentSource::build_index()
{
    const std::string_view b = bytes_;
    const auto fail = [this](const char* code) {
        error_ = code;
        return false;
    };

    if (b.size() < pack::kHeaderSize)
        return fail("pack.truncated");
    if (b[0] != pack::kMagic[0] || b[1] != pack::kMagic[1] || b[2] != pack::kMagic[2]
        || b[3] != pack::kMagic[3])
        return fail("pack.bad_magic");

    bool ok = true;
    std::uint32_t version = 0;
    ok &= pack::read_u32(b, 4, version);
    if (!ok)
        return fail("pack.bad_header");
    if (version != pack::kFormatVersion)
        return fail("pack.unsupported_version");

    std::uint32_t root_off = 0;
    std::uint32_t root_len = 0;
    std::uint64_t unit_count = 0;
    std::uint64_t dir_off = 0;
    std::uint64_t dir_size = 0;
    std::uint64_t str_off = 0;
    std::uint64_t str_size = 0;
    std::uint64_t chunk_region_off = 0;
    std::uint64_t chunk_region_size = 0;
    ok &= pack::read_u64(b, 16, engine_version_);
    ok &= pack::read_u32(b, 24, root_off);
    ok &= pack::read_u32(b, 28, root_len);
    ok &= pack::read_u64(b, 32, unit_count);
    ok &= pack::read_u64(b, 40, dir_off);
    ok &= pack::read_u64(b, 48, dir_size);
    ok &= pack::read_u64(b, 56, str_off);
    ok &= pack::read_u64(b, 64, str_size);
    ok &= pack::read_u64(b, 72, chunk_region_off);
    ok &= pack::read_u64(b, 80, chunk_region_size);
    if (!ok)
        return fail("pack.bad_header");
    (void)chunk_region_off;
    (void)chunk_region_size;

    // Resolve a string ref against the string-table region — bounded within the table AND the stream.
    const auto get_string = [&](std::uint32_t off, std::uint32_t len, std::string& dst) -> bool {
        if (off > str_size || len > str_size - off)
            return false;
        const std::uint64_t start = str_off + off;
        if (start > b.size() || b.size() - start < len)
            return false;
        dst.assign(b.data() + start, len);
        return true;
    };

    if (!get_string(root_off, root_len, root_scene_))
        return fail("pack.string_out_of_range");

    if (dir_off > b.size() || b.size() - dir_off < dir_size)
        return fail("pack.bad_directory");
    // Derive the entry count from the bounds-checked dir_size, never from the unvalidated unit_count
    // (unit_count * entrySize can overflow u64 and slip a huge reserve past a small dir_size — the
    // same total/never-throws hardening the build-side reader documents).
    if (dir_size % static_cast<std::uint64_t>(pack::kDirectoryEntrySize) != 0
        || unit_count != dir_size / static_cast<std::uint64_t>(pack::kDirectoryEntrySize))
        return fail("pack.bad_directory");

    directory_.reserve(static_cast<std::size_t>(unit_count));
    index_.reserve(static_cast<std::size_t>(unit_count));
    for (std::uint64_t i = 0; i < unit_count; ++i)
    {
        const std::size_t base = static_cast<std::size_t>(dir_off)
                                 + static_cast<std::size_t>(i) * pack::kDirectoryEntrySize;

        std::uint64_t unit_id = 0;
        std::uint64_t parent_unit = 0;
        std::uint32_t eflags = 0;
        std::uint32_t ecodec = 0;
        std::uint32_t eplatform = 0;
        std::uint64_t entity_count = 0;
        std::uint32_t soff = 0;
        std::uint32_t slen = 0;
        std::uint64_t chunk_off = 0;
        std::uint64_t chunk_len = 0;
        std::uint64_t uncompressed_len = 0;
        std::uint64_t content_hash = 0;

        bool entry_ok = true;
        entry_ok &= pack::read_u64(b, base + 0, unit_id);
        entry_ok &= pack::read_u64(b, base + 8, parent_unit);
        entry_ok &= pack::read_u32(b, base + 16, eflags);
        entry_ok &= pack::read_u32(b, base + 20, ecodec);
        entry_ok &= pack::read_u32(b, base + 24, eplatform);
        entry_ok &= pack::read_u64(b, base + 28, entity_count);
        entry_ok &= pack::read_u32(b, base + 36, soff);
        entry_ok &= pack::read_u32(b, base + 40, slen);
        entry_ok &= pack::read_u64(b, base + 44, chunk_off);
        entry_ok &= pack::read_u64(b, base + 52, chunk_len);
        entry_ok &= pack::read_u64(b, base + 60, uncompressed_len);
        entry_ok &= pack::read_u64(b, base + 68, content_hash);
        if (!entry_ok)
            return fail("pack.bad_directory");
        (void)eplatform;
        (void)slen;
        (void)soff;

        if (ecodec != static_cast<std::uint32_t>(pack::Codec::store))
            return fail("pack.unsupported_codec"); // v1 stores only (a compressed codec is additive)
        // Store codec: decoded length == stored length. Bound the chunk against the stream now so the
        // lazy decode in load() can trust the recorded offset/length.
        if (uncompressed_len != chunk_len)
            return fail("pack.bad_directory");
        if (chunk_off > b.size() || b.size() - chunk_off < chunk_len)
            return fail("pack.chunk_out_of_range");

        UnitDescriptor desc;
        desc.unit_id = unit_id;
        desc.parent_unit = parent_unit;
        desc.is_root = (eflags & pack::kFlagIsRoot) != 0;
        desc.is_sidecar = (eflags & pack::kFlagIsSidecar) != 0;
        desc.entity_count = entity_count;
        desc.resident_bytes = chunk_len;
        directory_.push_back(desc);

        Entry entry;
        entry.unit_id = unit_id;
        entry.parent_unit = parent_unit;
        entry.is_root = desc.is_root;
        entry.is_sidecar = desc.is_sidecar;
        entry.entity_count = entity_count;
        entry.chunk_offset = chunk_off;
        entry.chunk_length = chunk_len;
        entry.content_hash = content_hash;
        index_.emplace(unit_id, entry); // first-wins on a duplicate id (a malformed pack)
    }

    return true;
}

bool PackContentSource::contains(std::uint64_t unit_id) const
{
    return index_.find(unit_id) != index_.end();
}

bool PackContentSource::load(std::uint64_t unit_id, LoadedUnit& out, std::string* error) const
{
    const auto set_error = [error](const char* code) {
        if (error != nullptr)
            *error = code;
        return false;
    };

    const auto it = index_.find(unit_id);
    if (it == index_.end())
        return set_error("content.unknown_unit");
    const Entry& e = it->second;

    const std::string_view stored = std::string_view(bytes_).substr(
        static_cast<std::size_t>(e.chunk_offset), static_cast<std::size_t>(e.chunk_length));
    // Self-verifying on read (R-FILE-010 / R-SEC-009): a flipped byte is refused, not instantiated.
    if (serializer::canonical_hash_of(stored) != e.content_hash)
        return set_error("pack.hash_mismatch");

    out = LoadedUnit{};
    out.unit_id = e.unit_id;
    out.parent_unit = e.parent_unit;
    out.is_root = e.is_root;
    out.is_sidecar = e.is_sidecar;
    out.resident_bytes = e.chunk_length;

    if (e.is_sidecar)
    {
        // A sidecar chunk is stored verbatim — the runtime loads its bytes by GUID alongside its unit.
        out.sidecar_bytes.assign(stored.data(), stored.size());
        return true;
    }

    // A unit chunk body is canonical JSON: {"unitId", "entities":[{"identityHash","idPath","value"}]}.
    serializer::ParseResult parsed = serializer::parse_json(stored);
    if (!parsed.ok)
        return set_error("content.chunk_parse_failed");
    const serializer::JsonValue* entities = detail::find_member(parsed.root, "entities");
    if (entities == nullptr || entities->type != serializer::JsonValue::Type::array)
        return set_error("content.chunk_malformed");

    out.entities.reserve(entities->elements.size());
    for (const serializer::JsonValue& obj : entities->elements)
    {
        const serializer::JsonValue* ident = detail::find_member(obj, "identityHash");
        const serializer::JsonValue* id_path = detail::find_member(obj, "idPath");
        const serializer::JsonValue* value = detail::find_member(obj, "value");
        if (ident == nullptr || ident->type != serializer::JsonValue::Type::string
            || id_path == nullptr || id_path->type != serializer::JsonValue::Type::array
            || value == nullptr)
            return set_error("content.chunk_malformed");
        const std::optional<std::uint64_t> identity = detail::parse_stable_id(ident->string_value);
        if (!identity.has_value())
            return set_error("content.chunk_malformed");

        UnitEntity ue;
        ue.identity = *identity;
        ue.id_path.reserve(id_path->elements.size());
        for (const serializer::JsonValue& seg : id_path->elements)
        {
            if (seg.type != serializer::JsonValue::Type::string)
                return set_error("content.chunk_malformed");
            ue.id_path.push_back(seg.string_value);
        }
        ue.value = *value;
        out.entities.push_back(std::move(ue));
    }
    return true;
}

} // namespace context::runtime::content
