# CLAUDE.md

Context is a minimal-kernel game engine where **every feature is a package**, built AI-first without
making humans second-class. Project files are the single source of truth: the headless
**EditorKernel** (`src/editor/`) is a live derived index over them (the language-server model) —
every authored mutation is a file write, and GUI, CLI, and AI agents are equal clients over one
RPC/event surface, so the full authoring loop runs with no GPU and no GUI. Gameplay is authored in
TypeScript on an embedded V8 host; performance-critical code drops to the C++/WASM native tier.
Rendering targets WebGPU (native wgpu-native + the browser via Emscripten), 2D is first-class, and
determinism is a designed-in tier with a blocking cross-platform CI state-hash gate. Milestones
M0–M6 (foundations → editor-GUI observer → core gameplay systems) are complete; M7 (runtime UI) is
next. The normative design authority (requirements `R-*`, decision locks `L-*`, architecture,
roadmap) lives in the owner's design records **outside this repo** — in-repo `docs/` only records
how this repository implements them. Cite the relevant `R-`/`L-` id in PR bodies; never contradict
a locked decision — surface the conflict instead.

## Build & test

**All build files live in `src/`, not the repo root** — this trips everyone. Configure with an
explicit `-S src`; build/test presets resolve `CMakePresets.json` from the working directory, so run
those from `src/`:

```sh
cmake -S src --preset dev     # configure, from the repo root
cd src
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

- Requires CMake ≥ 3.25 and a C++20 compiler. The `dev` preset uses `$CMAKE_GENERATOR` if set
  (Ninja recommended). Builds land in `src/build/<preset>/` (gitignored). The `context` CLI binary
  lands at `src/build/dev/cli/context`.
- **vcpkg is opt-in.** A plain configure works with no vcpkg at all; the manifest (`src/vcpkg.json`)
  activates only when the vcpkg toolchain file is passed (the `spikes` / `shader-crosscompile`
  presets do this via `$VCPKG_ROOT`).
- Presets: `dev` (Release, the routine gate), `deterministic` (dev + engine-wide strict FP —
  produces the verified `deterministic:true` attestation), `sanitize` (ASan+UBSan, Debug),
  `tsan` (ThreadSanitizer), `spikes`, `shader-crosscompile`. `sanitize`/`tsan` are **non-Windows
  only** (their preset `condition` hides them on Windows; MSVC configure is a `FATAL_ERROR`) and
  mutually exclusive.
- Sanitizer mirror of CI: `cmake -S src --preset sanitize && cd src && cmake --build --preset
  sanitize && ctest --preset sanitize`.

### Windows local-dev nuance (CI-only dependency paths)

On the reference Windows dev setup the `dev` preset resolves to Ninja + **GCC** (Strawberry Perl's
toolchain), not MSVC. The heavyweight native dependencies ship **MSVC/Clang-ABI prebuilts that
cannot link under GCC** — V8 (`src/runtime/js/`), wgpu-native (`src/render/`,
`-DCONTEXT_BUILD_RENDER_WGPU=ON`), CEF (`src/editor/cef/` + `src/editor/gui/host/`,
`-DCONTEXT_BUILD_GUI_CEF=ON`), wasmtime (`src/runtime/wasm/`, `-DCONTEXT_BUILD_WASM=ON`). All are
**default-OFF CMake toggles**: with the toggle off the subdirectory early-returns or builds an
honest stub, so the local dev gate never fetches them, and the dedicated CI jobs are the
authoritative gates for those paths. Don't fight a local link failure on these under GCC — flip the
toggle only on MSVC/clang, or rely on CI.

A local GCC-green `dev` run is **not** proof of CI green. Recurring CI-only breaks:

- MSVC `/W4 /WX` rejects raw C stdio (`fopen`, `sprintf`, …) as C4996 — use C++ streams.
- Apple clang/libc++ overload resolution is stricter (e.g. `std::to_string` on a `std::chrono` rep
  is ambiguous) — cast to a concrete integer type first.
- Clang enables `-Wunused-const-variable`/`-Wunused-lambda-capture`/`-Wunused-private-field` by
  default; GCC does not — hand-audit new file-scope constants/captures/fields for actual use.
- Non-Windows `#if` branches (`__linux__`, `__APPLE__`, `#else // POSIX`) get zero local compile
  signal on Windows — hand-check them before pushing.
