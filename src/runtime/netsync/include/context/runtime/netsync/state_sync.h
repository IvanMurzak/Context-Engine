// State-sync harness over the L-48 in-storage replication metadata (M6 X2, R-NET-001).
//
// This runtime module DRIVES the fifth in-storage World protocol (world.h § L-48 replication
// metadata): per-entity network identity (the L-37 composed id), authority, and dirty/delta
// versioning. It adds the layer ABOVE those kernel hooks that a two-session netcode loop needs and
// the hooks deliberately do not carry (world.h: "these are the HOOKS ONLY; the state-sync harness
// that drives them is X2"):
//
//   * an identity guard (register_replicated) that keeps the composed-id -> entity mapping 1:1;
//   * a DirtyScanner that marks an entity dirty ONLY when its replicated bytes actually changed, so
//     the delta carries only genuinely-changed entities (static / settled bodies drop out);
//   * capture_delta — serialize the dirty/delta since a peer's cursor (only changed entities/fields,
//     each keyed by the composed id + authority) into a transport-agnostic snapshot;
//   * apply_snapshot — apply that snapshot to a REPLICA world so it CONVERGES to the source's
//     replicated state, fail-closed + atomic (a bad payload or an authority conflict leaves the
//     replica untouched).
//
// v1 is an IN-PROCESS two-session harness (source authority + replica) — the R-NET-001 validation
// bar. No real network transport / sockets: a snapshot is a plain value the caller hands to the
// replica. Because both sessions live in one process the process-global kernel::ComponentId of a
// component type is identical in both worlds, so the raw byte payload of a component transfers
// directly (a real cross-process transport would map component ids by stable name — out of scope).
//
// SCOPE: v1 replicates trivially-relocatable POD components (the M6 sim invariant — every sim
// component is a POD of std::int64_t fields, sim_component.h). The replica stores them through the
// kernel's data-driven raw storage path (add_raw + pod_ops, the R-LANG-010 seam), so this module
// needs no compile-time knowledge of the replicated types and composes on nothing but the kernel
// World (the moving-body sim it replicates — context_physics3d / physics2d — lives only in the test).

#pragma once

