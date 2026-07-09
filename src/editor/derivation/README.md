# `src/editor/derivation/` — the incremental derivation graph (M1)

The LSP-style pipeline that turns **file changes** (from the merged `src/editor/filesync/` layer) into
the **derived `context::kernel::World`** — EditorKernel as a restartable incremental derivation engine
over project files (**L-19**, **L-22**). The RuntimeKernel never parses authored files; it consumes this
graph's output (**L-24**). The graph mechanics were the M1 deliverable; the
canonical parse node now delegates to the REAL canonical-JSON serializer (`src/editor/serializer/`,
M2 issue #42) behind the same stable seam the M1 placeholder occupied — the graph never changed.

Library target: **`context_derivation`** (links `context_filesync` for the `ReconcileChange` input type,
`context_kernel` for the derived `World` + the internal `EventBus`, `context_serializer` for the
canonical parse node's real body, and `context_warnings`). Namespace:
`context::editor::derivation`.

## What it implements

- **Derivation-graph skeleton (L-22)** — `DerivationGraph::apply()` ingests a file-sync `ReconcileChange`
  + the authored bytes: `source file → canonical parse → derived World`. One entity per source file
  carries a `DerivedSource{canonical_hash, generation}`. `run_pass()` (re)derives the dirty subgraph and
  advances a monotonic **derived-world generation counter**, published on the internal event bus as
  `DerivationPassEvent` (the bridge-daemon's client event stream forwards it, R-BRIDGE-008).
- **Incremental re-derive (L-22)** — only nodes in the pending dirty set are visited, and each is
  **content-hash memoized**: a dirty node whose canonical form is unchanged (e.g. a whitespace-only
  edit, or a save-with-no-edit) is skipped, so its derived value and per-node generation are frozen.
- **Derivation-side backpressure (R-FILE-013)** — a write **burst coalesces** into batched passes (one
  batched pass per burst, never one pass per event); a **queue-depth / bounded-lag signal**
  (`BackpressureSignal`, also emitted as `BackpressureEvent` on overload transitions) lets cooperative
  clients self-throttle; and under overload the pass **load-sheds** — the visible/queried subgraph
  derives first, the rest defers to later passes.
- **Read-your-writes barrier (R-CLI-006)** — `apply()` returns a `WriteTicket{canonical_hash,
  generation_after}`. `query_barrier.h` bounded-blocks a read until the derived world reflects it:
  `wait_for_hash` is the **own-write** barrier (`--after-hash`, robust under load-shedding),
  `wait_for_generation` is the **foreign-generation** barrier (`--after-generation`); an unsatisfiable
  barrier **times out explicitly** rather than hanging.

- **Shader-compile node (R-FILE-005 / R-FILE-010 / R-FILE-013, issue #126)** —
  `ShaderCompileNode` (`shader_compile_node.h`) makes shader compilation a first-class derived
  artifact rather than a standalone side cache. It wraps the backend-free `IShaderCompiler` seam
  (`src/render/material/`): inputs `(authoring IR, variant key, compiler id)` → a `CompiledArtifact`,
  cached under the **content-addressed** R-FILE-010 key (`ir_content_hash | variant | compiler id`).
  The node OWNS the content-addressed store re-homed from the T3a `ShaderCompileCache`, and adds the
  two derivation-graph behaviours a side cache lacked: **invalidation** (`invalidate_ir()` drops every
  stale variant of a re-authored shader, so the next request recompiles under the new content) and
  **backpressure** (a coalesced `request()` queue drained by `run_pass()` under a per-pass compile
  budget, load-shedding non-priority work under overload and publishing the shared `BackpressureSignal`
  — the same bounded-lag vocabulary the file→World graph uses). The synchronous `get_or_compile()`
  preserves the exact T3a cache-hit semantics for a query that needs the artifact immediately. Backend-
  agnostic: the fake/reference backend is retained; the real toolchain drops in behind the seam later.

## Known M1 simplifications (deliberate, documented)

- ~~The canonical parse is a whitespace-normalizing placeholder~~ **RESOLVED (M2 wave 1, #42)**: the
  parse node delegates to the real canonical-JSON serializer (`src/editor/serializer/` — R-FILE-001
  key order / number formatting / NFC, the two-hash split, and the committed test-vector corpus).
  Non-JSON content passes through raw (raw ≡ canonical — the binary-sidecar rule).
- ~~No validate node~~ **RESOLVED (M2 wave 2, #47)**: schema-bound JSON validates on ingest against
  the registered kind schemas (`src/editor/schema/`, R-DATA-006) — on the SAME parse the canonical
  node produced. A FAILING payload retains its last-good derived state (R-FILE-003); diagnostics
  (JSON-pointer + line/column into the source bytes) surface through `validation()` with L-31
  stability. A nullptr schema set preserves the exact M1 behavior; richer per-kind derived
  components (compose/instantiate) still land in later M2 tasks.
- ~~No compose node~~ **PARTIALLY RESOLVED (M2 wave 3, #51 — the READ path)**: valid scene
  documents retain a composition view (`src/editor/compose/`, L-35) and every pass re-flattens the
  scenes it touched **plus their transitive dependents** over the graph's first cross-file
  dependency edges (instance edges — editing a template updates every instancing scene's composed
  view). `composed()` surfaces the flattened entities (id-path identity, innermost-out override
  precedence, R-CLI-006 provenance chains) + diagnostics, with closure-aware L-31 stability; a
  validation-FAILING scene ingest retains its last-good composed view like its derived node
  (R-FILE-003). Fan-out re-flatten is synchronous within the pass (the async-streamed fan-out of
  R-FILE-011(b) lands with the daemon threading model); the **instantiate** node (composed output →
  per-kind derived World components) and the composed WRITE path are still later M2 tasks — the
  derived World itself remains a flat file→entity map.
- **The "bounded-block" is modeled as a bounded number of passes**, not real threads/sleeps — matching
  how a daemon drains its queue while a query waits, and keeping the whole layer deterministic (no
  concurrency in M1; the daemon's threading model is a later task).
- **The backpressure signal is produced, not yet wired to the client stream.** It surfaces on the
  kernel-internal `EventBus`; forwarding it onto the R-BRIDGE-008 client event stream is the bridge's
  concern. The documented max dirty-set latency these bounds enable is benchmarked by the R-FILE-011
  bench task.

## Tests

`ctest --preset dev` runs each `derivation-*` executable (see `CMakeLists.txt`): the canonical-parse
seam (real-canonicalizer formatting-insensitivity, non-JSON passthrough, fixpoint, determinism,
encoding heals), the end-to-end change→World+generation flow
(create / removal / empty-pass), incremental re-derive + memoization + coalescing, backpressure
(coalescing, the queue-depth signal, load-shed to the visible subgraph, event publication), the
read-your-writes barrier (own-write hash, foreign generation, explicit timeout, load-shed
robustness), and the compose node (`test_compose_node`: ingest→flatten, template-edit fan-out to
dependents, memoized no-op edits leaving composed output untouched, removal → missing-scene
diagnostics on dependents, last-good retention under a failing ingest, closure-aware stability), and
the shader-compile node (`test_shader_compile_node`: the re-homed T3a cache-hit / distinct-variant /
full-variant-space / cache-key-enumerates-inputs semantics now driven through the node, plus the added
request-queue coalescing + overload load-shed with a bounded per-pass fill and the R-FILE-005
invalidate-drops-stale-variants + invalidate-drops-pending-requests paths).
