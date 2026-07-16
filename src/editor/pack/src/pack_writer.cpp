// Build-side chunked content-pack writer (see pack_writer.h): serializes a content-unit partition
// (+ optional binary sidecars) into the v1 on-disk layout (docs/chunk-pack-format.md §4). Pure and
// deterministic — the byte stream is a function of (units, sidecars, engine version) only.

#include "context/editor/pack/pack_writer.h"

#include "context/editor/compose/stable_id.h"
#include "context/editor/pack/pack_format.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace context::editor::pack
{

namespace
{
namespace serializer = context::editor::serializer;
using serializer::JsonMember;
using serializer::JsonValue;

[[nodiscard]] JsonValue json_string(std::string_view v)
{
    JsonValue out;
    out.type = JsonValue::Type::string;
    out.string_value = std::string(v);
    return out;
}

void set_member(JsonValue& object, std::string_view key, JsonValue value)
{
    JsonMember m;
    m.key = std::string(key);
    m.value = std::move(value);
    object.members.push_back(std::move(m));
}

// The string table: dedups by exact value, appends in first-use order. Returns each string's
// {offset, length} WITHIN the table region — the reader resolves it against stringTableOffset. The
// std::map is a lookup index only; the emitted byte order is first-use order, so it is deterministic.
class StringTable
{
public:
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> intern(const std::string& s)
    {
        const auto len = static_cast<std::uint32_t>(s.size());
        auto it = offsets_.find(s);
        if (it != offsets_.end())
            return {it->second, len};
        const auto offset = static_cast<std::uint32_t>(bytes_.size());
        offsets_.emplace(s, offset);
        bytes_ += s;
        return {offset, len};
    }

    [[nodiscard]] const std::string& bytes() const noexcept { return bytes_; }

private:
    std::string bytes_;
    std::map<std::string, std::uint32_t, std::less<>> offsets_;
};

// The canonical-JSON chunk body for one unit (docs/chunk-pack-format.md §3.2): the frozen
// identity + id-path + payload triple per composed entity. nullopt iff a composed value cannot
// serialize to canonical JSON (a non-finite double — impossible for a well-formed composed scene).
[[nodiscard]] std::optional<std::string> unit_chunk_bytes(const compose::ContentUnit& unit,
                                                          const compose::ComposedScene& scene)
{
    JsonValue root;
    root.type = JsonValue::Type::object;
    set_member(root, "unitId", json_string(unit.unit_id));

    JsonValue entities;
    entities.type = JsonValue::Type::array;
    for (std::size_t idx : unit.entity_indices)
    {
        const compose::ComposedEntity& e = scene.entities[idx];
        JsonValue obj;
        obj.type = JsonValue::Type::object;
        set_member(obj, "identityHash", json_string(compose::format_stable_id(e.identity_hash)));
        JsonValue id_path;
        id_path.type = JsonValue::Type::array;
        for (const std::string& seg : e.id_path)
            id_path.elements.push_back(json_string(seg));
        set_member(obj, "idPath", std::move(id_path));
        set_member(obj, "value", e.value); // a copy of the composed entity object
        entities.elements.push_back(std::move(obj));
    }
    set_member(root, "entities", std::move(entities));

    std::string out;
    if (!serializer::serialize_canonical(root, out))
        return std::nullopt;
    return out;
}

// One resolved directory entry + its stored chunk bytes, pre-layout (offsets assigned later).
struct Entry
{
    std::uint64_t unit_id = 0;
    std::uint64_t parent_unit = 0;
    std::uint32_t flags = 0;
    std::uint32_t codec = static_cast<std::uint32_t>(Codec::store);
    std::uint32_t platform = kPlatformCommon;
    std::uint64_t entity_count = 0;
    std::uint32_t source_off = 0;
    std::uint32_t source_len = 0;
    std::uint64_t content_hash = 0;
    std::string chunk; // the stored chunk bytes (codec = store ⇒ verbatim)
};

} // namespace

PackWriteResult write_pack(const compose::ContentUnitSet& units, const compose::ComposedScene& scene,
                           const std::vector<PackSidecar>& sidecars, const PackWriteOptions& options)
{
    PackWriteResult result;

    StringTable strings;
    // rootScene interns first, so the header's string ref is stable regardless of unit contents.
    const std::pair<std::uint32_t, std::uint32_t> root_ref = strings.intern(units.root_path);

    std::vector<Entry> entries;
    entries.reserve(units.units.size() + sidecars.size());

    // Content-unit entries, in the frozen ContentUnitSet order (root first — §2).
    for (const compose::ContentUnit& unit : units.units)
    {
        std::optional<std::string> body = unit_chunk_bytes(unit, scene);
        if (!body.has_value())
        {
            result.error = "pack.noncanonical_payload";
            return result;
        }
        const std::pair<std::uint32_t, std::uint32_t> src = strings.intern(unit.source_scene);
        Entry e;
        e.unit_id = unit.identity_hash;                 // the L-37 composed identity — load-by-GUID key
        e.parent_unit = 0;                              // top-level granularity (v1 emission; §2.1)
        e.flags = unit.is_root ? kFlagIsRoot : 0u;
        e.entity_count = unit.entity_indices.size();
        e.source_off = src.first;
        e.source_len = src.second;
        e.content_hash = serializer::canonical_hash_of(*body);
        e.chunk = std::move(*body);
        entries.push_back(std::move(e));
    }

    // Packed binary sidecars, in supplied order (docs/chunk-pack-format.md §4.5).
    for (const PackSidecar& sc : sidecars)
    {
        const std::pair<std::uint32_t, std::uint32_t> src = strings.intern(sc.relpath);
        Entry e;
        e.unit_id = sc.raw_hash;               // content-addressed by the declared raw-byte hash
        e.parent_unit = 0;
        e.flags = kFlagIsSidecar;
        e.entity_count = 0;
        e.source_off = src.first;
        e.source_len = src.second;
        e.content_hash = serializer::canonical_hash_of(sc.bytes);
        e.chunk = sc.bytes;
        entries.push_back(std::move(e));
    }

    // Region layout: header → directory → string table → chunk region.
    const std::uint64_t dir_offset = kHeaderSize;
    const std::uint64_t dir_size = static_cast<std::uint64_t>(entries.size()) * kDirectoryEntrySize;
    const std::uint64_t strtab_offset = dir_offset + dir_size;
    const std::uint64_t strtab_size = strings.bytes().size();
    const std::uint64_t chunk_region_offset = strtab_offset + strtab_size;

    std::vector<std::uint64_t> chunk_offsets(entries.size());
    std::uint64_t running = chunk_region_offset;
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        chunk_offsets[i] = running;
        running += entries[i].chunk.size();
    }
    const std::uint64_t chunk_region_size = running - chunk_region_offset;

    std::string out;
    out.reserve(static_cast<std::size_t>(chunk_region_offset + chunk_region_size));

    // --- Pack header (docs/chunk-pack-format.md §4.2), 88 bytes ---------------------------------
    out.append(kMagic, sizeof(kMagic));
    put_u32(out, kFormatVersion);
    put_u32(out, kHeaderSize);
    put_u32(out, 0u); // header flags — reserved in v1
    put_u64(out, options.engine_version);
    put_u32(out, root_ref.first);
    put_u32(out, root_ref.second);
    put_u64(out, entries.size());
    put_u64(out, dir_offset);
    put_u64(out, dir_size);
    put_u64(out, strtab_offset);
    put_u64(out, strtab_size);
    put_u64(out, chunk_region_offset);
    put_u64(out, chunk_region_size);

    // --- Directory (docs/chunk-pack-format.md §4.3), 76 bytes per entry -------------------------
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const Entry& e = entries[i];
        put_u64(out, e.unit_id);
        put_u64(out, e.parent_unit);
        put_u32(out, e.flags);
        put_u32(out, e.codec);
        put_u32(out, e.platform);
        put_u64(out, e.entity_count);
        put_u32(out, e.source_off);
        put_u32(out, e.source_len);
        put_u64(out, chunk_offsets[i]);
        put_u64(out, e.chunk.size());  // chunkLength (stored)
        put_u64(out, e.chunk.size());  // uncompressedLength (== chunkLength for codec = store)
        put_u64(out, e.content_hash);
    }

    // --- String table + chunk region ------------------------------------------------------------
    out += strings.bytes();
    for (const Entry& e : entries)
        out += e.chunk;

    result.ok = true;
    result.bytes = std::move(out);
    return result;
}

} // namespace context::editor::pack
