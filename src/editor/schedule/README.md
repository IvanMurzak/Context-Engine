# `src/editor/schedule/` — the safe parallel scheduler

M3 task 4a (**R-SIM-006** / **R-LANG-011** / **L-38**, issue #92; Part of #88): systems declare their
component read/write sets, and the scheduler runs non-conflicting systems in parallel **safely**. It
completes the M3-exit gameplay-execution vertical — **a TS system and a C++ system both mutate the
shared World, scheduled safely in one tick** — over the (query, executor) system model
(`editor/system`, #91).

## What this module provides

- **Declared access sets + the conflict predicate** (`access.h`) — an `AccessSet { reads, writes }`
  (canonical: sorted, duplicate-free) and `conflicts(a, b)`, the classic reader/writer exclusion: two
  systems conflict iff they **write a common component** or **one writes what the other reads**.
  Read-read on a shared component does **not** conflict.
- **The composition + the cached DAG** (`schedule.h`) — a `SystemRegistry` (the composition; `add` /
  `remove` bump a generation), and a `ParallelScheduler` that derives the parallel schedule DAG
  **once** and **caches** it, rebuilding **only** when the composition generation changes —
  **never per frame** (`rebuild_count()` proves it: N ticks with no composition change leave it at 1).
- **The DAG** (`build_schedule`) — an as-soon-as-possible **level assignment** over the conflict
  graph: a native system's batch (level) is one past the highest level of any **earlier** native
  system it conflicts with. Same-level systems are therefore pairwise non-conflicting (safe to run
  concurrently); conflicting systems land in strictly later batches, **preserving registration order**
  (deterministic — L-54).
- **The single TS lane** — TS systems are **excluded from the DAG entirely** (R-LANG-011: one JS VM =
  one thread) and run **sequentially** on the JS-VM lane. Declared-access parallelism applies only to
  native/engine (and, later, WASM) systems.
- **Small-system batching** (`plan_batch_chunks`) — a native batch is split into coalesced worker
  chunks: runs of `Small`-cost systems are merged (so scheduling overhead can't dominate tiny
  systems), and the chunk count is capped at the worker count. Exposed for direct testing.
- **Fork-join execution** (`run_tick`) — each native batch runs its chunks over a bounded set of
  `std::thread`s (one chunk inline on the calling thread, the rest on helpers, all `join`ed before the
  next batch); then the TS lane runs sequentially. The native phase fully joins **before** the TS
  phase begins, so a native system and a TS system that both mutate the World in the same tick never
  overlap.

## The tick order

```
run_tick:
  for each native batch (in sequence):      # batch k+1 only after batch k joins
      run the batch's chunks in parallel     # all systems in a batch are non-conflicting
  for each TS system (in registration order):# the single JS-VM lane, sequential
      run it
```

`join()` is a clean happens-before edge and every batch has disjoint declared writes, so the parallel
native phase is data-race-free by construction. Because a batch's systems are mutually non-conflicting,
the parallel result is **identical** to a single-threaded registration-order run (a determinism
property the tests assert against a sequential reference World).

## The correctness gate (sanitizers)

**The scheduler's own parallel execution is proven race-clean by `tests/test_schedule.cpp`** — it
spins real `std::thread`s that mutate a shared World over disjoint declared writes, links **no V8**,
and so is **fully instrumented** by the CI `sanitize (ASan+UBSan, ubuntu)` and `sanitize (TSan,
ubuntu)` legs with **no suppressions**. A race there is a real bug the sanitizers catch outright. The
C++-and-TS keystone (`tests/test_schedule_in_v8.cpp`) additionally proves the cross-lane tick under the
V8 host; its rusty_v8 worker-pool races are covered by the reused `runtime/js` TSan suppressions
(V8-internal only, never `context::` code).

## Local vs CI

The core `context_schedule` library names only `kernel::ComponentId` + `std::thread`, so it — and the
`test_access` + `test_schedule` gates (conflict logic, DAG structure, cache invalidation, batching, and
the real fork-join parallel run over a World) — build and run on **every** toolchain, including the
local Strawberry-GCC Windows `dev` gate. Only the **keystone** (`test_schedule_in_v8`) links the
rusty_v8 prebuilt (through `context_js`) + esbuild (through `context_ts`), so it is **CI-only for its
V8 dependency path**: on the local stub toolchain it takes a reduced-assertion branch (the native
parallel batch still runs + mutates the World, and the derived accessor still bundles through esbuild),
and the **3-OS CI build legs are the authoritative gate** for the in-V8 flow.

## Deliberately out of scope (clean seams, tracked)

This tier orchestrates **opaque runnables** and makes the parallel-safety guarantee from the systems'
**declared** access. The following are split out and noted where they touch this module:

- **Batched per-archetype view handout + the per-frame ArrayBuffer cap** (R-LANG-012, task 4b) — how a
  native/TS system efficiently reaches its declared World access each tick. This module schedules the
  runnables; it does not (yet) materialize the views.
- **Debug-mode undeclared-access Proxy** (#88 item 2) — a debug build that **throws** on access outside
  a system's declaration. Release hands declared views only, which is enough for the guarantee here;
  the throw-on-undeclared half is separate.
- **End-of-system command buffer** (#88 item 3) — deferred structural changes (add/remove component,
  entity create/destroy). Systems in this tier do read/write over a **stable** archetype set only.
- **WASM-tier systems** (#88 item 4 remainder) — a WASM system participates in the native DAG by its
  declared access; `memory.grow` contract-invalidation is its own task.
- **TS source-map debug** (R-OBS-005, task 4b).
- **Overlapping the TS lane with disjoint native batches** — the current tick separates the native and
  TS phases in time (simple + honestly safe); overlapping the JS lane with non-conflicting native work
  is a future optimization, not a v1 requirement.
- **A persistent worker pool** — the current fork-join spawns bounded per-batch threads (the batching
  policy caps the count). A reusable pool is a performance follow-up; correctness is identical.
