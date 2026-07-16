// runtime-content-test_pack_source: load / instantiate / unload of packed content units by GUID from a
// real v1 pack (R-ASSET-005), plus the self-verifying failure paths. Proves the pack-fed leg of the
// loading seam materializes the frozen identity + id-path + payload triple by GUID and refuses a
// corrupt/unknown chunk.

#include "context/runtime/content/content_loader.h"
#include "context/runtime/content/pack_source.h"

#include "content_fixture.h"
#include "content_test.h"

#include "context/editor/compose/content_unit.h"
#include "context/editor/compose/flatten.h"

#include <string>
#include <vector>

namespace compose = context::editor::compose;
namespace content = context::runtime::content;
namespace pack = context::editor::pack;

namespace
{

// A scene: root entity + two top-level instances of a child scene, plus one packed sidecar.
content_fixture::MapResolver make_resolver()
{
    content_fixture::MapResolver r;
    r.add("child.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [
        {"id": "ccccccccccccccc1", "name": "C1", "components": {"pos": {"x": 1, "y": 2}}},
        {"id": "ccccccccccccccc2", "name": "C2", "components": {}}
      ]})");
    r.add("root.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [{"id": "eeeeeeeeeeeeeee1", "name": "RootEnt", "components": {}}],
      "instances": [
        {"id": "aaaaaaaaaaaaaaa1", "scene": "child.scene.json"},
        {"id": "ddddddddddddddd1", "scene": "child.scene.json"}
      ]})");
    return r;
}

} // namespace

int main()
{
    content_fixture::MapResolver r = make_resolver();
    const compose::ComposedScene scene = compose::flatten("root.scene.json", r);
    CHECK(scene.ok);
    const compose::ContentUnitSet units = compose::partition_content_units(scene, r);
    CHECK(units.units.size() == 3); // root + 2 instances

    // One sidecar (L-33 binary blob) packed as a first-class chunk, keyed by its raw-byte hash.
    std::vector<pack::PackSidecar> sidecars;
    pack::PackSidecar sc;
    sc.relpath = "textures/rock.png";
    sc.raw_hash = 0x1234abcd5678ef01ULL;
    sc.bytes = "\x89PNG\r\n\x1a\n-fake-sidecar-payload";
    sidecars.push_back(sc);

    const std::string bytes = content_fixture::make_pack(units, scene, sidecars);
    CHECK(!bytes.empty());

    // --- the pack parses into an O(1) directory WITHOUT decoding chunk bodies ---------------------
    content::PackContentSource source(bytes);
    CHECK(source.ok());
    CHECK(source.error().empty());
    CHECK(source.engine_version() == 1);
    CHECK(source.root_scene() == "root.scene.json");
    CHECK(source.directory().size() == 4); // 3 units + 1 sidecar

    content::RuntimeContentLoader loader(source);
    CHECK(loader.known_unit_count() == 4);

    // --- load / instantiate a unit BY GUID --------------------------------------------------------
    const std::uint64_t root_id = units.units[0].identity_hash;
    const std::uint64_t inst_a_id = units.units[1].identity_hash;
    const std::uint64_t inst_d_id = units.units[2].identity_hash;
    CHECK(source.contains(root_id));
    CHECK(source.contains(inst_a_id));
    CHECK(!source.contains(0xdeadbeefULL));

    CHECK(loader.load_now(inst_a_id));
    CHECK(loader.is_resident(inst_a_id));
    const content::LoadedUnit* unit_a = loader.resident_unit(inst_a_id);
    CHECK(unit_a != nullptr);
    CHECK(unit_a->unit_id == inst_a_id);
    CHECK(!unit_a->is_sidecar);
    CHECK(unit_a->entities.size() == 2); // the child scene's two entities, composed under instance A
    // Each entity carries its composed identity + id-path rooted at the instance segment.
    for (const content::UnitEntity& e : unit_a->entities)
    {
        CHECK(!e.id_path.empty());
        CHECK(e.id_path.front() == "aaaaaaaaaaaaaaa1");
        CHECK(e.identity != 0);
        CHECK(e.value.type == context::editor::serializer::JsonValue::Type::object);
    }
    CHECK(loader.resident_bytes() > 0);

    // --- load a SIDECAR by GUID (its declared raw-byte hash) --------------------------------------
    CHECK(loader.load_now(sc.raw_hash));
    const content::LoadedUnit* side = loader.resident_unit(sc.raw_hash);
    CHECK(side != nullptr);
    CHECK(side->is_sidecar);
    CHECK(side->entities.empty());
    CHECK(side->sidecar_bytes == sc.bytes); // stored verbatim, loaded verbatim by GUID

    // --- unload by GUID releases exactly that unit's residency (isolation) ------------------------
    CHECK(loader.load_now(inst_d_id));
    const std::uint64_t bytes_before = loader.resident_bytes();
    const std::size_t entities_before = loader.resident_entity_count();
    // Capture unit_a's entity count BEFORE unload — unload() erases its Resident node, so unit_a
    // (a pointer into residency taken above) would otherwise be read after being freed.
    const std::size_t unit_a_entity_count = unit_a->entities.size();
    CHECK(loader.unload(inst_a_id));
    CHECK(!loader.is_resident(inst_a_id));
    CHECK(loader.is_resident(inst_d_id));    // the sibling is untouched
    CHECK(loader.is_resident(sc.raw_hash));  // the sidecar is untouched
    CHECK(loader.resident_bytes() < bytes_before);
    CHECK(loader.resident_entity_count() == entities_before - unit_a_entity_count);
    // resident_unit points into residency — it is now dangling for A; re-query proves the drop.
    CHECK(loader.resident_unit(inst_a_id) == nullptr);

    // Unloading an absent unit is a no-op false.
    CHECK(!loader.unload(inst_a_id));
    CHECK(!loader.unload(0xdeadbeefULL));

    // --- failure paths: unknown GUID + corrupt/malformed pack -------------------------------------
    {
        std::string err;
        content::LoadedUnit tmp;
        CHECK(!source.load(0xdeadbeefULL, tmp, &err));
        CHECK(err == "content.unknown_unit");
    }
    {
        // A bad-magic stream refuses to parse (never a partial/garbage directory).
        content::PackContentSource bad(std::string("NOPE") + std::string(200, '\0'));
        CHECK(!bad.ok());
        CHECK(bad.error() == "pack.bad_magic");
        CHECK(bad.directory().empty());
    }
    {
        // A truncated stream refuses to parse.
        content::PackContentSource trunc(bytes.substr(0, 40));
        CHECK(!trunc.ok());
    }
    {
        // A flipped byte in the LAST chunk is caught on read by the self-verifying content hash: the
        // directory still parses (offsets/lengths intact) but load() of the corrupted unit fails.
        std::string corrupt = bytes;
        corrupt.back() ^= 0xFF;
        content::PackContentSource cs(corrupt);
        CHECK(cs.ok()); // the directory + region bounds are unchanged
        // The last chunk in emission order is the sidecar (units first, sidecars after).
        std::string err;
        content::LoadedUnit tmp;
        CHECK(!cs.load(sc.raw_hash, tmp, &err));
        CHECK(err == "pack.hash_mismatch");
        // Other units still load fine — corruption is refused per-chunk, not pack-wide.
        content::LoadedUnit ok_unit;
        CHECK(cs.load(root_id, ok_unit, nullptr));
    }

    CONTENT_TEST_MAIN_END();
}
