# src/runtime/

**RuntimeKernel** (formerly "RuntimeCore") — the runtime that the editor embeds for
play-in-editor and that packaged builds ship, so what you play in the editor is exactly what
ships. Hosts the sim/render split and the deterministic native/WASM tier. Governed by the Context
Engine design records: **ARCHITECTURE.md** (sim/render split, L-39) and **REQUIREMENTS.md**
(R-SIM-*, R-PLAY-*).

RuntimeKernel **never parses authored project files** (R-FILE-009) — it consumes derivation-graph
output through one loading seam, and player saves are its OWN serialization, distinct from authored
files.

## Modules

- **`session/`** (M3-entry, issue #74) — the deterministic headless simulation session + versioned
  replay (R-QA-005 / L-54): fixed-timestep stepping driven only by a seed + an input stream, the one
  sim-facing `InputState` sink, and the hierarchical state hashing the `determinism-*` CI family
  asserts cross-platform. See `session/README.md`.
- **`determinism/`** (M6-F0a, issue #170) — the deterministic-build attestation (R-SIM-005 / L-54):
  `deterministic:true` is produced only from the actually-applied, verified strict-FP flags
  (compiler-set macros cross-checked against the flags CMake recorded), never a self-declared bit.
- **`save/`** (M2, issue #66) — player save-game groundwork (R-DATA-005 / L-37): the save format
  (per-component `schemaVersion` header + composed-identity-addressed entity records) and a minimal
  save-migration runner reusing the editor's per-payload migration mechanism. The first `src/runtime/`
  target. See `save/README.md`.
- **`js/`** (M3, issue #76) — the in-process V8 JavaScript host (L-61 / R-LANG-002/008/009): the
  embedded VM authored gameplay runs on, including the zero-copy view lifetime/invalidation protocol
  (`runSystemView`). V8 is an opt-in SHA-pinned prebuilt (default-OFF toggle; honest stub otherwise).
  See `js/README.md`.
- **`ts/`** (M3, issue #83) — the TypeScript toolchain (L-61 / R-LANG-002/004): transpiles + bundles
  authored TS into the single JS module the `js/` host evaluates, over the SHA-pinned esbuild prebuilt
  channel. See `ts/README.md`.
- **`wasm/`** (issue #71) — the sandboxed WASM package-migration runner tier (L-37 / L-62 /
  R-SEC-009): wasmtime-Cranelift with deterministic `consume_fuel` metering executes package-shipped
  migration steps (`MigrationTier::package_sandboxed`). See `wasm/README.md`.
- **`profile/`** (M6 X1) — the L-47 profiler substrate (R-SIM-008 / R-OBS-002): the engine's first
  profiler channel — per-tick, attributed GC-pause samples + running aggregates over the JS engine's
  pull seam, CLI/RPC-queryable. See `profile/README.md`.
- **`netsync/`** (M6 X2) — the replication state-sync harness (R-NET-001 / L-48): a transport-agnostic
  core that drives the kernel's in-storage replication metadata hooks (network identity, authority,
  dirty/delta versioning). See `netsync/README.md`.

The runtime content-unit loader (R-ASSET-005 — the chunk format is co-designed under
`docs/chunk-pack-format.md`) lands in a later milestone.
