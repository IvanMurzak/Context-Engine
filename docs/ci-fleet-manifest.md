# CI fleet manifest (R-QA-012)

> Design authority: `engine-design/REQUIREMENTS.md` **R-QA-012** (normative), `ROADMAP.md` §6 CI
> tiering. The machine-readable manifest is `docs/ci-fleet-manifest.json`; **CI consumes it** via
> `tools/check_fleet_manifest.py` (run in the `python-tests` job every PR). This document is the
> human-readable companion.

## What the manifest is

A **versioned** manifest that maps **every CI-enforced requirement to the named runner class that
enforces it** — OS/hardware, GPU presence, isolation class (perf-isolated vs shared), and build
flavor (dev / sanitizer / release / deterministic / spike). Without stating what CI *is*, an
unprovisioned gate becomes a silently-skipped gate. The manifest makes the fleet a designed,
versioned artifact and every gap **honest**: a gate whose runner class is **not yet provisioned** is
marked **advisory until provisioned** — visibly degraded, never silently green.

## Runner classes

| Runner class | OS / hardware | Isolation | GPU | Provisioned |
|---|---|---|---|---|
| `gh-ubuntu-shared` | ubuntu-latest, GitHub-hosted | shared | no | ✅ |
| `gh-macos-shared` | macos-latest, GitHub-hosted | shared | no | ✅ |
| `gh-windows-shared` | windows-latest, GitHub-hosted | shared | no | ✅ |
| `perf-linux-bare-metal` | Linux-x64, bare-metal perf box | **perf-isolated** | no | ❌ (R-QA-009 v1) |
| `determinism-matrix` | Linux-x64 · Win-x64 · macOS-ARM64 | shared | no | ❌ (M6) |
| `gpu-runner` | Linux-x64 + GPU | shared | **yes** | ❌ (real-GPU corpus leg) |
| `n-daemons-host` | Linux-x64 multi-worktree host | perf-isolated | no | ❌ (R-FILE-011) |
| `minspec-desktop-proxy` | R-QA-007 desktop floor device: Iris-Xe-class ultrabook (i5-1135G7-class, 16 GB) | perf-isolated | **yes** | ❌ (R-QA-007) |
| `minspec-web-proxy` | the desktop min-spec class + latest stable Chromium | perf-isolated | **yes** | ❌ (R-QA-007) |

A shared GitHub-hosted runner **cannot** host an R-QA-009 performance floor (rule 1 of the
[perf-gate methodology](perf-gate-methodology.md)); those floors map to `perf-linux-bare-metal`.

## Red-X policy taxonomy (ROADMAP §6)

Every gate carries exactly one written red-X policy:

- **blocking** — merge stops when the gate red-Xes.
- **advisory** — reported + tracked, does **not** stop the merge. **Every gate whose runner class is
  not yet provisioned is advisory** (the R-QA-012 rule the validator enforces).
- **quarantine-with-issue** — a known-flaky gate, auto-quarantined WITH an owned issue, kept running +
  visible for trend, **never silently retried to green**.

## Gates (M1 + M2 snapshot)

