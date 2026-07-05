# `src/runtime/save/` — player save-game groundwork (R-DATA-005 / L-37, M2)

The **first `src/runtime/` (RuntimeKernel) target** (`context_save`). A player save is RuntimeKernel's
**own serialization** of player state — progress, world snapshots — **distinct from authored project
files** (R-FILE-009: RuntimeKernel never parses authored files; a save is a different format with a
different feed). This is the M2 co-design groundwork: it freezes the save **format** and ships a
minimal **migration runner** so player persistence is not a retrofit — the full runtime save/load API
surface (streaming snapshots, incremental saves) lands with the shipped-runtime milestones.

## What a save records

`SaveDocument` (`save_document.h`) serializes to canonical JSON — the same serializer authored files
use (R-DATA-004: save data is versioned like other serialized data), but it is NOT an authored file:

- a **per-component `schemaVersion` map** (`componentVersions`) — the SAME per-payload stamps as
  authored files (L-32/L-37);
- a declared **back-compat scope** (`backCompatScope`, N versions) — a bounded promise, not unbounded;
- **entity records addressed by composed identity** (L-37) — each entity keys on its deterministic
  id-path/stable hash (16-hex), the ONE identity shared by saves, network ids (R-NET-001), and query
  results, so a save taken before a re-derivation or engine upgrade still addresses the same entities.

The save-kind marker is `"$save": "ctx:save"` (distinct from an authored file's `"$schema"`), making
the R-FILE-009 boundary explicit on disk.

## The minimal migration runner (`save_migration.h`)

`migrate_save` loads an OLDER save into the running build. For each component type the save header
stamps, it migrates every entity's payload from the saved version up to the build's current version.
It **reuses the editor's per-payload migration mechanism** — `context::editor::migrate::migrate_payload`
— rather than a second copy: R-DATA-005 says a shipped build loads older saves "through the same
per-payload migration mechanism the editor uses at parse time," so the L-37 execution contract (tier
gating, per-invocation budget, purity, and **id immutability** — composed identity survives the
upgrade) is enforced by exactly the code the editor runs. Blocking findings (`save.unknown_component`,
`save.back_compat_exceeded`, the reused `schema.newer_than_*` / `migration.*` codes) roll the whole
save back — last-good, never a partial load.

## Layering note (R-FILE-009 preserved)

`context_save` links `context_serializer` and `context_migrate`. `context_migrate` is a
**serialization-layer leaf** — it links only `context_serializer` and contains no watcher or
derivation machinery — so RuntimeKernel consuming the migration MECHANISM does not reach into
EditorKernel's derivation/watch layer. This is the intended reuse (R-DATA-005's "same per-payload
migration mechanism"), not a cross-kernel dependency on EditorKernel behavior.

## Deliberate boundaries (documented follow-ups, not silent stubs)

- **The runtime save/load I/O surface** (writing/reading save files, streaming world snapshots,
  incremental/delta saves) is a shipped-runtime-milestone deliverable. M2 freezes the FORMAT and the
  migration runner; producing a `SaveDocument` from a live World and applying one back is the runtime
  world layer's job when it lands.
- **Selective partial load** (dropping only an unknown component instead of refusing the save) is a
  future policy; today an unknown component or a too-old payload is refused whole (the honest,
  last-good default). The declared back-compat scope is the tuning knob.
- **The compiled component set + its embedded migrations** are registered through
  `MigrationSet::engine_set()` (empty until the first engine component-schema bump); a shipped build
  embeds migrations for exactly the compiled set (R-DATA-005). The tests exercise the mechanism with
  the migrate module's `test:sprite` reference steps (the R-QA-011 fixtures pin them).

## Tests (R-QA-013)

`ctest --preset dev` runs `save-test_save_document` (serialize/parse round-trip fixpoint, the
composed-identity hex codec, parse failure paths) and `save-test_save_migration` (the round-trip
across a schema bump reusing the reference steps, composed-identity stability, id immutability,
back-compat scope, unknown component, newer-than, malformed, a chain gap, and all-or-nothing
rollback).
