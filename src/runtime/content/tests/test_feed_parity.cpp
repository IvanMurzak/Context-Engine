// runtime-content-test_feed_parity: the R-FILE-009 / L-24 one-seam-two-feeds proof. The SAME source
// scene, fed through the loading seam TWO independent ways —
//   - baked into a v1 pack and read back by PackContentSource (the shipped-build feed);
//   - handed live in memory from the flatten output via EditorContentSource (play-in-editor) —
// materializes an IDENTICAL derived world with an IDENTICAL state hash. This is what makes
// editor==build fidelity real (L-24: one data format, two feeds).

#include "context/runtime/content/content_loader.h"
#include "context/runtime/content/editor_source.h"
#include "context/runtime/content/pack_source.h"

#include "content_fixture.h"
#include "content_test.h"

#include "context/editor/compose/content_unit.h"
#include "context/editor/compose/flatten.h"

#include <algorithm>
#include <string>
#include <vector>

namespace compose = context::editor::compose;
namespace content = context::runtime::content;

namespace
{

// Load every unit the loader's source knows about (order = the caller's chosen id list).
void load_all(content::RuntimeContentLoader& loader, const std::vector<std::uint64_t>& ids)
{
    for (std::uint64_t id : ids)
        CHECK(loader.load_now(id));
}

std::vector<std::uint64_t> unit_ids(const compose::ContentUnitSet& units)
{
    std::vector<std::uint64_t> ids;
    for (const compose::ContentUnit& u : units.units)
        ids.push_back(u.identity_hash);
    return ids;
}

} // namespace

int main()
{
    // A non-trivial scene: root entity with a component + a nested instance chain + a second instance,
    // so the flatten produces multiple units with composed overrides (a meaningful derived world).
    content_fixture::MapResolver r;
    r.add("leaf.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [
        {"id": "1111111111111111", "name": "Leaf", "components": {"hp": {"value": 10}}},
        {"id": "2222222222222222", "name": "Leaf2", "components": {}}
      ]})");
    r.add("mid.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [],
      "instances": [{"id": "3333333333333333", "scene": "leaf.scene.json"}]})");
    r.add("root.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [{"id": "4444444444444444", "name": "Root", "components": {"tag": {"v": 7}}}],
      "instances": [
        {"id": "5555555555555555", "scene": "mid.scene.json"},
        {"id": "6666666666666666", "scene": "leaf.scene.json"}
      ]})");

    const compose::ComposedScene scene = compose::flatten("root.scene.json", r);
    CHECK(scene.ok);
    const compose::ContentUnitSet units = compose::partition_content_units(scene, r);
    CHECK(units.units.size() >= 3);

    const std::vector<std::uint64_t> ids = unit_ids(units);

    // --- feed A: the shipped-build pack ----------------------------------------------------------
    const std::string bytes = content_fixture::make_pack(units, scene);
    CHECK(!bytes.empty());
    content::PackContentSource pack_source(bytes);
    CHECK(pack_source.ok());
    content::RuntimeContentLoader pack_loader(pack_source);
    load_all(pack_loader, ids);

    // --- feed B: the live in-editor derived units ------------------------------------------------
    content::EditorContentSource editor_source(content_fixture::make_editor_units(units, scene));
    content::RuntimeContentLoader editor_loader(editor_source);
    // Load in a DELIBERATELY REVERSED order — the derived world must be order-independent.
    std::vector<std::uint64_t> reversed = ids;
    std::reverse(reversed.begin(), reversed.end());
    load_all(editor_loader, reversed);

    // --- identical derived world, identical state hash -------------------------------------------
    CHECK(pack_loader.resident_unit_count() == editor_loader.resident_unit_count());
    CHECK(pack_loader.resident_entity_count() == editor_loader.resident_entity_count());
    CHECK(pack_loader.resident_unit_ids() == editor_loader.resident_unit_ids());
    const std::uint64_t pack_hash = pack_loader.world_hash();
    const std::uint64_t editor_hash = editor_loader.world_hash();
    CHECK(pack_hash == editor_hash);
    CHECK(pack_hash != 0); // a non-trivial world (the hash actually folded content)

    // A per-unit spot check: the same GUID resolves to the same entity set + composed identities on
    // both feeds.
    for (std::uint64_t id : ids)
    {
        const content::LoadedUnit* a = pack_loader.resident_unit(id);
        const content::LoadedUnit* b = editor_loader.resident_unit(id);
        CHECK(a != nullptr && b != nullptr);
        CHECK(a->entities.size() == b->entities.size());
        for (std::size_t i = 0; i < a->entities.size(); ++i)
        {
            CHECK(a->entities[i].identity == b->entities[i].identity);
            CHECK(a->entities[i].id_path == b->entities[i].id_path);
        }
    }

    // Unloading a unit on ONE feed changes only that feed's world; reloading restores parity — proof
    // the hash tracks residency, not feed identity.
    editor_loader.unload(ids.front());
    CHECK(editor_loader.world_hash() != pack_hash);
    CHECK(editor_loader.load_now(ids.front()));
    CHECK(editor_loader.world_hash() == pack_hash);

    CONTENT_TEST_MAIN_END();
}
