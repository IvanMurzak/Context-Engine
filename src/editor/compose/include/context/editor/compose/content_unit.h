// Content-unit boundary emission (R-ASSET-005 / L-35): flatten emits GUID-addressable,
// independently loadable/unloadable content-unit boundaries, co-designed with the chunked pack
// format (docs/chunk-pack-format.md) the M8 packer freezes against. A content unit is a slice of a
// flattened scene keyed by the L-37 composed identity of its boundary root — the ONE identity
// shared by player saves (R-DATA-005), network ids (R-NET-001), and query results.
//
// This is the M2 co-design deliverable: the EMISSION of conformant boundaries plus a PROOF that
// they load/unload independently through the existing derivation-output seam (the ComposedScene).
// It is NOT the M8 packer: no on-disk chunk encoding, codec, or async streaming scheduler lives
// here (those slot into the reserved header fields in docs/chunk-pack-format.md).

#pragma once

#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::compose
{

// One content-unit boundary of a flattened scene (docs/chunk-pack-format.md §2). Boundaries ARE
// flatten boundaries: the root scene's own directly-authored content is the root unit; each
// top-level instance subtree is its own unit.
struct ContentUnit
{
    // The GUID-addressable key: the 16-hex composed identity (L-37) of the unit's boundary root —
    // identity_hash_of(rootScene, []) for the root unit, identity_hash_of(rootScene, [instanceId])
    // for a top-level instance unit. Stable across re-derivation and engine upgrade.
    std::string unit_id;
    std::uint64_t identity_hash = 0; // the raw hash behind unit_id (format_stable_id renders it)
    bool is_root = false;            // the root scene's own content vs a top-level-instance subtree
    // The id-path[0] that defines an instance unit (the top-level instance id); EMPTY for the root
    // unit (its boundary root is the scene root, addressed by an empty id-path).
    std::string boundary_segment;
    // The instanced scene reference: the project-root-relative path in M2; widens to the scene GUID
    // once asset-db refs flow through compose (docs/chunk-pack-format.md §6). For the root unit,
    // the root scene path.
    std::string source_scene;
    // Indices into ComposedScene::entities (expansion order) of the entities in this unit.
    std::vector<std::size_t> entity_indices;
};

// The content-unit partition of one flattened scene: the frozen pack directory (the manifest the
// M8 packer lays chunks out from). Deterministic: the root unit first, then top-level instances in
// authored order (docs/chunk-pack-format.md §2).
struct ContentUnitSet
{
    std::string root_path;
    std::vector<ContentUnit> units;
};

// Partition a flattened scene into content units per the frozen boundary rule
// (docs/chunk-pack-format.md §2). `resolver` supplies the root scene doc so the top-level instance
// ids are known (the same resolver the flatten was produced with). Total and deterministic; a unit
// is emitted iff it contains at least one entity (a failed-to-resolve instance yields no unit).
[[nodiscard]] ContentUnitSet partition_content_units(const ComposedScene& scene,
                                                     const SceneResolver& resolver);

// Canonical-JSON emitter of the pack directory / manifest (docs/chunk-pack-format.md §3): the
// frozen columns the M8 packer reads, so the boundary rule cannot silently drift (a change is a
// reviewed diff on this shape). Shape:
//   {"rootScene", "unitCount", "units": [{"unitId", "parentUnit", "isRoot", "sourceScene",
//    "entityCount", "identities": [<16-hex composed identity>…]}…]}
// `parentUnit` is empty for every M2 unit (top-level granularity; §2.1). `identities` lists each
// unit's composed-entity identities in expansion order — the addressing keys a chunk carries.
[[nodiscard]] std::string content_units_json(const ContentUnitSet& units,
                                             const ComposedScene& scene);

// Boundary-property PROOF (docs/chunk-pack-format.md §5): a minimal residency model proving the
// emitted boundaries are independently loadable/unloadable — the R-ASSET-005 runtime capability,
// co-designed here over the derivation-output seam. The async streaming loader itself is M8; this
// proves only that the BOUNDARIES support independent load/unload with isolation.
//
// Built from a ContentUnitSet + the ComposedScene it partitions (the flatten output). Load/unload
// are keyed by unit id; residency of one unit never affects another (the isolation property).
class ContentUnitResidency
{
public:
    ContentUnitResidency(const ContentUnitSet& units, const ComposedScene& scene);

    // Mark a unit resident (materialize it). Returns false for an unknown unit id; loading an
    // already-resident unit is idempotent (returns true).
    bool load(std::string_view unit_id);

    // Drop a unit from residency. Returns false for an unknown or not-currently-resident unit id.
    bool unload(std::string_view unit_id);

    [[nodiscard]] bool is_known(std::string_view unit_id) const;
    [[nodiscard]] bool is_resident(std::string_view unit_id) const;
    [[nodiscard]] std::size_t resident_unit_count() const noexcept;
    // Total composed entities across all resident units (isolation asserts compare this before and
    // after loading/unloading a sibling).
    [[nodiscard]] std::size_t resident_entity_count() const noexcept;
    // The composed identities resident right now, ascending (a stable fingerprint of residency for
    // isolation asserts: unloading unit A leaves unit B's identities exactly as they were).
    [[nodiscard]] std::vector<std::uint64_t> resident_identities() const;

private:
    struct Unit
    {
        std::string unit_id;
        bool resident = false;
        std::vector<std::uint64_t> identities; // composed identities of the unit's entities
    };
    std::vector<Unit> units_;
};

} // namespace context::editor::compose