| Gate | Requirement | Runner class | Tier | Red-X policy | CI job |
|---|---|---|---|---|---|
| `license-gate` | O-7 | gh-ubuntu-shared | per-PR | blocking | `license-gate` |
| `python-tests` | R-QA-013 | gh-ubuntu-shared | per-PR | blocking | `python-tests` |
| `build-ubuntu` | R-QA-008 | gh-ubuntu-shared | per-PR | blocking | `build` |
| `build-macos` | R-QA-008 | gh-macos-shared | per-PR | blocking | `build` |
| `build-windows` | R-QA-008 | gh-windows-shared | per-PR | blocking | `build` |
| `m1-exit-files-as-truth` | L-19 | gh-ubuntu-shared | per-PR | blocking | `build` |
| `m1-exit-live-attach` | R-CLI-010 | gh-ubuntu-shared | per-PR | blocking | `build` |
| `m1-exit-crash-recovery` | R-FILE-004 | gh-ubuntu-shared | per-PR | blocking | `build` |
| `m1-exit-contract-parity` | R-CLI-009 | gh-ubuntu-shared | per-PR | blocking | `build` |
| `m1-exit-scope-enforcement` | R-SEC-007 | gh-ubuntu-shared | per-PR | blocking | `build` |
| `m2-exit-cli-authoring` | L-37 | gh-ubuntu-shared | per-PR | blocking | `build` |
| `m2-exit-schema-migration` | L-37 | gh-ubuntu-shared | per-PR | blocking | `build` |
| `m2-exit-flatten-content-units` | R-ASSET-005 | gh-ubuntu-shared | per-PR | blocking | `build` |
| `m2-exit-composed-write-provenance` | R-CLI-006 | gh-ubuntu-shared | per-PR | blocking | `build` |
| `m2-exit-per-payload-migration` | R-QA-011 | gh-ubuntu-shared | per-PR | blocking | `build` |
| `m2-exit-seam-checklist` | R-DATA-006 | gh-ubuntu-shared | per-PR | blocking | `build` |
| `sanitize-asan-ubsan` | L-38 | gh-ubuntu-shared | per-PR | blocking | `sanitize` |
| `sanitize-tsan` | L-38 | gh-ubuntu-shared | per-PR | blocking | `sanitize` |
| `bench-baseline-10k` | R-FILE-011 | gh-ubuntu-shared | per-PR | **advisory** | `bench-baseline` |
| `bench-attach-10k` | R-FILE-011 | gh-ubuntu-shared | per-PR | blocking | `bench-attach-10k` |
| `spike-wasm-interpreters` | R-LANG-003 | gh-ubuntu-shared | per-PR | blocking | `spike-wasm` |
| `spike-wasm-wamr-aot` | R-LANG-003 | gh-ubuntu-shared | per-PR | **quarantine** (#24) | `spike-wasm` |
| `spike-webgpu-native` | R-REND-005 | gh-ubuntu-shared | per-PR | blocking | `spike-webgpu` |
| `spike-webgpu-web` | R-REND-005 | gh-ubuntu-shared | per-PR | blocking | `spike-webgpu-web` |
| `render-offscreen` | R-REND-002 | gh-ubuntu-shared | per-PR | blocking | `render` |
| `render-web` | R-REND-002 | gh-ubuntu-shared | per-PR | blocking | `render-web` |
| `golden-scene-native-linux` | R-REND-002 | gh-ubuntu-shared | per-PR | blocking | `render` |
| `golden-scene-web-chromium` | R-REND-002 | gh-ubuntu-shared | per-PR | blocking | `render-web` |
| `m4-exit-headless-no-render` | R-HEAD-002 | gh-ubuntu-shared | per-PR | blocking | `build` |
| `m5-exit-walkthrough` | R-EDIT-001 | gh-ubuntu-shared | per-PR | blocking | `build` |
| `m5-exit-a11y-coverage` | R-A11Y-001 | gh-ubuntu-shared | per-PR | blocking | `build` |
| `m5-exit-seam-checklist` | R-EDIT-001 | gh-ubuntu-shared | per-PR | blocking | `build` |
| `minspec-floor-proxy-measure` | R-QA-007 | gh-ubuntu-shared | per-PR | blocking | `render` |
| `minspec-floor-proxy-gate` | R-QA-007 | gh-ubuntu-shared | per-PR | **advisory** | `render` |
| `minspec-floor-desktop` | R-QA-007 | minspec-desktop-proxy | nightly | **advisory** ⏳ | — |
| `minspec-floor-linux-server` | R-QA-007 | perf-linux-bare-metal | nightly | **advisory** ⏳ | — |
| `minspec-floor-web` | R-QA-007 | minspec-web-proxy | nightly | **advisory** ⏳ | — |
| `shader-crosscompile` | R-REND-005 | gh-ubuntu-shared | per-PR | blocking | `shader-crosscompile` |
| `fleet-manifest-validation` | R-QA-012 | gh-ubuntu-shared | per-PR | blocking | `python-tests` |
| `perf-filesync-attach-100k` | R-FILE-011 | perf-linux-bare-metal | nightly | **advisory** ⏳ | `bench-100k-nightly` |
| `bench-query-p99` | R-BRIDGE-008 | perf-linux-bare-metal | nightly | **advisory** ⏳ | `bench-100k-nightly` |
| `bench-sustained-backpressure` | R-FILE-013 | perf-linux-bare-metal | nightly | **advisory** ⏳ | `bench-100k-nightly` |
| `bench-dense-reference` | R-FILE-011 | gh-ubuntu-shared | nightly | **advisory** | `bench-100k-nightly` |
| `bench-multiworktree-contention` | R-FILE-010 | n-daemons-host | nightly | **advisory** ⏳ | `bench-100k-nightly` |
| `determinism-state-hash` | R-QA-005 | determinism-matrix | per-PR | **advisory** ⏳ | — |
| `deterministic-attestation` | R-SIM-005 | determinism-matrix | per-PR | blocking | `deterministic` |
| `samples-corpus` | R-QA-006 | gh-ubuntu-shared | per-PR | blocking | `build` |
| `visual-equivalence` | R-REND-005 | gpu-runner | nightly | **advisory** ⏳ | — |
| `n-daemons-budget` | R-FILE-011 | n-daemons-host | nightly | **advisory** ⏳ | `bench-100k-nightly` |

⏳ = advisory **until its runner class is provisioned**. The R-QA-008 M1-exit suites (kernel,
file-sync, and the R-QA-010 fault-injection harness) ride the `build` gate — their ctest
registrations run on every `build (<os>)` leg. The five `m1-exit-*` gates are the ROADMAP §1
M1 Exit criteria themselves (issue #36): the `m1-exit-<n>-*` ctest family under
`src/tests/integration/`, run by the dedicated "M1 exit gate (5 criteria, blocking)" step of the
`build` job on all three OS legs (the runner class shown is the representative leg). The six
`m2-exit-*` gates are the ROADMAP §1 M2 Exit criteria (issue #68): the `m2-exit-*` ctest family
under `src/tests/integration/`, run by the dedicated "M2 exit gate (5 criteria + seam audit,
blocking)" step of the `build` job on all three OS legs (representative leg shown) — the
milestone-closing mirror of the M1 gate.

**The M4 exit trio (issue #141, ROADMAP §1 M4 Exit):** `golden-scene-native-linux` +
`golden-scene-web-chromium` are the **golden-scene visual-equivalence corpus** (`goldens/` +
`tools/golden_compare.py` — mean block-SSIM, per-scene tolerances, rebaselines are REVIEWED
changes): every corpus scene rendered offscreen per backend — native wgpu on lavapipe (`render`
job) and the BROWSER's WebGPU in headless Chromium + SwiftShader (`render-web` job,
`tools/web_golden_run.py`) — per the minimal-v1 ruling (Linux-Vulkan + one browser blocking,
others advisory; `visual-equivalence` remains the real-GPU nightly leg, advisory until
`gpu-runner` provisions). `m4-exit-headless-no-render` is the R-HEAD-002/004 headless criterion
(the "M4 exit gate" step of the `build` job, all three legs). The `minspec-floor-*` rows are the
**R-QA-007 committed floor table** — reference device + target rate per v1 platform live in the
manifest's `minspec_floors` section (Android trailing / iOS v2 = explicitly N/A); the per-PR
proxy pair measures the lit3d subject on lavapipe under the R-QA-009 discipline
(`bench/minspec_floor.py`, measure blocking / floor gate advisory-until-provisioned), and each
platform's nightly floor row activates when its named runner class provisions.

**The M5 exit trio (issue #168, ROADMAP §1 M5 Exit):** the three `m5-exit-*` gates are the
observer-editor milestone-closing mirror of the M1/M2/M4 gates — the `m5-exit-*` ctest family under
`src/tests/integration/`, run by the dedicated "M5 exit gate" step of the `build` job on all three OS
legs (representative leg shown), driving the LANDED headless editor-GUI panels with no CEF / no GPU /
no daemon. `m5-exit-walkthrough` is the headless end-to-end user journey (open a project → inspect via
scene-tree F2 / inspector F3 / Problems F4 → play F5 → override-edit F3 → undo F7 → the F1 viewport
observes the SAME render snapshot; the R-HUX-011 human-loops are measured from instrumented timestamps,
a SHOULD recorded in `docs/human-latency-budget.md`). `m5-exit-a11y-coverage` asserts every editor
panel is a11y-clean + keyboard-navigable AND the a11y registry matches `coverage.manifest.jsonl` (the
exit registered the historically-missing F4 Problems, completing the manifest). `m5-exit-seam-checklist`
is the executable M5 seam audit (one assertion per seam). The **sibling** M5 CI-job gates the exit
references are `editor-cef-smoke` (the per-OS CEF host boot smoke + the axe-class DOM a11y re-scan via
`tools/a11y_scan.py`, Linux blocking) and the M4 golden-scene rows above (the native-vs-web WebGPU
visual-equivalence within the T1 feature set — the `viewport` scene is the M5-F1 observer composite).

**The two-tier R-FILE-011 benchmark (issue #38, ROADMAP §6 tiering):** `bench-attach-10k` is the
**per-PR blocking 10k proxy** on the REAL attach pipeline (filesync reconcile index + derivation
graph + daemon boot via the `context bench` subject) — the JOB blocks (the real path must run
green + the per-stage budget-table extraction must succeed), while the perf NUMBERS stay advisory
(in-job perf gate is `continue-on-error`) because a shared runner cannot satisfy R-QA-009 rule 1.
The nightly `bench-100k-nightly` job (`.github/workflows/bench-nightly.yml`) runs the FULL 100k
benchmark + the dense-reference, session-query-p99, sustained-backpressure, and N-daemons
scenarios as an **advisory trend** on `gh-ubuntu-shared`; the nightly gates' `runner_class`
columns name the class that will make each a real floor once provisioned (never silently green).
The normative budget allocation lives in [`latency-budget-table.md`](latency-budget-table.md).

## How CI consumes it (the R-QA-012 tie)

`tools/check_fleet_manifest.py` runs in the `python-tests` job on every PR:

```bash
python3 tools/check_fleet_manifest.py --manifest docs/ci-fleet-manifest.json \
    --ci-workflow .github/workflows/ci.yml \
    --ci-workflow .github/workflows/bench-nightly.yml
```

(`--ci-workflow` is repeatable: per-PR gates live in `ci.yml`, the nightly benchmark gates in
`bench-nightly.yml`; a `ci_job_id` must exist in at least one given workflow.) It fails the build
(exit 1) when the manifest drifts out of self-consistency, and exit 2 on a config error. It
enforces:

1. Every gate references a **declared runner class**.
2. Every `red_x_policy` is one of the three taxonomy values; every `tier` is `per-PR` or `nightly`.
3. **Advisory-until-provisioned**: a gate whose runner class has `provisioned: false` MUST be
   `advisory` (never blocking, never silently green).
4. Every `quarantine-with-issue` gate **names an `issue`**.
5. Every gate with a non-null `ci_job_id` maps to a **real job** in the live workflow (the
   consumption/sync check — the manifest cannot claim a CI job that does not exist).
6. The **R-QA-007 `minspec_floors` table** is present and well-formed: every v1 platform row names
   a non-empty reference device, exactly one positive target rate, and a declared runner class,
   and the Android/iOS N/A scope notes stay stated.

`tools/tests/test_check_fleet_manifest.py` is the R-QA-013 coverage for the validator itself.

## Maintenance

- **Adding a CI gate** → add a manifest row (runner class + tier + red-X policy + `ci_job_id`) in the
  same PR; the validator fails if the `ci_job_id` has no matching job.
- **Provisioning a runner class** → flip `provisioned: true` and promote its advisory gates to
  `blocking` (and drop any `continue-on-error` in the workflow). Provisioning a runner class is a
  milestone de-risk item like any other dependency.
- **Bump `manifest_version`** on any schema change.
