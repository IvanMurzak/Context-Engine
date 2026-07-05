// Content-unit boundary emission + the boundary-property proof (R-ASSET-005 / L-35): flatten emits
// GUID-addressable, independently loadable/unloadable content-unit boundaries co-designed with the
// chunked pack format (docs/chunk-pack-format.md). Coverage: the boundary partition (root unit +
// top-level instance units), GUID-addressability (unit ids = composed identity of the boundary
// root), emission determinism (same flatten -> byte-identical manifest), and the independent
// load/unload isolation property.

#include "context/editor/compose/content_unit.h"

#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/compose/stable_id.h"
#include "context/editor/serializer/canonical.h"

#include "compose_test.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace context::editor::compose;
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

class MapResolver final : public SceneResolver
{
public:
    void add(const char* path, const char* json)
    {
        std::optional<SceneDoc> doc = build_scene_doc(path, parse(json));
        CHECK(doc.has_value());
        docs_[path] = std::move(*doc);
    }
    [[nodiscard]] const SceneDoc* resolve(std::string_view path) const override
    {
        auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, SceneDoc, std::less<>> docs_;
};

[[nodiscard]] const ContentUnit* unit_with_segment(const ContentUnitSet& set,
                                                   std::string_view segment)
{
    for (const ContentUnit& u : set.units)
        if (u.boundary_segment == segment)
            return &u;
    return nullptr;
}

// Stable ids referenced from the C++ asserts (16 lowercase hex chars each). The nested mid->child
// instance id (bbbbbbbbbbbbbbb1) and child entity ids live only inside the scene JSON below.
const std::string kInstA = "aaaaaaaaaaaaaaa1"; // root -> mid (top-level instance)
const std::string kInstD = "ddddddddddddddd1"; // root -> child (a second top-level instance)
const std::string kEntR1 = "eeeeeeeeeeeeeee1"; // root's own entity

} // namespace

