// runtime-content-test_content_loader: the RuntimeContentLoader residency + async + handle-invalidation
// surface (R-ASSET-005 / L-22 / L-24). Uses the EditorContentSource feed (mutable, so a re-derive can
// be simulated). Proves: async request/pump load/unload, residency isolation, unload provably releases
// residency, and the handle-invalidation path shared with hot reload.

#include "context/runtime/content/content_loader.h"
#include "context/runtime/content/editor_source.h"

#include "content_fixture.h"
#include "content_test.h"

#include "context/editor/compose/content_unit.h"
#include "context/editor/compose/flatten.h"

#include <vector>

namespace compose = context::editor::compose;
namespace content = context::runtime::content;
namespace serializer = context::editor::serializer;

namespace
{

serializer::JsonValue make_number(std::int64_t n)
{
    serializer::JsonValue v;
    v.type = serializer::JsonValue::Type::integer;
    v.int_value = n;
    return v;
}

serializer::JsonValue make_object_with(const char* key, serializer::JsonValue value)
{
    serializer::JsonValue obj;
    obj.type = serializer::JsonValue::Type::object;
    serializer::JsonMember m;
    m.key = key;
    m.value = std::move(value);
    obj.members.push_back(std::move(m));
    return obj;
}

} // namespace

int main()
{
    content_fixture::MapResolver r;
    r.add("child.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [
        {"id": "ccccccccccccccc1", "name": "C1", "components": {}},
        {"id": "ccccccccccccccc2", "name": "C2", "components": {}}
      ]})");
    r.add("root.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [{"id": "eeeeeeeeeeeeeee1", "name": "RootEnt", "components": {}}],
      "instances": [
        {"id": "aaaaaaaaaaaaaaa1", "scene": "child.scene.json"},
        {"id": "ddddddddddddddd1", "scene": "child.scene.json"}
      ]})");
    const compose::ComposedScene scene = compose::flatten("root.scene.json", r);
    CHECK(scene.ok);
    const compose::ContentUnitSet units = compose::partition_content_units(scene, r);
    CHECK(units.units.size() == 3);

    content::EditorContentSource source(content_fixture::make_editor_units(units, scene));
    CHECK(source.directory().size() == 3);

    const std::uint64_t root_id = units.units[0].identity_hash;
    const std::uint64_t a_id = units.units[1].identity_hash;
    const std::uint64_t d_id = units.units[2].identity_hash;

    // --- async request + bounded pump -------------------------------------------------------------
    {
        content::RuntimeContentLoader loader(source);
        loader.request_load(a_id);
        loader.request_load(d_id);
        loader.request_load(root_id);
        CHECK(loader.pending_requests() == 3);
        CHECK(loader.resident_unit_count() == 0); // nothing materializes until pumped

        CHECK(loader.pump(1) == 1); // bounded: services exactly one request
        CHECK(loader.resident_unit_count() == 1);
        CHECK(loader.pending_requests() == 2);
        CHECK(loader.pump() == 2); // drains the rest
        CHECK(loader.resident_unit_count() == 3);
        CHECK(loader.pending_requests() == 0);

        // A request for a unit unknown to the source is dropped (never enqueued).
        loader.request_load(0xdeadbeefULL);
        CHECK(loader.pending_requests() == 0);
    }

    // --- residency isolation + unload releases residency ------------------------------------------
    {
        content::RuntimeContentLoader loader(source);
        CHECK(loader.load_now(a_id));
        CHECK(loader.load_now(d_id));
        CHECK(loader.resident_unit_count() == 2);
        const std::size_t entities_a = loader.resident_unit(a_id)->entities.size();
        const std::size_t entities_both = loader.resident_entity_count();
        const std::uint64_t bytes_both = loader.resident_bytes();
        const std::vector<std::uint64_t> resident_ids_before = loader.resident_unit_ids();
        CHECK(resident_ids_before.size() == 2);

        // Fingerprint D's residency, unload A, assert D is byte-for-byte unaffected (isolation).
        const std::uint64_t d_hash_before = [&] {
            content::RuntimeContentLoader solo(source);
            solo.load_now(d_id);
            return solo.world_hash();
        }();
        CHECK(loader.unload(a_id));
        CHECK(loader.resident_unit_count() == 1);
        CHECK(!loader.is_resident(a_id));
        CHECK(loader.is_resident(d_id));
        // Unload provably released A's residency: bytes + entity count dropped by exactly A's share.
        CHECK(loader.resident_bytes() == bytes_both - content_fixture::dir_bytes(source, a_id));
        CHECK(loader.resident_entity_count() == entities_both - entities_a);
        // D alone now hashes identically to D-loaded-in-isolation — no cross-unit disturbance.
        CHECK(loader.world_hash() == d_hash_before);

        // Unloading everything drives residency + bytes to zero (fully released).
        CHECK(loader.unload(d_id));
        CHECK(loader.resident_unit_count() == 0);
        CHECK(loader.resident_bytes() == 0);
        CHECK(loader.resident_entity_count() == 0);
    }

    // --- handle invalidation shared with hot reload (L-22 / L-24) ---------------------------------
    {
        content::EditorContentSource live(content_fixture::make_editor_units(units, scene));
        content::RuntimeContentLoader loader(live);
        CHECK(loader.load_now(a_id));
        const content::Handle h = loader.handle_for(a_id);
        CHECK(h.valid);
        CHECK(loader.resolve(h) != nullptr);
        const std::uint64_t hash_before = loader.world_hash();

        // Simulate a re-derivation in the editor: A's content changes (a component value edit), then
        // the loader invalidates the resident unit — the hot-reload path.
        content::LoadedUnit rederived = *loader.resident_unit(a_id);
        CHECK(!rederived.entities.empty());
        rederived.entities.front().value =
            make_object_with("hp", make_number(999)); // an authored value edit
        live.replace_unit(rederived);

        CHECK(loader.invalidate(a_id));
        // The OLD handle is now stale — resolve returns null (generation bumped).
        CHECK(loader.resolve(h) == nullptr);
        // A fresh handle resolves to the re-derived content.
        const content::Handle h2 = loader.handle_for(a_id);
        CHECK(h2.valid);
        CHECK(h2.generation != h.generation);
        CHECK(loader.resolve(h2) != nullptr);
        CHECK(loader.world_hash() != hash_before); // the derived world reflects the re-derivation

        // Invalidating a non-resident unit fails cleanly.
        CHECK(!loader.invalidate(d_id));
    }

    CONTENT_TEST_MAIN_END();
}
