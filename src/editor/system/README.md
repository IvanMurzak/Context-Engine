# `src/editor/system/` — the (query, executor) system model + derived TS accessors

The M3 keystone (**R-LANG-009** / **R-LANG-010** item 1 / **R-LANG-002**, issue #90, Part of #88):
gameplay **authored in TypeScript mutates the shared World**. It stitches the two landed halves —
the zero-copy view-lifetime gate (`runtime/js` `runSystemView`, #89) and declarative component
storage (`editor/component`, #87) — into a runnable system tier.

## What this module provides

- **The (query, executor) system model** (`system.h`) — a system is a `(query, executor)` pair. The
  query is a set of declared component types; the executor is a resolved JS function that runs **once**
  over zero-copy views of those components. `run_system(world, engine, executor, query)`:
  1. **selects** every live entity whose archetype carries **all** queried components, in the World's
     canonical archetype+row order (`select_entities` — deterministic, L-54);
  2. **gathers** each queried component's records for those entities into a contiguous VM-allocated
     backing store, one `VmBuffer` per column (`gather_column`);
  3. binds each column as a zero-copy `Uint8Array` view and invokes the executor **once** through
     `JsEngine::runSystemView` — so the views are detached/neutered at exit (the **R-LANG-009 hard
     gate is preserved**, not weakened here);
  4. **scatters** the mutated records back (`scatter_column`).
- **Derived TS accessors** (`accessor_codegen.h`) — `generate_component_accessor_ts(type)` emits a
  small, deterministic TypeScript class from a compiled `ComponentTypeSchema`. Its instance wraps a
  bound view and exposes `get<Field>` / `set<Field>` per field (a `lanes>1` field takes a `lane`
  argument), a `count`, and a static record `stride`. Authored TS `import`s it and reads/writes the
  gathered bytes directly.

## The keystone (the DoD proof)

`tests/test_system_in_v8.cpp` registers a `demo:mover` component, seeds entities, generates the mover
accessor, bundles an **authored** TS system (`tests/examples/movement_system.ts`) that imports it
(esbuild `--bundle --format=iife`), evaluates it in the V8 host, and runs it via `run_system`. The
mutation (each mover's x advanced by its speed, hit counter ticked) is then **observable in the
derived World**, and the view the system retained on `globalThis` is confirmed **neutered** post-run.

## The gather/scatter copy (and when it disappears)

The World's hot columns are not yet VM-allocated, so this tier **copies** each queried column into a
`VmBuffer` before the run and back after. That copy is the deliberate bridge until the split-out
**VM-interior World columns** seam (R-LANG-008 / L-60, #88 item 5) makes the columns themselves
VM-allocated — at which point the views alias the columns directly and the copy vanishes. Structural
changes inside an executor (add/remove component, entity create/destroy) are **unsupported** (memory
may move under a live view); the **end-of-system command buffer** that defers them is a separate
split-out task (#88 item 3).

## Endianness

The record layout the accessors read/write is **little-endian**. Every CI/host target is
little-endian (x86_64 + ARM64), and the host gathers **native** scalar bytes into the `VmBuffer`, so
the LE `DataView` reproduces the host's native records bit-for-bit (the L-54 determinism law holds
across the 3-OS matrix). A future big-endian port would flip the endianness flag in
`accessor_codegen.cpp` and byte-swap in gather/scatter — out of scope here.

## Deliberately split out (clean follow-ups on this proven tier — tracked on #88)

- **Write-set enforcement** — a debug-build `Proxy` that throws on undeclared component access (#88
  item 2). This tier hands out only the declared-component views; the throw-on-undeclared half is split.
- **End-of-system command buffer** — deferred structural changes (#88 item 3).
- **WASM-tier analog** — `memory.grow` contract-invalidates the host-mapped linear-memory base (#88
  item 4).
- **VM-interior World columns** — removes the gather/scatter copy (#88 item 5).
- **The safe parallel scheduler** (R-LANG-011, task 4) — running disjoint-write systems concurrently.
  **Landed** in `editor/schedule` (#92): declared-access DAG (computed once/cached), non-conflicting
  native systems in parallel, TS on the single JS-VM lane.

## Layering + local vs CI

Depends on `context_component` (declarative component types), `context_kernel` (the raw World
storage), and `context_js` (the `runSystemView` gate). The library names only the `JsEngine`
**interface** (no V8 types), so it — and the accessor-codegen + query/gather/scatter unit tests —
build and run on **every** toolchain (the local Strawberry-GCC Windows `dev` gate included). Only the
in-V8 **keystone** links the rusty_v8 prebuilt (through `context_js`) + esbuild (through `context_ts`),
so it is **CI-only for its V8 dependency path**: the local `dev` gate builds the js stub and the
keystone takes its reduced-assertion branch (still proving the generated accessor is valid TS that
esbuild bundles). The **3-OS CI build legs are the authoritative gate** (profile `setup.md`/`test.md`
carve-out).
