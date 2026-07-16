// runtime-content-test_streaming_budget: the R-ASSET-003 memory-budget proof. A synthetic world whose
// total content far exceeds a configured memory budget is streamed by the StreamingScheduler so that
// the resident set NEVER exceeds the budget, while the nearest units to the observer are the ones kept
// resident. Uses the built-in brute-force proximity scan (no external index) — the proximity test
// covers the spatial-index hook separately.

#include "context/runtime/content/content_loader.h"
#include "context/runtime/content/pack_source.h"
#include "context/runtime/content/streaming_scheduler.h"

#include "content_fixture.h"
#include "content_test.h"

#include "context/editor/compose/content_unit.h"
#include "context/editor/compose/flatten.h"

#include <string>
#include <vector>

namespace compose = context::editor::compose;
namespace content = context::runtime::content;

namespace
{

constexpr int kInstanceCount = 8;

// A root scene instancing the child scene N times — root unit + N instance units.
std::string make_root_json()
{
    std::string json = R"({"$schema": "ctx:scene", "version": 1,
      "entities": [{"id": "4444444444444444", "name": "Root", "components": {}}],
      "instances": [)";
    for (int i = 0; i < kInstanceCount; ++i)
    {
        // Distinct 16-hex instance ids: "aaaaaaaaaaaaaaaN".
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
      "entities": [
        {"id": "ccccccccccccccc1", "name": "Child1", "components": {"pos": {"x": 1, "y": 2, "z": 3}}},
        {"id": "ccccccccccccccc2", "name": "Child2", "components": {"vel": {"x": 4, "y": 5, "z": 6}}}
      ]})");
    CHECK(r.add("root.scene.json", make_root_json().c_str()));

    const compose::ComposedScene scene = compose::flatten("root.scene.json", r);
    CHECK(scene.ok);
    const compose::ContentUnitSet units = compose::partition_content_units(scene, r);
    CHECK(units.units.size() == static_cast<std::size_t>(kInstanceCount + 1)); // root + N instances

    const std::string bytes = content_fixture::make_pack(units, scene);
    CHECK(!bytes.empty());
    content::PackContentSource source(bytes);
    CHECK(source.ok());

    // The largest single-unit byte cost — a budget of 3x it means at most a handful of units fit, far
    // fewer than the whole world (an over-budget world).
    std::uint64_t max_unit_bytes = 0;
    std::uint64_t total_bytes = 0;
    for (const content::UnitDescriptor& d : source.directory())
    {
        total_bytes += d.resident_bytes;
        if (d.resident_bytes > max_unit_bytes)
            max_unit_bytes = d.resident_bytes;
    }
    const std::uint64_t budget = max_unit_bytes * 3;
    CHECK(total_bytes > budget); // the world genuinely does not fit

    content::RuntimeContentLoader loader(source);
    content::StreamingScheduler scheduler(loader);
    scheduler.set_memory_budget(budget);

    // Register every unit along a line: unit i (in ContentUnitSet order) at x = i * 10.
    std::vector<std::uint64_t> ids;
    for (std::size_t i = 0; i < units.units.size(); ++i)
    {
        const std::uint64_t id = units.units[i].identity_hash;
        ids.push_back(id);
        scheduler.register_unit(id, content::Vec3{static_cast<float>(i) * 10.0f, 0.0f, 0.0f});
    }
    CHECK(scheduler.registered_unit_count() == units.units.size());

    // --- observer at the near end: stream in; residency stays within budget -----------------------
    scheduler.update_observer(content::Vec3{0.0f, 0.0f, 0.0f}, /*radius=*/1000.0f);
    scheduler.pump();
    CHECK(loader.resident_bytes() <= budget);           // the hard ceiling holds
    CHECK(loader.resident_unit_count() >= 1);            // something streamed in
    CHECK(loader.resident_unit_count() < units.units.size()); // NOT everything — it is over budget
    CHECK(loader.is_resident(ids[0]));                   // the nearest unit is resident
    CHECK(!loader.is_resident(ids.back()));              // the farthest is not

    // --- observer moves to the far end: the working set streams over, still within budget ----------
    scheduler.update_observer(
        content::Vec3{static_cast<float>(kInstanceCount) * 10.0f, 0.0f, 0.0f}, 1000.0f);
    scheduler.pump();
    CHECK(loader.resident_bytes() <= budget);
    CHECK(loader.is_resident(ids.back()));  // the now-nearest unit streamed in
    CHECK(!loader.is_resident(ids[0]));     // the now-farthest unit streamed out

    // --- enforce_budget backstop: force ALL units resident, then reconcile to the budget ----------
    for (std::uint64_t id : ids)
        CHECK(loader.load_now(id));
    CHECK(loader.resident_bytes() > budget); // deliberately over budget
    const std::size_t evicted = scheduler.enforce_budget();
    CHECK(evicted > 0);
    CHECK(loader.resident_bytes() <= budget); // the ceiling is restored
    // The observer is at the far end, so the farthest (near-origin) units were evicted first.
    CHECK(loader.is_resident(ids.back()));
    CHECK(!loader.is_resident(ids[0]));

    // A zero (unlimited) budget never evicts.
    {
        content::RuntimeContentLoader loader2(source);
        content::StreamingScheduler unlimited(loader2);
        unlimited.set_memory_budget(0);
        for (std::uint64_t id : ids)
            CHECK(loader2.load_now(id));
        CHECK(unlimited.enforce_budget() == 0);
        CHECK(loader2.resident_unit_count() == units.units.size());
    }

    CONTENT_TEST_MAIN_END();
}