int main()
{
    // --- degenerate: a scene with no instances is ONE root unit --------------------------------
    {
        MapResolver r;
        r.add("solo.scene.json", R"({
          "$schema": "ctx:scene", "version": 1,
          "entities": [
            {"id": "ccccccccccccccc1", "name": "A", "components": {}},
            {"id": "ccccccccccccccc2", "name": "B", "components": {}}
          ]})");
        ComposedScene scene = flatten("solo.scene.json", r);
        CHECK(scene.ok);
        ContentUnitSet units = partition_content_units(scene, r);
        CHECK(units.units.size() == 1);
        CHECK(units.units[0].is_root);
        CHECK(units.units[0].boundary_segment.empty());
        CHECK(units.units[0].entity_indices.size() == 2);
        // The root unit is keyed by the composed identity of the scene root (an empty id-path).
        CHECK(units.units[0].identity_hash == identity_hash_of("solo.scene.json", {}));
        CHECK(units.units[0].unit_id == format_stable_id(units.units[0].identity_hash));
    }

    // --- an instance whose scene fails to resolve contributes NO unit (§2 boundary rule) ---------
    {
        MapResolver missing_r;
        // The root has its own entity (a non-empty root unit) plus ONE instance pointing at a scene
        // the resolver does not know. flatten suppresses the unresolvable instance best-effort, so it
        // composes no entities and the partition emits no unit for it (only non-empty units emit).
        missing_r.add("root.scene.json", R"({
          "$schema": "ctx:scene", "version": 1,
          "entities": [{"id": "eeeeeeeeeeeeeee1", "name": "RootEnt", "components": {}}],
          "instances": [{"id": "fffffffffffffff1", "scene": "missing.scene.json"}]})");
        ComposedScene missing_scene = flatten("root.scene.json", missing_r);
        ContentUnitSet missing_units = partition_content_units(missing_scene, missing_r);
        // Only the root unit is emitted; the failed-to-resolve instance produced no unit.
        CHECK(missing_units.units.size() == 1);
        CHECK(missing_units.units[0].is_root);
        CHECK(unit_with_segment(missing_units, "fffffffffffffff1") == nullptr);
    }

    // --- root unit + two top-level instance units ----------------------------------------------
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

    ComposedScene scene = flatten("root.scene.json", r);
    CHECK(scene.ok);
    ContentUnitSet units = partition_content_units(scene, r);

    // Three units, deterministic order: the root unit FIRST, then the top-level instances in
    // authored order (A before D).
    CHECK(units.units.size() == 3);
    CHECK(units.units[0].is_root);
    CHECK(units.units[0].boundary_segment.empty());
    CHECK(units.units[1].boundary_segment == kInstA);
    CHECK(units.units[2].boundary_segment == kInstD);

    // Membership: the root unit holds R1 only; each instance unit holds the child's two entities
    // (A via the nested mid->child instance, D via a direct child instance).
    const ContentUnit* root_unit = &units.units[0];
    const ContentUnit* unit_a = unit_with_segment(units, kInstA);
    const ContentUnit* unit_d = unit_with_segment(units, kInstD);
    CHECK(root_unit->entity_indices.size() == 1);
    CHECK(scene.entities[root_unit->entity_indices[0]].id_path == std::vector<std::string>{kEntR1});
    CHECK(unit_a != nullptr && unit_a->entity_indices.size() == 2);
    CHECK(unit_d != nullptr && unit_d->entity_indices.size() == 2);
    // Every entity in unit A is under the A id-path root; likewise D.
    for (std::size_t idx : unit_a->entity_indices)
        CHECK(scene.entities[idx].id_path.front() == kInstA);
    for (std::size_t idx : unit_d->entity_indices)
        CHECK(scene.entities[idx].id_path.front() == kInstD);

    // --- GUID-addressability: unit ids are the composed identity of the boundary root -----------
    CHECK(root_unit->identity_hash == identity_hash_of("root.scene.json", {}));
    CHECK(unit_a->identity_hash == identity_hash_of("root.scene.json", {kInstA}));
    CHECK(unit_d->identity_hash == identity_hash_of("root.scene.json", {kInstD}));
    CHECK(unit_a->unit_id == format_stable_id(unit_a->identity_hash));
    // Distinct units never share an id.
    CHECK(root_unit->unit_id != unit_a->unit_id);
    CHECK(unit_a->unit_id != unit_d->unit_id);
    CHECK(root_unit->unit_id != unit_d->unit_id);

    // The manifest carries the source scene reference (path in M2) and the entity count.
    CHECK(unit_a->source_scene == "mid.scene.json");
    CHECK(unit_d->source_scene == "child.scene.json");
    CHECK(root_unit->source_scene == "root.scene.json");

    // --- emission determinism: same flatten -> byte-identical manifest, twice -------------------
    {
        ContentUnitSet again = partition_content_units(scene, r);
        const std::string a = content_units_json(units, scene);
        const std::string b = content_units_json(again, scene);
        CHECK(a == b);
        CHECK(!a.empty());
        // The manifest names the frozen columns (docs/chunk-pack-format.md §3).
        CHECK(a.find("\"unitId\"") != std::string::npos);
        CHECK(a.find("\"parentUnit\"") != std::string::npos);
        CHECK(a.find("\"sourceScene\"") != std::string::npos);
        CHECK(a.find("\"unitCount\": 3") != std::string::npos);
    }

    // --- independent load / unload isolation (the R-ASSET-005 boundary property) -----------------
    {
        ContentUnitResidency residency(units, scene);
        CHECK(residency.is_known(unit_a->unit_id));
        CHECK(!residency.is_resident(unit_a->unit_id));

        // An unknown unit id cannot be loaded.
        CHECK(!residency.load("0000000000000000"));
        // Unloading a not-resident unit is a no-op false.
        CHECK(!residency.unload(root_unit->unit_id));

        CHECK(residency.load(unit_a->unit_id));
        CHECK(residency.load(unit_d->unit_id));
        CHECK(residency.resident_unit_count() == 2);
        CHECK(residency.resident_entity_count() == 4);
        const std::vector<std::uint64_t> both = residency.resident_identities();
        CHECK(both.size() == 4);

        // Loading is idempotent (no double-count).
        CHECK(residency.load(unit_a->unit_id));
        CHECK(residency.resident_unit_count() == 2);

        // D's residency fingerprint while both are resident, for the isolation compare below.
        ContentUnitResidency d_only(units, scene);
        CHECK(d_only.load(unit_d->unit_id));
        const std::vector<std::uint64_t> d_ids = d_only.resident_identities();
        CHECK(d_ids.size() == 2);

        // Unloading A leaves D EXACTLY as it was — the isolation property.
        CHECK(residency.unload(unit_a->unit_id));
        CHECK(!residency.is_resident(unit_a->unit_id));
        CHECK(residency.is_resident(unit_d->unit_id));
        CHECK(residency.resident_unit_count() == 1);
        CHECK(residency.resident_entity_count() == 2);
        CHECK(residency.resident_identities() == d_ids);
    }

    COMPOSE_TEST_MAIN_END();
}
