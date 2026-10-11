# AGENTS.md

Context is a minimal-kernel C++ game engine where **every feature is a package**, built AI-first
without making humans second-class. Project files are the single source of truth: the headless
**EditorKernel** (`src/editor/`) is a live derived index over them (the language-server model) —
every authored mutation is a file write, and GUI, CLI and AI agents are equal clients over one
RPC/event surface, so the full authoring loop runs with no GPU and no GUI. Gameplay is TypeScript on
an embedded V8 host; performance-critical code drops to the C++/WASM native tier. Rendering targets
WebGPU (native wgpu-native + the browser via Emscripten), 2D is first-class, and determinism is a
designed-in tier with a blocking cross-platform CI state-hash gate. M0–M8.5 are shipped; M9 (the
interactive editor application) is in progress.

**Design authority** lives in this repo's `.taskflow/2026-07-01-context-engine/`: `core/`
(`REQUIREMENTS.md` `R-*`, `DESIGN-DECISIONS.md` locks `L-*`, `ARCHITECTURE.md`, `ROADMAP.md`) plus
one folder per milestone design (`m9-editor/` etc.). In-repo `docs/` only records how the code
implements it. Cite the relevant `R-`/`L-` id in PR bodies; never contradict a locked decision —
surface the conflict instead.

## Layout

