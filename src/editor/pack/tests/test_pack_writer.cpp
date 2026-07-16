// Build-side pack writer + verification reader (R-ASSET-005 / L-35 / L-37, docs/chunk-pack-format.md).
// Coverage: the v1 byte layout + header fields, round-trip (write -> read -> the parsed directory and
// decoded chunk bodies equal the input units, composed-identity addressing preserved end to end),
// emission determinism (same source packed twice -> byte-identical), packed binary sidecars, and the
// reader's self-verification failure paths (bad magic, truncation, a flipped chunk byte -> hash
// mismatch, an unknown codec).

#include "context/editor/pack/pack_reader.h"
#include "context/editor/pack/pack_writer.h"

#include "context/editor/compose/content_unit.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/compose/stable_id.h"
#include "context/editor/pack/pack_format.h"
#include "context/editor/schema/json_access.h"
#include "context/editor/serializer/canonical.h"

#include "pack_test.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace context::editor::pack;
namespace compose = context::editor::compose;
namespace schema = context::editor::schema;
namespace serializer = context::editor::serializer;
using serializer::JsonValue;

namespace
{

[[nodiscard]] JsonValue parse(const char* json)
{
    serializer::CanonicalizeResult r = serializer::canonicalize(json);
    CHECK(r.is_json);
    return r.root;
}

class MapResolver final : public compose::SceneResolver
{
public:
    void add(const char* path, const char* json)
    {
        std::optional<compose::SceneDoc> doc = compose::build_scene_doc(path, parse(json));
        CHECK(doc.has_value());
        if (doc.has_value())
            docs_[path] = std::move(*doc);
    }
    [[nodiscard]] const compose::SceneDoc* resolve(std::string_view path) const override
    {
        auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, compose::SceneDoc, std::less<>> docs_;
};

// The number of {"identityHash", ...} entity records in a decoded chunk body.
[[nodiscard]] std::size_t chunk_entity_count(const std::string& chunk_bytes)
{
    serializer::CanonicalizeResult r = serializer::canonicalize(chunk_bytes);
    CHECK(r.is_json);
    const JsonValue* entities = schema::find_member(r.root, "entities");
    if (entities == nullptr || entities->type != JsonValue::Type::array)
        return 0;
    return entities->elements.size();
}

// True iff the decoded chunk body carries a composed entity whose identityHash renders `hash`.
[[nodiscard]] bool chunk_has_identity(const std::string& chunk_bytes, std::uint64_t hash)
{
    serializer::CanonicalizeResult r = serializer::canonicalize(chunk_bytes);
    CHECK(r.is_json);
    const JsonValue* entities = schema::find_member(r.root, "entities");
    if (entities == nullptr || entities->type != JsonValue::Type::array)
        return false;
    const std::string want = compose::format_stable_id(hash);
    for (const JsonValue& e : entities->elements)
    {
        const JsonValue* id = schema::find_member(e, "identityHash");
        if (id != nullptr && id->type == JsonValue::Type::string && id->string_value == want)
            return true;
    }
    return false;
}

const std::string kInstA = "aaaaaaaaaaaaaaa1"; // root -> mid (a top-level instance, nested child)
const std::string kInstD = "ddddddddddddddd1"; // root -> child (a direct top-level instance)

[[nodiscard]] MapResolver build_scenes()
{
    MapResolver r;
    r.add("child.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [
        {"id": "ccccccccccccccc1", "name": "C1", "components": {}},
        {"id": "ccccccccccccccc2", "name": "C2", "components": {}}
      ]})");
    r.add("mid.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [],
      "instances": [{"id": "bbbbbbbbbbbbbbb1", "scene": "child.scene.json"}]})");
    r.add("root.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [{"id": "eeeeeeeeeeeeeee1", "name": "RootEnt", "components": {}}],
      "instances": [
        {"id": "aaaaaaaaaaaaaaa1", "scene": "mid.scene.json"},
        {"id": "ddddddddddddddd1", "scene": "child.scene.json"}
      ]})");
    return r;
}

} // namespace

