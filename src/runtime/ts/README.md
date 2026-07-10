# `src/runtime/ts/` — the M3 TypeScript toolchain

The **build half** of the M3 TypeScript scripting tier (issue #83 / **L-61** /
**R-LANG-002/004**). It transpiles + bundles authored TypeScript into a single JavaScript
module that the `src/runtime/js/` in-process V8 host (task 2a) evaluates. This is **task
2b-i**: make authored TS *runnable* in the shipped VM. The declarative component-type
authoring (R-LANG-010) is task 2b-ii; the zero-copy view protocol (R-LANG-009) is task 3.

## What this package provides

- **`TsToolchain`** (`include/context/runtime/ts/ts_toolchain.h`) — the backend-agnostic,
  STL-only toolchain seam, mirroring `runtime/js`'s `JsEngine` seam.
- **`createEsbuildToolchain`** — the esbuild backend factory.
- `transpile(tsFilePath, opts)` — transpile a `.ts` file to a JS module; `opts.bundle`
  resolves + inlines its imports, `opts.format` picks the module wrapper (`Iife` self-executes
  at eval — the shape the V8 host runs; `Esm` emits bare `export`s); `opts.sourcemap` emits a
  **Source Map v3** alongside the JS (`TranspileResult::sourceMap`) — the R-OBS-005 foundation.
- Transpile/bundle **diagnostics** carrying stable **R-CLI-008 catalog codes**
  (`ts.transpile_failed` / `ts.bundle_failed`) so a CLI/RPC caller branches on the failure
  class without parsing text; plus the RUN-tier `ts.runtime_error` code (R-OBS-005) **registered**
  for an authored-TS throw's TS-source-mapped stack trace (the emit-path that composes the envelope
  is the deferred follow-up — see § Deferred seams).
- **`TsTypechecker`** (`include/context/runtime/ts/ts_typecheck.h`) — the backend-agnostic SEMANTIC
  typecheck seam (issue #85). esbuild transpiles by STRIPPING types without checking them, so a
  `--noEmit` typecheck is a **separate** tsc-class tool: **tsgo** (microsoft/typescript-go), a
  SHA-pinned native prebuilt (`tools/tsc-toolchain.json` + `tools/fetch_tsc.py`) invoked as a
  subprocess (`createTsgoTypechecker` → `check(tsFilePath)`). Each type error surfaces as a
  **`ts.type_error`** R-CLI-008 diagnostic carrying the tsc code (`TSxxxx`) and the authored `.ts`
  line:column — the agent's author→typecheck→fix loop. Like esbuild, tsgo runs on EVERY toolchain,
  so its ctest (`ts-test_ts_typecheck`) is a **LOCAL gate** (the loop converges on the dev machine,
  not only at CI).
- **`SourceMap`** (`include/context/runtime/ts/source_map.h`) — a Source Map v3 parser + resolver
  (base64-VLQ mappings, `sources`/`names`) that maps a GENERATED `(line, column)` in the bundle
  back to the authored `(source, line, column, name)`. STL-only, no V8 — a **LOCAL gate**.
- **`stack_trace`** (`include/context/runtime/ts/stack_trace.h`) — `parse_v8_stack` /
  `remap_stack` / `render_stack` / `resolve_ts_stack`: turn a raw V8 error `.stack` string into a
  **TS-resolved** trace (authored `.ts` positions) for the R-CLI-008 envelope + headless CLI
  output. Also STL-only, so the whole "JS stack → TS positions" path is locally testable.
- Authored-TS **examples** (`examples/game.ts` + `examples/util.ts`) — a gameplay entrypoint
  that exercises the host↔TS boundary **both ways**; plus `examples/throwing.ts` — a throw-at-load
  fixture whose V8 stack remaps back to authored TS (the R-OBS-005 end-to-end proof). Used by the
  tests.

## Supply chain — SHA-pinned esbuild prebuilt

esbuild is acquired as a **SHA-pinned third-party prebuilt** from esbuild's own per-platform
npm packages (`@esbuild/linux-x64` / `@esbuild/darwin-arm64` / `@esbuild/win32-x64`). This is
the **same wgpu-native / CEF / rusty_v8 precedent** codified in `docs/signing.md`: third-party
build tools are authenticated by **their own publisher** via **TLS + SHA-pin +
verify-before-use** and are **out of scope for the engine's first-party R-SEC-009 trust root**
— so they are deliberately **not** routed through `tools/verify_artifact.py`.

- Pins: `tools/ts-toolchain.json` (the single source of truth — esbuild **0.28.1**; per-platform
  package SHA-256 + the binary member path).
- Fetch/verify/stage: `tools/fetch_esbuild.py` (fail-closed; `--source` offline escape hatch;
  covered by `tools/tests/test_fetch_esbuild.py`).
- **Version-coherence** (every per-platform package carries the same esbuild version) is asserted
  at runtime by `fetch_esbuild.py` and statically by the tests.
- Wiring: `CMakeLists.txt` reads the manifest, invokes `fetch_esbuild.py` at configure time, and
  **`FATAL_ERROR`s (HALTs)** if the host has no pinned platform or verification fails.

## Local vs CI (build gate) — the key asymmetry with `runtime/js`

esbuild is a **build-TIME native binary invoked as a subprocess** — it is **never linked** into
the engine. So, unlike `runtime/js`'s rusty_v8 (an MSVC/Clang-ABI static lib the local
Strawberry-GCC Windows dev gate cannot link), **`context_ts` builds + runs on every toolchain**,
and its transpile ctest (`ts-test_ts_toolchain`) is a **local gate** — the transpile path is
fully exercised on the Windows dev host.