| Path | What lives there / what goes there |
|---|---|
| `src/` | All engine source **and all build/lint files** (`CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `.clang-format`, `.clang-tidy`). Map: `src/AGENTS.md` |
| `samples/` | The CI-gated agent few-shot corpus of runnable projects (a new content kind adds an entry) |
| `goldens/` | Golden-scene SSIM baselines (binary PPM) + `manifest.json` tolerances |
| `spikes/` | M0 de-risking spikes — **throwaway proof code, never production code**; opt-in builds |
| `bench/` | Scale/perf benchmark harness (Python) + budgets; tests in `bench/tests/` |
| `tools/` | Repo/CI tooling (Python gates, SHA-pinned fetchers, pins). See `tools/AGENTS.md` |
| `cmake/` | Shared CMake modules (`ContextWarnings`, `ContextDownload`, `ContextCef`, …) |
| `docs/` | Engineering docs that live with the code (not design authority); new as-built write-ups go here |
| `third_party/` | Vendored third-party sources/fonts + verbatim upstream licenses |
| `.taskflow/` | Design records and implementation ledgers (see above) |
| `.github/` | `workflows/ci.yml` (the PR rollup), `bench-nightly.yml`, `release-sign.yml`, composite actions |

## Build & test

**All build files live in `src/`, not the repo root.** Configure with explicit `-S src`; build/test
presets resolve `CMakePresets.json` from the working directory, so run those from `src/`:

```sh
cmake -S src --preset dev     # configure, from the repo root
cd src
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

- CMake ≥ 3.25, a C++20 compiler; `dev` uses `$CMAKE_GENERATOR` if set (Ninja recommended). Builds
  land in `src/build/<preset>/` (gitignored); the `context` CLI at `src/build/dev/cli/context`.
- **vcpkg is opt-in**: a plain configure needs none; `src/vcpkg.json` activates only with the vcpkg
  toolchain file (the `spikes` / `shader-crosscompile` presets pass it via `$VCPKG_ROOT`).
- Presets: `dev` (Release, the routine gate), `deterministic` (strict FP — produces the verified
  `deterministic:true` attestation), `sanitize` (ASan+UBSan, Debug), `tsan`, `spikes`,
  `shader-crosscompile`. `sanitize`/`tsan` are **non-Windows only** and mutually exclusive. CI
  mirror: `cmake -S src --preset sanitize && cd src && cmake --build --preset sanitize && ctest --preset sanitize`.

**Windows local dev:** `dev` resolves to Ninja + **GCC** (Strawberry Perl), not MSVC. V8
(`src/runtime/js/`), wgpu-native (`src/render/`, `-DCONTEXT_BUILD_RENDER_WGPU=ON`), CEF
(`src/editor/cef/` + `src/editor/gui/host/`, `-DCONTEXT_BUILD_GUI_CEF=ON`) and wasmtime
(`src/runtime/wasm/`, `-DCONTEXT_BUILD_WASM=ON`) ship MSVC/Clang-ABI prebuilts that cannot link
under GCC. The three flags are **default-OFF** and V8 auto-enables only on toolchains that can link
it (off = early return or honest stub); their CI jobs are the authoritative gates. Don't fight a local GCC link failure there — flip the toggle only on
MSVC/clang, or rely on CI. A local GCC-green `dev` run is **not** proof of CI green (the recurring
MSVC/Clang-only breaks are listed in `src/AGENTS.md`).

## Tests and CI wiring

Tests are ctest registrations in prefix families that CI selects by regex: `kernel-test_*`, `gui-*` /
`gui-a11y-*`, `ui-*` (`src/packages/ui/`), `render-ui-*` (`src/render/ui/`), `render-present-*`
(`src/render/present/`), `render-wgpu-*`, `shader-*`, `wasm-runner-*`, `cef-substrate-*`,
`editor-cef-smoke-*`, `client-*` (`src/editor/client/`), `editor-shell-*` (`src/editor/shell/`),
`editor-session-*` (`src/tests/integration/`), `webui-*` (`src/editor/webui/`), `game-smoke-*`,
`determinism-*`, `samples-corpus*`, and the milestone exit gates `m1-exit-*`, `m2-exit-*`,
`m4-exit-*` … `m8-exit-*`, `m85-exit-*` (some `m8`/`m85` entries are ALIASES of landed executables).
Note `^cli-` does NOT match `client-`.

- The `build` job's general ctest step **excludes the gate families**
  (`-E "^m1-exit-|^m2-exit-|^m4-exit-|^m5-exit-|^m6-exit-|^m7-exit-|^m8-exit-|^m85-exit-|^determinism-|^samples-corpus"`);
  each gate runs once in its own named step. Plain package families (`ui-*`, `render-ui-*`,
  `render-present-*`, `editor-shell-*`, `editor-session-*`, `client-*`, `webui-*`) auto-run in the
  general step — no CI edit needed.
- **"Not Run = RED" tripwire.** The `deterministic` (`determinism-*`), `wasm-runner`
  (`wasm-runner-*`) and `editor-cef-smoke` (`gui-a11y-*` + `context_editor_shell_cef_smoke`) jobs
  build an explicit `--target` list. A new ctest in a gate family needs its executable in that
  job's `--target` list in `ci.yml` AND a match in the named `ctest -R` step, or it reports
  "Not Run" → red. The golden-scene list in `render`/`render-web` is hardcoded too.
- **Tests are part of the feature (R-QA-013):** happy path, edge cases and failure paths in the SAME
  PR. Python tests: `tools/tests/` + `bench/tests/` (pytest, per PR). Spikes need only a runnable
  ctest self-check.
- **Wall-clock budget assertions must be sanitizer-aware in the same PR** (~100x overshoot under
  TSan/ASan): gate the ceiling behind a sanitizer define (`CONTEXT_TSAN_BUILD` in
  `src/tests/integration/CMakeLists.txt`).

## CI structure (`.github/workflows/ci.yml`)

- Two deterministic ~10 s gates front everything via `needs:`: `license-gate`
  (`tools/check_licenses.py` + SBOM) and `ci-config-gate` (`tools/check_fleet_manifest.py` +
  `tools/check_ci_gating.py`). Only checks with no network, subprocess, timing or ports may sit in
  a gate job.
- `python-tests` is a blocking check that **gates nothing** (issue #459): it has timing flake
  surface, and a `needs:` edge would skip every dependent job. `check_ci_gating.py` enforces this.
- Counts: 24 job definitions (3 unfronted + 21 fronted, expanding to 39 of the rollup's 42 checks).
  Keep these in step with `ci.yml`'s header comment and `check_ci_gating.py` when adding a job/leg.
- Blocking: `build` (3 OS: dev build + general step + every named gate step), `deterministic`
  (3 OS, cross-platform golden state-hash), `sanitize` (ASan+UBSan, TSan; Linux/clang — V8
  false-positive class: `docs/sanitizer-v8-false-positives.md`), `render` (Linux lavapipe; macOS
  advisory), `render-web` (Emscripten + headless Chromium), `shader-crosscompile` (Linux; others
  advisory), the opt-in-toggle jobs (`spike-*`, `wasm-runner`, `cef-substrate`, `editor-cef-smoke`),
  `editor-boundary` (see `src/editor/AGENTS.md`). Bench jobs must run green; their numbers are
  advisory until the perf runner exists (`docs/perf-gate-methodology.md`).
- **There is deliberately NO Windows-native GPU render leg — never add one.**
- Windows legs of heavy jobs use a self-hosted MSVC runner (`docs/self-hosted-runners.md`).
  `bench-nightly.yml` is schedule-only and never in the PR rollup.
- `docs/ci-fleet-manifest.json` registers every gate and is validated against the live workflows
  on every run — gate changes update it too.

## Authored-data conventions

- **Canonical JSON (R-FILE-001):** one canonical byte form per file; a raw-byte hash (change
  detection, CAS) and a canonical-content hash (derivation keys). See `src/editor/serializer/README.md`.
- **Stable intra-file ids (L-33):** every entity/instance carries `"id"` — random lowercase hex,
  16–32 chars, file-scoped, never sequential. Child collections are arrays-of-objects-with-`id`;
  map-keyed encoding is forbidden; duplicate ids are a blocking diagnostic.
- **`context validate` exits 0 even when validation fails** — assert the envelope's `"valid"` field.
- `samples/` stays a canonical fixpoint (`context migrate --dry-run` reports 0 changes) and uses
  L-33 ids in-project. **A new authored content kind ships a `samples/` entry** (`*.<kind>.json`
  with its `$schema` id) in the same PR, or the samples-corpus gate reds every build leg.
- Golden rebaselines (`goldens/*.ppm`) are **reviewed changes, never automatic** — an unintended
  golden failure is a regression (`goldens/README.md`).

## Dependencies

Deny-by-default. A new third-party dependency goes into `src/vcpkg.json` (or, for a web-layer npm
dependency, the editor-core `package.json` declaration) **and** into `tools/license-allowlist.json`
with a verified SPDX id — never guessed — or the license gate fails. Prefer the standard library;
heavyweight prebuilts and configure-time fetches follow the SHA-pinned, verify-before-use pattern
(`cmake/ContextDownload.cmake`, `tools/fetch_*.py`; mirror rules in `tools/AGENTS.md`).

## Conventions

- **C++20**, extensions OFF; style per `src/.clang-format` (no CI format gate — match it anyway).
- **Every feature is a package; the microkernel stays minimal** (`src/packages/AGENTS.md`).
- **No Apache/ASCII-art file headers** (proprietary EULA): a short one-line descriptive top
  comment, matching neighbours.
- **Conventional commits** (`feat:`, `fix:`, `build:`, `ci:`, `docs:`, `test:`, `chore:` …);
  `Closes #N` in the body; cite implemented `R-`/`L-` ids.

## Key engineering docs (`docs/`)

`ci-fleet-manifest.md` (R-QA-012 gate registry) · `perf-gate-methodology.md` (when a perf number
may gate) · `physics-determinism-decision.md` (Q16 fixed-point sim core, sim vs presentation
observer) · `sim-render-timing-contract.md` · `query-language.md` · `deprecation-policy.md` (frozen
`protocolMajor 1` lifecycle) · `wgsl-tool-decision.md` · `client-sdk.md` (`context_client`, D10
boundary gate) · `asset-delete-and-restore.md` · `editor-session-state.md` · `editor-ui-bus.md` ·
`package-facts.md` (package fact bus + cross-package grants) · `present-path.md` · `shell.md` (native
Shell) · `signing.md` (Ed25519 trust root) · `toolchain-bootstrap.md` (what `context doctor`
validates) · `versioned-install.md` · `chunk-pack-format.md` · `self-hosted-runners.md` ·
`sanitizer-v8-false-positives.md` · `cef-keychain-isolation.md` (why CEF smokes pass
`--use-mock-keychain`) · `test-vector-corpus.md` · `latency-budget-table.md` /
`human-latency-budget.md` · `build-time-budget-table.md` · `density-targets.md`.

## Contributions

**External pull requests cannot be merged yet.** Context is source-available under the Context
Engine EULA (`LICENSE.md`; not open source) and needs a CLA with copyright assignment that does not
exist yet — external PRs are closed with a pointer to `.github/CONTRIBUTING.md` / `.github/CLA.md`.
Issues, bug reports and design discussion are welcome.
