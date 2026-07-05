// M2 exit criterion 3 — flatten a composed scene into GUID-addressable content units the co-designed
// chunk boundaries can load/unload INDEPENDENTLY (ROADMAP §1 M2 Exit / issue #68; R-ASSET-005 / L-35).
// Authored scenes on real disk are re-derived through the file-backed resolver, flattened, and
// partitioned into content units; the exit gate proves the three co-design properties end-to-end:
//   1. boundary partition — the root scene's own content is the root unit; each top-level instance
//      subtree is its own unit (deterministic order: root first, then instances in authored order);
//   2. GUID-addressability — every unit id is the L-37 composed identity of its boundary root, so a
//      chunk is addressed by a stable GUID (identity_hash_of(rootScene, [segment])); and
//   3. independent load/unload — a residency model proves unloading one unit leaves every sibling's
//      resident entity set EXACTLY as it was (the isolation property the M8 streaming loader needs).
//
// R-QA-013: happy path (partition + GUID addressing + isolation) + edge (emission determinism —
// same flatten yields a byte-identical manifest) + failure-path control (an unknown unit id cannot
// load; unloading a not-resident unit is a no-op false).

#include "context/editor/compose/content_unit.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/stable_id.h"

#include "m2_exit_test.h"

#include <cstdint>
#include <string>
#include <vector>

namespace compose = context::editor::compose;
namespace fs = std::filesystem;

namespace
{

const std::string kInstA = "aaaaaaaaaaaaaaa1"; // root -> mid (top-level instance)
const std::string kInstD = "ddddddddddddddd1"; // root -> child (a second top-level instance)
const std::string kEntR1 = "eeeeeeeeeeeeeee1"; // root's own entity

void seed_scenes(const fs::path& dir)
{
    m2exit::write_file_raw(dir / "child.scene.json", R"({
  "$schema": "ctx:scene", "version": 1,
  "entities": [
    {"id": "ccccccccccccccc1", "name": "C1", "components": {}},
    {"id": "ccccccccccccccc2", "name": "C2", "components": {}}
  ]})");
    m2exit::write_file_raw(dir / "mid.scene.json", R"({
  "$schema": "ctx:scene", "version": 1, "entities": [],
  "instances": [{"id": "bbbbbbbbbbbbbbb1", "scene": "child.scene.json"}]})");
    m2exit::write_file_raw(dir / "root.scene.json", R"({
  "$schema": "ctx:scene", "version": 1,
  "entities": [{"id": "eeeeeeeeeeeeeee1", "name": "RootEnt", "components": {}}],
  "instances": [
    {"id": "aaaaaaaaaaaaaaa1", "scene": "mid.scene.json"},
    {"id": "ddddddddddddddd1", "scene": "child.scene.json"}
  ]})");
}

const compose::ContentUnit* unit_with_segment(const compose::ContentUnitSet& set,
                                              const std::string& segment)
{
    for (const compose::ContentUnit& u : set.units)
        if (u.boundary_segment == segment)
            return &u;
    return nullptr;
}

} // namespace

int main()
{
    const fs::path project = m2exit::make_temp_project("units");
    seed_scenes(project);

    m2exit::FileSceneResolver resolver;
    CHECK(resolver.load(project) == 3);

    compose::ComposedScene scene = compose::flatten("root.scene.json", resolver);
    CHECK(scene.ok);
    CHECK(scene.diagnostics.empty());
    // root entity + child's two entities (via mid->child) + child's two entities (direct) = 5.
    CHECK(scene.entities.size() == 5);

    compose::ContentUnitSet units = compose::partition_content_units(scene, resolver);

    // --- 1. boundary partition: root unit first, then top-level instances in authored order --------
    CHECK(units.units.size() == 3);
    CHECK(units.units[0].is_root);
    CHECK(units.units[0].boundary_segment.empty());
    CHECK(units.units[1].boundary_segment == kInstA);
    CHECK(units.units[2].boundary_segment == kInstD);

    const compose::ContentUnit* root_unit = &units.units[0];
    const compose::ContentUnit* unit_a = unit_with_segment(units, kInstA);
    const compose::ContentUnit* unit_d = unit_with_segment(units, kInstD);
    CHECK(unit_a != nullptr && unit_d != nullptr);

    // Membership: the root unit holds only R1; each instance unit holds the child's two entities.
    CHECK(root_unit->entity_indices.size() == 1);
    CHECK(scene.entities[root_unit->entity_indices[0]].id_path == std::vector<std::string>{kEntR1});
    CHECK(unit_a->entity_indices.size() == 2);
    CHECK(unit_d->entity_indices.size() == 2);

    // --- 2. GUID-addressability: unit ids ARE the composed identity of the boundary root -----------
    CHECK(root_unit->identity_hash == compose::identity_hash_of("root.scene.json", {}));
    CHECK(unit_a->identity_hash == compose::identity_hash_of("root.scene.json", {kInstA}));
    CHECK(unit_d->identity_hash == compose::identity_hash_of("root.scene.json", {kInstD}));
    CHECK(unit_a->unit_id == compose::format_stable_id(unit_a->identity_hash));
    CHECK(root_unit->unit_id != unit_a->unit_id);
    CHECK(unit_a->unit_id != unit_d->unit_id);
    CHECK(root_unit->unit_id != unit_d->unit_id);

    // --- edge: emission determinism — same flatten yields a byte-identical manifest ---------------
    {
        compose::ContentUnitSet again = compose::partition_content_units(scene, resolver);
        const std::string a = compose::content_units_json(units, scene);
        const std::string b = compose::content_units_json(again, scene);
        CHECK(a == b);
        CHECK(a.find("\"unitCount\": 3") != std::string::npos);
        CHECK(a.find("\"unitId\"") != std::string::npos);
    }

    // --- 3. independent load / unload isolation (the R-ASSET-005 boundary property) ----------------
    {
        compose::ContentUnitResidency residency(units, scene);
        // failure-path control: an unknown id cannot load; unloading a not-resident unit is a no-op.
        CHECK(!residency.load("0000000000000000"));
        CHECK(!residency.unload(unit_a->unit_id));

        CHECK(residency.load(unit_a->unit_id));
        CHECK(residency.load(unit_d->unit_id));
        CHECK(residency.resident_unit_count() == 2);
        CHECK(residency.resident_entity_count() == 4);

        // D's fingerprint in isolation, for the isolation compare.
        compose::ContentUnitResidency d_only(units, scene);
        CHECK(d_only.load(unit_d->unit_id));
        const std::vector<std::uint64_t> d_ids = d_only.resident_identities();
        CHECK(d_ids.size() == 2);

        // Unloading A leaves D EXACTLY as it was — the isolation property that lets chunks stream
        // independently.
        CHECK(residency.unload(unit_a->unit_id));
        CHECK(!residency.is_resident(unit_a->unit_id));
        CHECK(residency.is_resident(unit_d->unit_id));
        CHECK(residency.resident_unit_count() == 1);
        CHECK(residency.resident_entity_count() == 2);
        CHECK(residency.resident_identities() == d_ids);
    }

    m2exit::remove_quiet(project);
    M2_EXIT_MAIN_END();
}