The **in-V8 integration** ctest (`ts-test_ts_in_v8`) links `context_js`, so it is **CI-only for
its V8 dependency path**: it runs the real "authored TS → esbuild → eval in V8 → host↔TS both
ways" flow on the 3-OS CI `build` legs, and on the local js-stub host takes a reduced branch
(esbuild still bundles; the V8 backend reports unavailable) so `ctest --preset dev` stays green.
The 3-OS CI `build` legs are the **authoritative gate** for the in-V8 flow.

## Deferred seams (documented; NOT built in task 2b-i)

Task 2b-i landed the DoD floor — *authored TS transpiles + runs in the V8 host + calls the host
both ways, green on 3 OS*. Two of its clean seams have since **LANDED** (issue #85):

- **Semantic typecheck loop** ✅ (R-LANG-002/004, R-CLI-008): a `tsc`-class `--noEmit` pass whose
  findings surface as the `ts.type_error` validation-class catalog code — the agent's
  author→typecheck→fix loop. esbuild deliberately does **not** typecheck (it transpiles + strips
  types), so this is a **separate** `tsc`-class tool: **tsgo** (microsoft/typescript-go), a
  SHA-pinned native prebuilt (`tools/tsc-toolchain.json` + `tools/fetch_tsc.py`) invoked as a
  subprocess — `ts_typecheck.h` / `tsgo_typecheck.cpp` (see § What this package provides). The
  `ts.type_error` code is now **registered** in `error_catalog.cpp` (was reserved).
- **Derivation-graph compile caching** ✅ (R-FILE-010): the TS→JS compile is now a content-addressed
  cached derivation node keyed on (source bytes + transpile options + toolchain version), so
  unchanged TS is not re-transpiled — `src/editor/derivation/ts_compile_node.h` (the peer of the
  shader-compile node, wrapping this package's `TsToolchain` seam).

The following remain intentionally left as clean seams:

- **R-LANG-010 declarative component-type TS accessors** (task 2b-ii): the codegen hook plugs in
  as an extra **generated input** to the bundle (the accessor `.ts`/`.d.ts` becomes an import the
  authored gameplay TS resolves) — `game.ts`'s `declare function hostBias(...)` stands in for the
  ambient engine typings that codegen will emit.
- **R-LANG-009 zero-copy view boundary** (task 3): the host↔TS boundary here is the task-2a
  doubles-only seam (`bindHostFunction` / `getFunction`); task 3 layers the per-system ArrayBuffer
  view protocol over `runtime/js`'s shape-B VM buffer on top of it.
- **R-SEC-005 engine-driven npm install** (task 5): resolving third-party npm packages the
  authored TS imports (with `--ignore-scripts`, lockfile-integrity, SHA pins) is a separate
  supply-chain surface. Task 2b-i bundles only first-party project `.ts` (esbuild `--bundle` over
  local imports); it installs nothing.
- **Interactive CDP inspector attach + source-mapped breakpoints** (R-OBS-005, the debugger half):
  task 4b lands the source-map + TS-resolved-stack-trace FOUNDATION (this package's `source_map` /
  `stack_trace` + the `ts.runtime_error` code + the V8 host capturing `error.stack`). Wiring V8's
  in-box CDP inspector (`v8-inspector.h`; the real session in `runtime/js/src/inspector.cpp`) through
  EditorKernel over a WebSocket/CDP transport — so a standard CDP client (Chrome DevTools / VS Code
  js-debug) attaches and hits a source-mapped breakpoint in authored `.ts` — is the **split-out
  follow-up** (issue #94's remaining core). It needs a NEW rusty_v8 extern-C shim for the
  STL-crossing `V8Inspector::create` (see `runtime/js/src/inspector.cpp` / `v8_rust_stubs.cpp`),
  which links only under the 3-OS CI V8 legs — none of it is buildable on the local GCC gate, so it
  is deliberately its own PR rather than folded in here. **Follow-up task.**
