# `src/editor/migrate/` — per-payload schema versioning + parse-time migration

Implements the L-37 / R-DATA-004 schema-evolution law (M2 wave 3, issue #52):

- **Versioning is per component payload.** The authored-file header's `componentVersions` map
  (`{"<ns>:<type>": <schemaVersion>}`, read by the serializer's `read_document_header`) stamps the
  schema version of every component payload the file carries.
- **The engine reads old versions by migrating at parse time** (`migrate_document`) — entirely in
  memory. Truth on disk never mutates silently: the ONLY migrations that write disk are tool saves
  (which canonicalize + stamp the file they were writing anyway) and the explicit `context migrate`
  bulk verb (`src/cli/src/migrate_command.cpp`).
- **The registered set is R-FILE-005 pass-0 state.** `MigrationSet` (current version per component
  type + the steps between versions) is, with the kind `SchemaSet`, the "registered schema +
  migration set" the pass-0 stratum derives. Its `set_hash()` is folded into
  `DerivationConfig::registered_set_hash`, which the derivation graph keys memoization on — a
  package upgrade that changes a migration yields NEW cache keys, never stale derived state
  (R-FILE-010).

## The execution contract (L-37)

Every step invocation is wrapped by the engine's contract checks (`apply_step`):

| Rule | Enforcement |
|---|---|
| Tier gating | `package_sandboxed` steps run ONLY through an injected `MigrationRunner` (the sandboxed-migration seam, `migration_runner.h`). With NO runner injected they are REFUSED in-process (`migration.runner_unavailable`) — never run unsandboxed; with the wasmtime runner injected (issue #71 PR3) they route to the guest per the frozen ABI under the SAME host-side contract (budget, canonical, id immutability re-checked host-side). `engine_native` (first-party) steps run now. |
| Budget | `MigrationBudget::max_nodes` bounds a step's input AND output payload (`migration.budget_exceeded`). Deterministic by design (node counts, not wall time); the WASM runner (issue #71 PR3, `src/runtime/wasm/`) maps the same budget to deterministic VM fuel (`kWasmFuelPerBudgetNode × max_nodes`), and its fuel exhaustion reuses the SAME catalog code via `SandboxedStepResult::budget_exceeded`. |
| Purity / determinism | Steps see ONLY the payload (no IO/clock/randomness by API shape) and are pinned forever by the R-QA-011 fixture corpus, round-tripped in CI. |
| Id immutability | The exact multiset of (`id`/`guid` pointer, canonical value) inside a payload must survive every step (`migration.id_mutated`) — composed identity survives upgrade. |
| Downgrade rule | A payload stamped NEWER than the installed schema is never best-effort parsed: `schema.newer_than_engine` (engine `ctx:` namespace) / `schema.newer_than_package` (any other), blocking, last-good retained (R-PKG-005). |
| Path transforms | Migrations transform override/reference paths as well as payloads (per-step `PathTransform`); an unmappable path yields a non-blocking `migration.orphan_override` finding — the entry is preserved on disk/in-memory, and flatten excludes it (the compose layer consults the same rule). |
| All-or-nothing | Any blocking finding rolls the WHOLE document back; derivation retains last-good derived state (R-FILE-003) and the bulk path does not write the file. |

## The sandboxed execution seam + guest ABI (issue #71)

`migration_runner.h` defines `MigrationRunner` — the dependency-inverted boundary the
`package_sandboxed` tier runs through. `migrate` DEPENDS ON NOTHING wasm: the wasmtime runtime
(PR3 of the #71 chain) implements `MigrationRunner` in a separate library and is INJECTED via
`MigrateOptions::runner` / `migrate_payload(..., runner)`. `apply_step` routes a package step to the
injected runner and, with no runner injected, keeps the honest `migration.runner_unavailable`
refusal (the seam never runs package migrations unsandboxed).

The same header FREEZES the guest ABI (protocolMajor 1) — the wire between the host runner and a
compiled package migration module: ZERO host imports; exports `ctx_alloc` + `ctx_migrate` (required)
and `ctx_map_path` (optional — absent ⇒ identity path mapping); all input/output byte buffers are
canonical JSON in the module's linear memory; the host maps `MigrationBudget::max_nodes` to VM fuel
(`K × max_nodes` — `kWasmFuelPerBudgetNode` in `src/runtime/wasm/wasm_runner.h`) and re-checks every
structural invariant (budget, canonical serializability, id immutability) host-side — the guest is
trusted for nothing. The real runner (`context::runtime::wasm::WasmRunner`, PR3 — LANDED) owns the
fresh-Store+instance-per-step lifecycle and the deterministic VM config; it is injected at the
R-FILE-005 cold-start "VM" slot via `EditorKernelConfig::migration_runner`. `tests/mock_runner.h`
is a byte-only mock defined STRICTLY to this contract (never more capable than the real runner);
`tests/test_migration_runner.cpp` exercises the routing, the refusal fall-through, the budget->fuel
failure mapping, the host re-checks, and the `ctx_map_path` override rewrite; the real-VM
end-to-end equivalents live in `src/runtime/wasm/tests/`.

## Fixtures (R-QA-011)

`fixtures/<type-dir>/v<N>-to-v<M>/{input.json,golden.json}` — pre-migration input + golden
post-migration output per schema bump, committed forever; `tests/test_fixtures.cpp` enumerates and
byte-compares every pair and asserts the idempotence fixpoint. See `fixtures/README.md`.

The engine-shipped set (`MigrationSet::engine_set()`) is EMPTY today — no engine component-payload
schema has ever bumped. The first real bump registers its step there and adds its fixture pair in
the same PR.