- Never return a user-defined C++ type by value across an extern-"C" boundary
  (Clang `-Wreturn-type-c-linkage`, MSVC C4190; GCC is silent).

## Test taxonomy & CI wiring rules

Tests are ctest registrations, named in families the CI steps select by prefix regex:
`kernel-test_*`, `gui-*` / `gui-a11y-*`, `ui-*` (runtime UI package, `src/packages/ui/`),
`render-ui-*` (M7 GPU UI backend, `src/render/ui/`), `render-present-*` (M9 e03 presentation path,
`src/render/present/`), `render-wgpu-*`, `shader-*`, `wasm-runner-*`,
`cef-substrate-*`, `editor-cef-smoke-*`, `client-*` (the M9 client SDK, `src/editor/client/`),
`editor-shell-*` (the M9 e04 native Shell, `src/editor/shell/`),
`webui-*` (the M9 e05a editor-core web workspace, `src/editor/webui/`),
`game-smoke-*`, `determinism-*`, `samples-corpus*`, and the
milestone exit gates `m1-exit-*`, `m2-exit-*`, `m4-exit-*`, `m5-exit-*`, `m6-exit-*`, `m7-exit-*`,
`m8-exit-*` (the M8 build-pipeline gate; -3/-4a/-4b are ALIASES of the a07 runtime-host / netsync
packed-wedge executables, the m6-exit-3 alias precedent), and `m85-exit-*` (the M8.5
wedge-hardening/v1 gate; -1a/-1b/-2a/-2b/-4a/-4b/-4c are likewise ALIASES of landed executables).
The `ui-*`,
`render-ui-*`, `render-present-*`, `editor-shell-*`, `client-*`, and `webui-*` families are plain
package test families (not gates) — NOT in the
general step's `-E` gate-exclusion regex, so they auto-run there, and the `build` job builds them via
`--preset dev` (no `--target`/CI edits needed). Note `^cli-` does NOT match `client-`.

- The `build` job's general ctest step **excludes the gate families**
  (`-E "^m1-exit-|^m2-exit-|^m4-exit-|^m5-exit-|^m6-exit-|^m7-exit-|^m8-exit-|^m85-exit-|^determinism-|^samples-corpus"`);
  each gate family runs exactly once in its own named blocking step so failures are legible per OS leg.
- **The "Not Run = RED" tripwire.** Several jobs build a hand-maintained explicit `--target` list
  instead of the whole tree: the `deterministic` job (`determinism-*` executables), the
  `wasm-runner` job (`wasm-runner-*` executables), and the `editor-cef-smoke` job (the `gui-a11y-*`
  a11y-step executables plus the M9 e04 `context_editor_shell_cef_smoke`). Registering a NEW ctest in any gate family means (1) its executable
  is built by that job's `--target` list in `ci.yml` AND (2) the named `ctest -R` step matches it —
  otherwise the regex finds the test but no executable exists and it reports "Not Run" → red.
  The golden-scene list in the `render`/`render-web` jobs is likewise hardcoded (dump + compare
  loops) — a new scene edits `ci.yml` too.
- **Tests are part of the feature (R-QA-013).** A behavior change merges only with the tests that
  pin it down — happy path, edge cases, and failure paths — in the SAME PR. Python tooling tests
  live in `tools/tests/` + `bench/tests/` (pytest, run per-PR). Spikes are exempt from full suites
  but must register a runnable ctest self-check.
- **Wall-clock/budget assertions must be sanitizer-aware in the same PR.** A test asserting a real
  time budget will overshoot by ~100x under TSan/ASan instrumentation — gate the ceiling behind a
  sanitizer compile define (see `CONTEXT_TSAN_BUILD` in `src/tests/integration/CMakeLists.txt`) so
  the dev gate enforces the real budget while instrumented legs stay green.

## CI structure (`.github/workflows/ci.yml`)

