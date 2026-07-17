# `src/runtime/netsync/` — replication state-sync harness (M6 X2, R-NET-001 / L-48)

The state-sync layer that DRIVES the F0b in-storage **L-48 replication metadata** hooks
(`src/kernel/.../world.h` § *L-48 replication metadata*): per-entity network identity (the L-37
composed id), authority, and dirty/delta versioning. F0b shipped those hooks and left the harness to
X2 ("these are the HOOKS ONLY; the state-sync harness that drives them is X2"). This module is that
harness — it does **not** re-implement the metadata, it composes on top of it.

## What it provides (`context_netsync`)

A small, transport-agnostic, **physics-agnostic** core over the kernel `World` (`state_sync.h`):

- **`register_replicated`** — an identity guard on top of `World::set_replication`: keeps the
  composed-id → entity mapping 1:1 (`net.invalid_net_id` / `net.duplicate_net_id`, fail-closed).
- **`DirtyScanner`** — marks an entity dirty (`World::mark_replication_dirty`) **only** when its
  replicated bytes actually changed since the previous scan, so a static or settled body drops out of
  every later delta (the delta-culling half of the harness).
- **`capture_delta`** — serializes the dirty/delta since a peer's cursor
  (`World::replication_delta_since`) into a `StateSyncSnapshot`: only changed entities/fields, each
  keyed by its composed id + authority.
- **`apply_snapshot`** — applies a snapshot to a **replica** world so it *converges* to the source's
  replicated state. Fail-closed + **atomic**: a zero net id, a payload whose length disagrees with
  the replicated component set (`net.snapshot_component_mismatch`), or an authority conflict
  (`net.authority_conflict`, when the replica owns the entity) leaves the replica completely
  untouched.

The replica reconstructs components through the kernel's **data-driven raw storage path**
(`add_raw` + `pod_ops`, the R-LANG-010 seam), so the module needs no compile-time knowledge of the
replicated types and links nothing but `context_kernel`. v1 replicates trivially-relocatable **POD**
components (the M6 sim invariant — every sim component is a POD of `std::int64_t` fields).

## v1 scope (the R-NET-001 validation bar)

An **in-process, two-session** harness (source authority + replica) — no real network transport /
sockets. A `StateSyncSnapshot` is a plain value the caller hands to the replica. Because both
sessions live in one process, a component type's process-global `kernel::ComponentId` is identical in
both worlds, so a component's raw bytes transfer directly; a real cross-process transport would map
component ids by stable name (out of scope for v1).

## Tests

- `tests/test_state_sync.cpp` — the generic core over synthetic POD components (no physics):
  capture/apply round-trip, delta culling, the net-id identity guard, snapshot size-mismatch and
  authority-conflict fail-closed paths, full-snapshot vs incremental delta.
- `tests/test_convergence.cpp` — the real **two-session moving-body convergence** proof: a
  `context_physics3d` scene (and a `context_physics2d` scene) of dynamic bodies replicated source →
  replica by dirty/delta over N fixed ticks; the replica, which never runs physics, converges to the
  source's positions/velocities (byte-exact), the per-tick delta shrinks as bodies settle, a static
  body is sent only in the initial full snapshot, and an authority handover replicates.
- `tests/test_packed_replication.cpp` (M8 a07, `netsync-packed-replication`) — the convergence harness
  re-run **against packs**: a real v1 pack is loaded back through the shipped runtime content seam
  (`PackContentSource` → `RuntimeContentLoader`), each packed entity's composed identity (L-37 — the ONE
  identity R-NET-001 binds network ids to) is read out, and the source→replica moving-body convergence
  is driven keyed by those pack-carried identities — validating that the M8 wedge builds carry the
  validated L-48/R-NET-001 replication metadata. Every replication net-id is asserted to come from the
  pack. Not a strict-FP scene (no `determinism-*` ctest); runs on all three `build` legs.
- `tests/test_errors.cpp` — pins the `net.*` code strings to their catalog identities.

The `net.*` fail-closed codes are defined in `include/context/runtime/netsync/errors.h`
(`context::runtime::netsync::k*Code`) and registered by the contract error catalog's F0a-reserved
`net.*` block (`src/editor/contract/src/error_catalog.cpp`) — the module never links the contract
layer (dependency direction stays runtime → kernel, per L-60).
