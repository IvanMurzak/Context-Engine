// runtime-content-test_streaming_proximity: proves the R-ASSET-003 proximity-driven streaming decision
// HOOKS the R-SIM-007 broad-phase spatial index (R-SIM-007). Content units sit in two spatial
// clusters; a SpatialProximityProvider backed by a REAL context_spatial::SpatialIndex answers "which
// units are near the observer", and the StreamingScheduler loads the near cluster + streams out the
// far one as the observer moves — the SAME index that feeds render culling + spatial queries driving
// asset streaming.

#include "context/runtime/content/content_loader.h"
#include "context/runtime/content/pack_source.h"
#include "context/runtime/content/streaming_scheduler.h"

#include "content_fixture.h"
#include "content_test.h"
#include "spatial_provider.h"

#include "context/editor/compose/content_unit.h"
#include "context/editor/compose/flatten.h"

#include <string>
#include <vector>

namespace compose = context::editor::compose;
namespace content = context::runtime::content;

namespace
{

constexpr int kInstanceCount = 6;

std::string make_root_json()
{
    std::string json = R"({"$schema": "ctx:scene", "version": 1,
      "entities": [{"id": "4444444444444444", "name": "Root", "components": {}}],
      "instances": [)";
    for (int i = 0; i < kInstanceCount; ++i)
    {
        std::string id = "aaaaaaaaaaaaaaa";
        id += static_cast<char>('1' + i);
        json += "{\"id\": \"" + id + "\", \"scene\": \"child.scene.json\"}";
        if (i + 1 < kInstanceCount)
            json += ", ";
    }
    json += "]}";
    return json;
}

} // namespace

int main()
{
    content_fixture::MapResolver r;
    r.add("child.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [{"id": "ccccccccccccccc1", "name": "Child", "components": {"pos": {"x": 1}}}]})");
    CHECK(r.add("root.scene.json", make_root_json().c_str()));

    const compose::ComposedScene scene = compose::flatten("root.scene.json", r);
    CHECK(scene.ok);
    const compose::ContentUnitSet units = compose::partition_content_units(scene, r);
    CHECK(units.units.size() == static_cast<std::size_t>(kInstanceCount + 1));

    const std::string bytes = content_fixture::make_pack(units, scene);
    content::PackContentSource source(bytes);
    CHECK(source.ok());

    // Two spatial clusters: instances 0..2 near the origin (cluster A), instances 3..5 far away at
    // x~100 (cluster B); the root unit is parked far from both. units.units order is root, then the
    // instances in authored order.
    std::vector<content::Vec3> positions(units.units.size());
    positions[0] = content::Vec3{1000.0f, 0.0f, 0.0f};       // root — out of every query
    positions[1] = content::Vec3{0.0f, 0.0f, 0.0f};          // cluster A
    positions[2] = content::Vec3{1.0f, 0.0f, 0.0f};          // cluster A
    positions[3] = content::Vec3{2.0f, 0.0f, 0.0f};          // cluster A
    positions[4] = content::Vec3{100.0f, 0.0f, 0.0f};        // cluster B
    positions[5] = content::Vec3{101.0f, 0.0f, 0.0f};        // cluster B
    positions[6] = content::Vec3{102.0f, 0.0f, 0.0f};        // cluster B

    std::vector<std::uint64_t> ids;
    content_fixture::SpatialProximityProvider provider; // wraps a real context_spatial::SpatialIndex
    content::RuntimeContentLoader loader(source);
    content::StreamingScheduler scheduler(loader);
    scheduler.set_proximity_provider(&provider);
    for (std::size_t i = 0; i < units.units.size(); ++i)
    {
        const std::uint64_t id = units.units[i].identity_hash;
        ids.push_back(id);
        provider.add_unit(id, positions[i]);
        scheduler.register_unit(id, positions[i]);
    }
    // The spatial index really holds every unit (broad-phase leaves).
    CHECK(provider.index().size() == units.units.size());

    // --- observer inside cluster A: only cluster A streams in -------------------------------------
    scheduler.update_observer(content::Vec3{1.0f, 0.0f, 0.0f}, /*radius=*/10.0f);
    scheduler.pump();
    CHECK(loader.is_resident(ids[1]));
    CHECK(loader.is_resident(ids[2]));
    CHECK(loader.is_resident(ids[3]));
    CHECK(!loader.is_resident(ids[4])); // cluster B is out of range
    CHECK(!loader.is_resident(ids[5]));
    CHECK(!loader.is_resident(ids[6]));
    CHECK(!loader.is_resident(ids[0])); // the parked root is out of range
    CHECK(loader.resident_unit_count() == 3);

    // --- observer moves into cluster B: A streams out, B streams in -------------------------------
    scheduler.update_observer(content::Vec3{101.0f, 0.0f, 0.0f}, 10.0f);
    scheduler.pump();
    CHECK(!loader.is_resident(ids[1]));
    CHECK(!loader.is_resident(ids[2]));
    CHECK(!loader.is_resident(ids[3]));
    CHECK(loader.is_resident(ids[4]));
    CHECK(loader.is_resident(ids[5]));
    CHECK(loader.is_resident(ids[6]));
    CHECK(loader.resident_unit_count() == 3);

    // --- observer between clusters (nothing in range): everything streams out ---------------------
    scheduler.update_observer(content::Vec3{50.0f, 0.0f, 0.0f}, 10.0f);
    scheduler.pump();
    CHECK(loader.resident_unit_count() == 0);

    CONTENT_TEST_MAIN_END();
}