Two ~10-second gates front everything via `needs:`: `license-gate` (deny-by-default dependency
license check + CycloneDX SBOM artifact, `tools/check_licenses.py`) and `python-tests` (pytest over
`tools/tests` + `bench/tests` + fleet-manifest validation). Then, in one rollup (~34 checks):

- **`build` (ubuntu / macos / windows)** — blocking. Dev-preset build + the general ctest step +
  the named gate steps (M1/M2/M4/M5/M6/M7/M8 exit, determinism, samples-corpus) on every leg.
- **`deterministic` (3 OS)** — blocking. Strict-FP flavor (clang `-ffp-contract=off`, MSVC
  `/fp:strict`) + the `determinism-*` family, including the cross-platform golden state-hash wedge
  (Linux-x64 / Win-x64 / macOS-ARM64 — any per-platform divergence reds that leg).
- **`sanitize (ASan+UBSan, ubuntu)` + `sanitize (TSan, ubuntu)`** — blocking, Linux/clang only.
  Known false-positive class from the uninstrumented V8 prebuilt: `docs/sanitizer-v8-false-positives.md`.
- **`render`** — Linux is the blocking correctness gate (lavapipe software Vulkan: offscreen
  readback + the golden-scene SSIM corpus vs `goldens/` + min-spec bench); macOS is advisory.
  **There is deliberately NO Windows-native GPU leg — never add one** (Session-0 native-render
  teardown crashes; Windows coverage comes from the MSVC compile/link legs).
- **`render-web`** — blocking: same RHI compiled via Emscripten/emdawnwebgpu, run in headless
  Chromium + SwiftShader WebGPU, SSIM-gated against the same goldens.
- **`shader-crosscompile`** — Linux blocking; macOS/Windows advisory (`continue-on-error`).
- **`spike-wasm` / `spike-webgpu` / `spike-webgpu-web`, `wasm-runner`, `cef-substrate`,
  `editor-cef-smoke`** — the opt-in-toggle dependency paths, each blocking on its matrix (the
  a11y enforcement gate inside `editor-cef-smoke` blocks on Linux).
