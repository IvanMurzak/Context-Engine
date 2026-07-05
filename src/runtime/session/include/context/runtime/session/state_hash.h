// Hierarchical canonical state hash + the per-tick trace (R-QA-005 / L-54).
//
// The canonical state hash is HIERARCHICAL: a per-tick ROOT hash composed from per-ARCHETYPE
// sub-hashes, plus (in trace mode) a per-SYSTEM breakdown — the world root hash captured after each
// system runs within a tick. This is the substrate the determinism story rests on:
//   * two runs of the same (seed + input stream) produce an identical root hash on every platform;
//   * on a divergence, the per-tick roots localize the FIRST divergent tick, the per-system hashes
//     localize the first divergent SYSTEM within it, and the per-archetype hashes localize WHICH
//     archetype's data moved — the inputs the auto-triage (`context determinism diff`) consumes.
//
// Canonicalization is by stable component NAME, not the first-touch-order ComponentId, so the digest
// is independent of component-registration order across processes (the cross-platform law). Entities
// within an archetype are folded in (index, generation) order; archetypes in signature order.

#pragma once

#include "context/editor/serializer/json_tree.h"
#include "context/kernel/world.h"
#include "context/runtime/session/sim_component.h"

#include <cstdint>
#include <string>
#include <vector>

namespace context::runtime::session
{

namespace serializer = ::context::editor::serializer;

// One archetype's contribution to the state hash.
struct ArchetypeHash
{
    std::string signature;         // component names, '+'-joined in name order (e.g. "position+velocity")
    std::uint64_t hash = 0;        // FNV over the archetype's entities + component fields (canonical order)
    std::uint64_t entity_count = 0;
};

// The hierarchical state hash at one instant.
struct StateHash
{
    std::uint64_t root = 0;
    std::vector<ArchetypeHash> archetypes; // sorted by signature
};

// One system's post-run world root hash within a tick.
struct SystemHash
{
    std::string system;
    std::uint64_t hash = 0;
};

// The hash tree recorded for one tick in trace mode: the end-of-tick root + per-archetype breakdown,
// plus the world root hash captured immediately after each system ran (the per-system attribution
// axis the divergence triage bisects on).
struct HashTree
{
    std::uint64_t tick = 0;
    std::uint64_t root = 0;
    std::vector<SystemHash> per_system;
    std::vector<ArchetypeHash> per_archetype;
};

using HashTrace = std::vector<HashTree>;

// Compute the hierarchical state hash of `world`, naming + folding components through `registry`.
[[nodiscard]] StateHash hash_world(const kernel::World& world,
                                   const SimComponentRegistry& registry);

// Canonical JSON projections (for `session hash`, trace output, and the replay artifact's expected
// trace). state_hash_to_json emits {"root", "archetypes":[{"signature","hash","entityCount"}...]}.
[[nodiscard]] serializer::JsonValue state_hash_to_json(const StateHash& hash);
[[nodiscard]] serializer::JsonValue hash_tree_to_json(const HashTree& tree);
[[nodiscard]] serializer::JsonValue hash_trace_to_json(const HashTrace& trace);

// The compact per-tick root list an artifact stores as its expected trace (index i == tick i).
[[nodiscard]] std::vector<std::uint64_t> trace_roots(const HashTrace& trace);

} // namespace context::runtime::session
