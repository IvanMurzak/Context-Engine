// Determinism-divergence auto-triage (`context determinism diff`, R-QA-005 / L-54, issue #74).
//
// Given two replay artifacts, triage explains WHERE they diverge, from coarse to fine:
//   1. replay-bisect the two per-tick ROOT-hash ladders to the FIRST divergent tick;
//   2. within that tick, walk the per-SYSTEM hashes to the first divergent system;
//   3. attribute the divergence to a concrete (entity, component, field) by snapshotting each run's
//      world state right after that system ran and diffing it field-by-field, naming the field
//      through the R-LANG-010 sim-component schema (sim_component.h).
// The reported tuple is (tick, system, entity, componentField) — the four-level localization the
// hierarchical state hash was built to feed.
//
// Both artifacts are re-run LOCALLY with per-system introspection; determinism guarantees the
// re-run reproduces the recorded run, so the live world state is the recorded world state. When the
// two runs encode the same (seed + input) they are bit-identical on THIS host — a purely
// cross-platform divergence that cannot be reproduced locally; in that case triage still bisects the
// artifacts' STORED per-tick traces to the first divergent tick (reproduced=false) but cannot name
// the field (no local state to diff). Comparability metadata flags when the two runs were not even
// the same run (differing seed / scenario / input) so an input-driven difference is not misread as a
// determinism bug.

#pragma once

#include "context/kernel/world.h"
#include "context/runtime/session/replay.h"
#include "context/runtime/session/sim_component.h"

#include <cstdint>
#include <string>
#include <vector>

namespace context::runtime::session
{

// One entity's one component-field value at a snapshot instant — the granularity triage diffs.
struct FieldValue
{
    std::uint64_t entity_index = 0;
    std::uint64_t entity_generation = 0;
    std::string archetype; // canonical '+'-joined component-name signature (name order)
    std::string component; // stable component name (R-LANG-010 schema)
    std::string field;     // field name within the component (declared order)
    std::int64_t value = 0;
};

// The canonical per-field snapshot of a world: archetypes in signature order, entities in
// (index, generation) order, components in name order, fields in declared order — the SAME canonical
// walk hash_world() folds, so two snapshots of structurally-identical worlds align positionally.
using WorldSnapshot = std::vector<FieldValue>;

// Extract the canonical per-field snapshot of `world`, naming components/fields through `registry`.
// Unregistered component columns carry no named schema and are omitted (nothing to attribute).
[[nodiscard]] WorldSnapshot snapshot_world(const kernel::World& world,
                                           const SimComponentRegistry& registry);

// The pinpointed field-level attribution of a divergence.
struct FieldDivergence
{
    bool found = false;      // false == the two snapshots are identical
    bool structural = false; // true == the worlds differ in SHAPE (an entity/component present on one
                             //         side only), not merely in a field value
    std::uint64_t entity_index = 0;
    std::uint64_t entity_generation = 0;
    std::string archetype;
    std::string component;
    std::string field;
    std::int64_t left_value = 0;
    std::int64_t right_value = 0;
};

// Locate the FIRST canonical field where two snapshots differ (a value, or a structural mismatch).
[[nodiscard]] FieldDivergence first_field_divergence(const WorldSnapshot& left,
                                                     const WorldSnapshot& right);

// Bisect two per-tick root-hash ladders to the FIRST tick whose root differs. The traced replay
// yields every tick's root, so the "bisect" is a direct walk of the two ladders (cheaper than
// re-running to binary-search midpoints) — and, unlike a monotonic-boundary binary search, it stays
// correct when the divergence is a single non-monotonic point (two platforms' stored traces that
// differ at just one tick). Returns -1 when the ladders are identical (equal values AND equal
// length); a length mismatch with an equal common prefix diverges at the shorter length.
[[nodiscard]] std::int64_t bisect_first_divergent_tick(const std::vector<std::uint64_t>& left,
                                                       const std::vector<std::uint64_t>& right);

// The full triage report for `context determinism diff`.
struct DivergenceReport
{
    bool diverged = false;
    bool reproduced = true; // false == the divergence is cross-platform only (not reproducible on
                            //          this host), so tick is from the STORED traces and no field
                            //          attribution is available
    std::int64_t tick = -1; // first divergent tick; -1 == no divergence
    std::string system;     // first divergent system within that tick ("" == n/a / not reproduced)
    FieldDivergence field;  // (entity, component, field) attribution — only when reproduced

    // Comparability metadata: were the two runs even supposed to be identical?
    bool seed_match = true;
    bool scenario_match = true;
    bool input_match = true;
    std::uint64_t left_ticks = 0;
    std::uint64_t right_ticks = 0;
};

// Triage a determinism divergence between two replay artifacts (see file header).
[[nodiscard]] DivergenceReport triage_divergence(const ReplayArtifact& left,
                                                 const ReplayArtifact& right);

} // namespace context::runtime::session