- **`editor-boundary`** — blocking, ubuntu. The D10 client-boundary gate (M9 e02): `cmake --install`
  the exported `context_client` closure to a staging prefix, build + run the standalone out-of-tree
  consumer (`src/editor/client/consumer/`) against it via `find_package(ContextClient)`, and run the
  transitive include-graph check (`tools/check_include_graph.py`) that forbids kernel-internal
  headers on the published surface. Since M9 e04 it also builds `context_editor` (the Shell — the
  SDK's first real consumer) and runs the `editor-shell-boundary` ctest over the configure-time D10
  link-closure audit. See `docs/client-sdk.md` + `docs/shell.md`. **Adding a library to the exported
  install set means adding it to this job's `--target` list too** — `cmake --install` fails on a
  target whose artifact was never built.
- **`bench-baseline` / `bench-attach-10k` / `build-time-bench` / `density-bench`** — the JOBS are
  blocking (harness must run green end-to-end); the perf NUMBERS are advisory (`continue-on-error`)
  until the perf-isolated runner class named in `docs/ci-fleet-manifest.json` is provisioned
  (`docs/perf-gate-methodology.md`). `build-time-bench` is the R-BUILD-006 build-time budget bench
  (M8 a12, `bench/build_time.py` + `bench/build-time-budget.json` + `docs/build-time-budget-table.md`):
  the from-source compile (cache-amortized) budgeted separately from the a03 transcode + a05 LTO/DCE
  link (cache-exempt, per-build). `density-bench` is the R-FILE-011 orchestration-density bench
  (M8.5 a21, `bench/density.py` + `bench/density-targets.json` + `docs/density-targets.md`): N packed
  `context_runtime_server` instances stepped/seeded/hashed in parallel from one controller —
  committed ticks/sec/instance + instances-per-box floors, advisory until the perf box (ops1)
  provisions; the full ladder runs nightly (`density-nightly` in `bench-nightly.yml`).

Windows legs of the heavy jobs ride a self-hosted MSVC runner (Ninja + `cl` via VsDevCmd) for
same-repo PRs. A separate `bench-nightly.yml` (schedule-only, advisory) runs the 100k corpus — it
never appears in a PR rollup. `docs/ci-fleet-manifest.json` is the machine-readable registry of
every gate; CI validates it against the live workflows on every run, so gate changes update it too.

## Authored-data conventions

- **Canonical JSON (R-FILE-001).** Authored files serialize to one canonical byte form (stable key
  order, number formatting, NFC). Two hashes per file: the **raw-byte hash** (change detection,
  CAS) and the **canonical-content hash** (derivation/cache keys) — cosmetic byte diffs never
  poison derivation. See `src/editor/serializer/README.md`.
- **Stable intra-file ids (L-33).** Every entity/instance carries `"id"`: random lowercase hex,
  16–32 chars, file-scoped, never sequential. Child collections are **arrays-of-objects-with-`id`**;
  map-keyed encoding is forbidden. Duplicate ids are a blocking diagnostic.
- **`context validate` exits 0 even when validation fails** — it reports through the JSON envelope.
  Assert the `"valid"` field, never the process exit code.
- `samples/` must stay a canonical fixpoint (`context migrate --dry-run` reports 0 changes), and
  in-project files enforce the L-33 hex-id form (standalone few-shot kind dirs are exempt).
- **A new authored content kind ships its `samples/` corpus entry in the same PR** (a
  `*.<kind>.json` carrying its `$schema` id), or the blocking samples-corpus gate reds all three
  build legs. **A new editor panel registers its a11y coverage in the same PR**
  (`src/editor/gui/a11y/`: `registry.cpp` + `coverage.manifest.jsonl` + the a11y CMake link).
- Golden-image rebaselines (`goldens/*.ppm`) are **reviewed changes, never automatic** — a golden
  failure with no intentional rendering change is a regression; fix the code (`goldens/README.md`).

## Repository map

| Directory | Purpose |
|---|---|
| `src/` | Engine source + **all build/lint files** (`CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `.clang-format`, `.clang-tidy`) |
| `src/kernel/` | The microkernel — ~6 stable interfaces (World/ECS, scheduler, module registry, event bus, resources, platform seam) |
| `src/common/` | Shared cross-cutting infra (hardened subprocess/temp-file runner) |
| `src/editor/` | **EditorKernel** — serializer, filesync, derivation, schema, compose, merge, migrate, assetdb, import, contract, bridge, **client** (the installed/exported client SDK), **shell** (the M9 native Shell — `context_editor`), **webui** (the M9 editor-core web workspace — build-time esbuild bundle, no Node at runtime), pkg, GUI (CEF host + headless panels) |
| `src/runtime/` | **RuntimeKernel** — session/determinism hashing, save, V8 JS host, TS toolchain, WASM migration runner, profiler, netsync |
| `src/render/` | Tiered RHI (WebGPU T1) + extract/double-buffer + the M9 present path (`present/`: surface/swapchain seam, OSR import, composite, CPU fallback) + opt-in wgpu-native and Emscripten web backends |
| `src/packages/` | First-party feature packages: spatial, simmath, physics3d/2d, particles, animation, spline, audio, input |
| `src/cli/` | The `context` CLI — a pure projection of the one contract registry (CLI ≡ RPC ≡ MCP ≡ introspection) |
| `src/testing/`, `src/tests/integration/` | Fault-injection harness; milestone exit gates + samples-corpus + game smokes |
| `samples/` | The maintained agent few-shot corpus — runnable projects, CI-gated (rots-if-broken) |
| `goldens/` | Committed golden-scene SSIM baselines (binary PPM) + `manifest.json` tolerances |
| `spikes/` | M0 de-risking spikes — **throwaway proof code, never production code**; opt-in builds |
| `bench/` | Scale-benchmark harness: seeded corpus generator + median-of-5 runner (Python) |
| `tools/` | Repo/CI tooling: license gate + SBOM, toolchain-manifest gate, artifact verification, golden compare, prebuilt fetchers |
| `cmake/` | Shared CMake modules (`ContextWarnings`, `ContextDownload`, `ContextCef`) |
| `docs/` | Engineering docs that live with the code (not design authority) |

## Key engineering docs (`docs/`)

`ci-fleet-manifest.md` (the R-QA-012 gate registry CI consumes) · `perf-gate-methodology.md` (when
a perf number may gate) · `physics-determinism-decision.md` (fixed-point Q16 sim core, the
sim-vs-presentation-observer split) · `sim-render-timing-contract.md` (fixed timestep +
interpolation) · `query-language.md` (the one query grammar) · `deprecation-policy.md` (the frozen
`protocolMajor 1` contract lifecycle) · `wgsl-tool-decision.md` (Tint, measured) ·
`client-sdk.md` (the `context_client` SDK, the subscription consumer, and the D10 boundary gate) ·
`present-path.md` (surface/swapchain, OSR import + composite, the CPU present fallback, and the
headless-invariant gate) · `shell.md` (the native Shell: windows, the single-threaded owner loop, the
per-window compositor + `PET_POPUP`, input arbitration, DPI, the D10 shell boundary, and the deferred
interactive-Windows verification) ·
`signing.md` (Ed25519 trust root, verify-before-use) · `toolchain-bootstrap.md` (the R-BUILD-008
fetchable-vs-preinstalled split per v1 target — what `context doctor` validates) ·
`versioned-install.md` (side-by-side
`versions/<semver>/` layout — a day-one contract) · `chunk-pack-format.md` (draft pack spec) ·
`self-hosted-runners.md` · `sanitizer-v8-false-positives.md` · `test-vector-corpus.md` ·
`latency-budget-table.md` / `human-latency-budget.md`.

## Engineering conventions

- **C++20**, extensions OFF, CMake ≥ 3.25. Formatting: `src/.clang-format` (LLVM base, 100 columns,
  Allman braces, 4-space indent, `PointerAlignment: Left`); there is no CI format gate — match the
  style either way.
- **Warnings-as-errors** via the `context_warnings` INTERFACE target (`cmake/ContextWarnings.cmake`:
  MSVC `/W4 /WX`, otherwise `-Wall -Wextra -Wpedantic -Werror`). Every new target links it
  `PRIVATE`. **Exemption: CEF-linking targets** (`src/editor/cef/`, `src/editor/gui/host/`,
  `src/editor/shell/cef/` + the CEF-linked `context_editor`) do not link it — CEF ships its own
  warning set; their headless dependencies still do.
- **Every feature is a package; the microkernel stays minimal.** Packages are STATIC libraries
  named `context_<name>` under `src/packages/`, composing on `context_kernel` (and `context_session`
  for sim components) — the kernel never links back. New engine features belong in a package, not
  the kernel. Deterministic sim state is integer/fixed-point only (`context_simmath` Q16);
  cosmetic/presentation state is an observer that folds into no state hash.
- **Dependencies are deny-by-default.** A new third-party dependency goes into `src/vcpkg.json` —
  or, for a web-layer npm dependency, into the editor-core `package.json` declaration
  (`check_licenses.py` scans every non-`node_modules` `package.json`) — AND into
  `tools/license-allowlist.json` (verified SPDX id — never guessed), or the license gate fails.
  Prefer the standard library; heavyweight prebuilts follow the SHA-pinned fetch + verify-before-use
  pattern (`cmake/ContextDownload.cmake`, `tools/fetch_*.py`).
- **No Apache/ASCII-art file headers** — this is a proprietary-EULA repo. Source files carry a
  short one-line descriptive top comment, matching their neighbors.
- **Conventional commits** (`feat:`, `fix:`, `build:`, `ci:`, `docs:`, `test:`, `chore:` …);
  reference issues in the body (`Closes #N`) and cite implemented `R-`/`L-` ids.

## Contributions

**External pull requests cannot be merged yet.** Context is source-available under the Context
Engine EULA (`LICENSE.md` — free under $200k/year gross revenue per product, 2% marginal royalty
above; not open source), and license enforceability requires a CLA with copyright assignment that
does not exist yet — external PRs are closed with a pointer to `.github/CONTRIBUTING.md` /
`.github/CLA.md` until the flow lands. Issues, bug reports, and design discussion are welcome.