#include "context/kernel/component.h"
#include "context/kernel/entity.h"
#include "context/kernel/world.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace context::runtime::netsync
{

// One replicated component type: its process-global kernel ComponentId plus the POD layout the
// replica reconstructs it with (add_raw + pod_ops). `size` must be >= 1.
struct ReplicatedComponent
{
    kernel::ComponentId id = 0;
    std::size_t size = 0;
    std::size_t align = 0;
};

// The ordered set of component types a harness replicates. Built once by the caller from each
// replicated type's kernel::component_id<T>() + sizeof/alignof(T).
class ReplicatedComponentSet
{
public:
    // Register component id (deduplicated — a repeat id keeps the first entry). `size` >= 1.
    void add(kernel::ComponentId id, std::size_t size, std::size_t align);

    // Convenience: register a C++ POD component type T by its process-global id + layout.
    template <class T>
    void add()
    {
        add(kernel::component_id<T>(), sizeof(T), alignof(T));
    }

    [[nodiscard]] const std::vector<ReplicatedComponent>& components() const noexcept
    {
        return components_;
    }
    [[nodiscard]] bool empty() const noexcept { return components_.empty(); }

    // The declared entry for `id`, or nullptr if `id` is not in the set.
    [[nodiscard]] const ReplicatedComponent* find(kernel::ComponentId id) const noexcept;

private:
    std::vector<ReplicatedComponent> components_;
};

// A single replicated component's raw bytes inside an entity delta.
struct ComponentBytes
{
    kernel::ComponentId id = 0;
    std::vector<std::byte> bytes;
};

// One entity's contribution to a snapshot: its L-37 network identity + authority, plus the raw bytes
// of each replicated component currently present on it.
struct EntityDelta
{
    std::uint64_t net_id = 0;
    std::uint32_t authority = 0;
    std::vector<ComponentBytes> components;
};

// A dirty/delta snapshot: the entities whose replicated state changed since a peer's cursor, in the
// kernel's deterministic delta order (ascending net_id), plus the source's replication version AFTER
// capture — the cursor the peer advances to for its next capture.
struct StateSyncSnapshot
{
    std::uint64_t source_version = 0;
    std::vector<EntityDelta> entities;
};

// net_id (the L-37 composed id) -> the entity carrying it, in ONE world. Used both as the source-side
// identity guard (register_replicated) and the replica-side resolution map (apply_snapshot).
using NetIdMap = std::unordered_map<std::uint64_t, kernel::Entity>;

// Register `e` (in `world`) for replication with `net_id` + `authority`, enforcing L-48 identity on
// top of world.set_replication: `net_id` must be non-zero (net.invalid_net_id) and unused in
// `registry` (net.duplicate_net_id). Returns nullptr on success (and records net_id -> e in
// `registry`); on refusal returns the net.* code and leaves the world + registry untouched.
[[nodiscard]] const char* register_replicated(kernel::World& world, kernel::Entity e,
                                              std::uint64_t net_id, std::uint32_t authority,
                                              NetIdMap& registry);

// Capture the dirty/delta from `source` since `since_version` for the components in `set`: walk
// source.replication_delta_since(since_version) (deterministic ascending-net_id order) and, for each
// replicated entity, record its net_id + authority (from replication_of) and the raw bytes of every
// set-component present on it. `source_version` is set to source.replication_version(). Pass
// since_version == 0 for a full snapshot of every replicated entity.
[[nodiscard]] StateSyncSnapshot capture_delta(const kernel::World& source,
                                              const ReplicatedComponentSet& set,
                                              std::uint64_t since_version);

// The outcome of apply_snapshot.
struct ApplyResult
{
    const char* error = nullptr; // nullptr on success; a net.* code on a fail-closed refusal
    std::size_t applied = 0;     // entities applied (0 when error != nullptr — apply is atomic)
};

// Apply `snap` to `replica` so it converges to the source's replicated state, resolving each net_id
// via `map` (creating + replication-registering a fresh replica entity on first sight, reusing it
// thereafter). Fail-closed + ATOMIC: every delta is validated FIRST — a zero net_id
// (net.invalid_net_id), a component payload whose length disagrees with `set` (net.snapshot_component_
// mismatch), or (when `replica_authority` is engaged) a delta whose authority equals the replica's
// own authority (net.authority_conflict) — and on ANY violation the replica is left completely
// untouched and the code is returned. On success each entity's components are written (add_raw for a
// component the replica lacks, in-place overwrite otherwise) and its authority is synced (an L-48
// authority handover replicates). `has_replica_authority`/`replica_authority` name the peer id this
// replica is authoritative over; leave `has_replica_authority` false to accept every delta.
[[nodiscard]] ApplyResult apply_snapshot(kernel::World& replica, const StateSyncSnapshot& snap,
                                         const ReplicatedComponentSet& set, NetIdMap& map,
                                         bool has_replica_authority = false,
                                         std::uint32_t replica_authority = 0);

// Marks an entity dirty (world.mark_replication_dirty) ONLY when its replicated bytes changed since
// the previous scan — the delta-culling half of the harness. It tracks the last-seen concatenated
// bytes per net_id, so an unmoving entity (a static body, a settled one) is scanned but never
// re-marked, and therefore drops out of every subsequent capture_delta. A freshly-registered entity
// is already dirty (set_replication marks it), so the FIRST capture full-snapshots it regardless.
class DirtyScanner
{
public:
    // Scan every replicated entity in `world`; mark dirty each whose set-component bytes changed
    // since the last scan (or that is seen for the first time with non-empty state). Returns the
    // number marked dirty this scan.
    std::size_t scan(kernel::World& world, const ReplicatedComponentSet& set);

    // Forget all tracked state (e.g. before replaying a fresh run).
    void reset() noexcept { last_.clear(); }

private:
    std::unordered_map<std::uint64_t, std::vector<std::byte>> last_; // net_id -> concatenated bytes
};

// Read the concatenated raw bytes of the `set` components present on `e` in `world`, in set order
// (absent components contribute nothing). A small shared helper the harness + convergence assertions
// use to compare an entity's replicated state across two worlds.
[[nodiscard]] std::vector<std::byte> read_replicated_bytes(const kernel::World& world,
                                                           kernel::Entity e,
                                                           const ReplicatedComponentSet& set);

} // namespace context::runtime::netsync