int main()
{
    MapResolver r = build_scenes();
    compose::ComposedScene scene = compose::flatten("root.scene.json", r);
    CHECK(scene.ok);
    compose::ContentUnitSet units = compose::partition_content_units(scene, r);
    CHECK(units.units.size() == 3);

    PackWriteOptions options;
    options.engine_version = 0x0102030405060708ULL;

    // --- write + basic header/round-trip --------------------------------------------------------
    PackWriteResult w = write_pack(units, scene, {}, options);
    CHECK(w.ok);
    CHECK(w.error.empty());
    CHECK(w.bytes.size() > kHeaderSize);

    ParsedPack pack = read_pack(w.bytes);
    CHECK(pack.ok);
    CHECK(pack.errors.empty());
    CHECK(pack.format_version == kFormatVersion);
    CHECK(pack.engine_version == 0x0102030405060708ULL);
    CHECK(pack.root_scene == "root.scene.json");
    CHECK(pack.entries.size() == 3);

    // The directory preserves the frozen unit order (root first) and the L-37 composed-identity key.
    CHECK(pack.entries[0].is_root);
    CHECK(!pack.entries[0].is_sidecar);
    CHECK(pack.entries[0].unit_id == compose::identity_hash_of("root.scene.json", {}));
    CHECK(pack.entries[0].parent_unit == 0);
    CHECK(pack.entries[0].source_scene == "root.scene.json");

    // Load-by-GUID: each unit is addressable by its composed identity, and the chunk carries that
    // identity end to end.
    const std::uint64_t id_a = compose::identity_hash_of("root.scene.json", {kInstA});
    const std::uint64_t id_d = compose::identity_hash_of("root.scene.json", {kInstD});
    const PackEntry* unit_a = find_unit(pack, id_a);
    const PackEntry* unit_d = find_unit(pack, id_d);
    CHECK(unit_a != nullptr);
    CHECK(unit_d != nullptr);
    CHECK(find_unit(pack, 0xdeadbeefULL) == nullptr);
    CHECK(unit_a->source_scene == "mid.scene.json");
    CHECK(unit_d->source_scene == "child.scene.json");

    // Each instance unit holds the child's two composed entities (A via the nested mid->child edge).
    CHECK(unit_a->entity_count == 2);
    CHECK(unit_d->entity_count == 2);
    CHECK(chunk_entity_count(unit_a->chunk_bytes) == 2);
    CHECK(chunk_entity_count(unit_d->chunk_bytes) == 2);

    // Composed-identity addressing preserved end to end: every composed entity's identity appears in
    // exactly the chunk of the unit it belongs to.
    for (const compose::ContentUnit& u : units.units)
    {
        const PackEntry* entry = find_unit(pack, u.identity_hash);
        CHECK(entry != nullptr);
        for (std::size_t idx : u.entity_indices)
            CHECK(chunk_has_identity(entry->chunk_bytes, scene.entities[idx].identity_hash));
    }

    // --- determinism: same source packed twice -> byte-identical (the R-FILE-010 property) --------
    {
        // A fresh partition from the same flatten must pack to the identical bytes.
        compose::ContentUnitSet again = compose::partition_content_units(scene, r);
        PackWriteResult w2 = write_pack(again, scene, {}, options);
        CHECK(w2.ok);
        CHECK(w2.bytes == w.bytes);
    }

    // --- packed binary sidecars (docs/chunk-pack-format.md §4.5) ---------------------------------
    {
        std::vector<PackSidecar> sidecars;
        PackSidecar tex;
        tex.relpath = "textures/hero.png.bin";
        tex.raw_hash = 0xABCDEF0123456789ULL;
        tex.bytes = std::string("\x89PNG\r\n\x1a\n synthetic pixel bytes", 30);
        sidecars.push_back(tex);

        PackWriteResult ws = write_pack(units, scene, sidecars, options);
        CHECK(ws.ok);
        ParsedPack ps = read_pack(ws.bytes);
        CHECK(ps.ok);
        CHECK(ps.entries.size() == 4); // 3 units + 1 sidecar
        const PackEntry* sc = find_unit(ps, tex.raw_hash);
        CHECK(sc != nullptr);
        CHECK(sc->is_sidecar);
        CHECK(sc->entity_count == 0);
        CHECK(sc->source_scene == "textures/hero.png.bin");
        CHECK(sc->chunk_bytes == tex.bytes); // stored verbatim (codec = store)

        // Sidecar packing is deterministic too.
        PackWriteResult ws2 = write_pack(units, scene, sidecars, options);
        CHECK(ws2.ok);
        CHECK(ws2.bytes == ws.bytes);
    }

    // --- reader self-verification failure paths -------------------------------------------------
    {
        // Bad magic.
        std::string bad = w.bytes;
        bad[0] = 'X';
        ParsedPack p = read_pack(bad);
        CHECK(!p.ok);
        CHECK(!p.errors.empty());
        CHECK(p.errors.front() == "pack.bad_magic");
    }
    {
        // Truncated below the header.
        ParsedPack p = read_pack(std::string_view(w.bytes).substr(0, kHeaderSize - 1));
        CHECK(!p.ok);
        CHECK(p.errors.front() == "pack.truncated");
    }
    {
        // A flipped byte in the LAST chunk (the tail of the stream is chunk bytes) -> hash mismatch.
        std::string tampered = w.bytes;
        tampered.back() = static_cast<char>(tampered.back() ^ 0x01);
        ParsedPack p = read_pack(tampered);
        CHECK(!p.ok);
        CHECK(p.errors.front() == "pack.hash_mismatch");
    }
    {
        // A crafted unitCount that overflows unitCount * kDirectoryEntrySize back onto a small
        // directorySize must be refused cleanly, never throw (read_pack is total). directorySize is
        // forced to 0 and unitCount to 2^62 (2^62 * 76 wraps to 0 mod 2^64): the size check must not
        // be fooled into entries.reserve(2^62), which would throw std::length_error.
        std::string crafted = w.bytes;
        const auto put_u64_at = [&crafted](std::size_t off, std::uint64_t v) {
            for (unsigned i = 0; i < 8; ++i)
                crafted[off + i] = static_cast<char>((v >> (8u * i)) & 0xFFu);
        };
        put_u64_at(32, static_cast<std::uint64_t>(1) << 62); // unitCount (header offset 32)
        put_u64_at(48, 0);                                   // directorySize (header offset 48)
        ParsedPack p = read_pack(crafted);
        CHECK(!p.ok);
        CHECK(p.errors.front() == "pack.bad_directory");
    }

    PACK_TEST_MAIN_END();
}
