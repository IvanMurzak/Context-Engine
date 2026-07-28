# CI fleet manifest (R-QA-012)

> Design authority: `.claude/design/context-engine/core/REQUIREMENTS.md` **R-QA-012** (normative), `ROADMAP.md` §6 CI
> tiering. The machine-readable manifest is `docs/ci-fleet-manifest.json`; **CI consumes it** via
> `tools/check_fleet_manifest.py` (run in the `ci-config-gate` job every PR). This document is the
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
| `build-time-proxy-measure` | R-BUILD-006 | gh-ubuntu-shared | per-PR | blocking | `build-time-bench` |
| `build-time-proxy-gate` | R-BUILD-006 | gh-ubuntu-shared | per-PR | **advisory** | `build-time-bench` |
| `density-proxy-measure` | R-FILE-011 | gh-ubuntu-shared | per-PR | blocking | `density-bench` |
| `density-proxy-gate` | R-FILE-011 | gh-ubuntu-shared | per-PR | **advisory** | `density-bench` |
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
| `fleet-manifest-validation` | R-QA-012 | gh-ubuntu-shared | per-PR | blocking | `ci-config-gate` |
| `ci-gating-topology` | R-QA-012 | gh-ubuntu-shared | per-PR | blocking | `ci-config-gate` |
| `perf-filesync-attach-100k` | R-FILE-011 | perf-linux-bare-metal | nightly | **advisory** ⏳ | `bench-100k-nightly` |
| `bench-query-p99` | R-BRIDGE-008 | perf-linux-bare-metal | nightly | **advisory** ⏳ | `bench-100k-nightly` |
| `bench-sustained-backpressure` | R-FILE-013 | perf-linux-bare-metal | nightly | **advisory** ⏳ | `bench-100k-nightly` |
| `build-time-budget` | R-BUILD-006 | perf-linux-bare-metal | nightly | **advisory** ⏳ | — |
| `density-targets` | R-FILE-011 | perf-linux-bare-metal | nightly | **advisory** ⏳ | `density-nightly` |
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

**The R-BUILD-006 build-time budgets (M8 a12):** the `build-time-bench` per-PR job (`build_time.py
measure` → `gate`) times the a05/a06 build pipeline under the same R-QA-009 discipline
(median-of-5, dispersion, a ±10% band, archived time series). The from-source C++ compile (amortized
by the L-28 / sccache cache) is budgeted SEPARATELY from the recurring per-build cache-exempt costs —
the a03 per-platform transcode and the a05 LTO/DCE final link. The MEASURE step blocks (the harness
runs green + archives); the budget GATE is advisory (`continue-on-error`) until the perf-isolated
`perf-linux-bare-metal` runner class is provisioned — mirroring the perf-gate pattern above. The WARM
remote-cache-assisted path is the per-PR default; the fully COLD worst case + the committed budget
table are the nightly `build-time-budget` floor. WASM-AOT + JS-VM bytecode-precompile are v2-with-iOS
(tracked, not budgeted). The normative allocation lives in
[`build-time-budget-table.md`](build-time-budget-table.md); the machine-readable copy is
`bench/build-time-budget.json`.

**The R-FILE-011 orchestration-density targets (M8.5 a21):** the `density-bench` per-PR job
(`density.py measure` → `gate`) runs the wedge-pillar-1 demo shape (ARCHITECTURE §1.1) — N headless
PACKED instances (the a06 `context_runtime_server` over a `context build` v1 pack) stepped, seeded,
and hashed in parallel from ONE controller over the R-QA-005 session surface — under the same
R-QA-009 discipline (median-of-5, dispersion, a ±10% band, archived time series). Two committed
floors fall out: **ticks/sec/instance** (single-instance rate, ≥ 100k on the packed demo-scenario
subject) and **instances-per-box** (≥ 16 with EVERY instance sustaining the 60 ticks/s R-SIM-002
floor). The MEASURE step blocks (every instance boots ok + steps exactly N ticks; the same-seed
determinism pair must be bit-identical); the floors GATE is advisory (`continue-on-error`) until the
perf-isolated `perf-linux-bare-metal` runner class is provisioned (ops1 deferred, owner ruling
2026-07-18). The nightly `density-nightly` job (`bench-nightly.yml`) runs the FULL ladder (1…32) as
the `density-targets` advisory trend row. Honesty: a METHODOLOGY subject (minimal packed scene, demo
scenario — not a game-workload claim), and vs GPU-vectorized simulators (Isaac/Brax/Madrona-class)
Context does NOT compete on raw samples/sec (the acknowledged-gaps row). The normative definitions
live in [`density-targets.md`](density-targets.md); the machine-readable copy is
`bench/density-targets.json`.

## How CI consumes it (the R-QA-012 tie)

`tools/check_fleet_manifest.py` runs in the `ci-config-gate` job on every PR (it moved out of
`python-tests` in issue #459 — `ci-config-gate` is one of the two deterministic gates that front the
rollup, while `python-tests` gates nothing):

```bash
python3 tools/check_fleet_manifest.py --repo-root .
python3 tools/check_ci_gating.py --repo-root .      # the ci-gating-topology row (issue #459)
```

Both R-QA-012 rows in the table above live in that one job: `fleet-manifest-validation` is the first
command, `ci-gating-topology` the second (which jobs may front the rollup via `needs:`).

That is the whole invocation: the gate resolves the manifest and BOTH workflow files from
`--repo-root` itself (per-PR gates live in `ci.yml`, the nightly benchmark gates in
`bench-nightly.yml`), and a default it cannot read is a hard configuration error rather than a silent
skip. `--ci-workflow` remains available and repeatable, but it now SUPPLEMENTS that default set
instead of replacing it — naming only `ci.yml` used to make the gate report seven confident-looking
FALSE violations (the seven nightly-tier rows), and a gate that cries wolf gets ignored, which is its
own silent-failure mode. It fails the build (exit 1) when the manifest drifts out of
self-consistency, and exit 2 on a config error. It enforces:

⚠ These numbers are the ones the gate's own failure messages cite (`… (rule 8, status-claim drift)`),
so they must stay aligned with the `Checks:` list in `tools/check_fleet_manifest.py`'s module
docstring — including the structural rule 1, which this list used to omit, leaving every number here
one behind the message a reader was following.

1. **Structural**: `manifest_version`, `runner_classes` and `gates` are present and well-typed.
2. Every gate references a **declared runner class**.
3. Every `red_x_policy` is one of the three taxonomy values; every `tier` is `per-PR` or `nightly`.
4. **Advisory-until-provisioned**: a gate whose runner class has `provisioned: false` MUST be
   `advisory` (never blocking, never silently green).
5. Every `quarantine-with-issue` gate **names an `issue`**.
6. Every gate with a non-null `ci_job_id` maps to a **real job** in the workflow **its TIER lives
   in** — a per-PR row's job in `ci.yml`, a nightly row's in `bench-nightly.yml`. Reading the two
   files as one pool left that split unasserted, so a nightly row could point at a per-PR job and
   pass. An explicit `--ci-workflow` file is attributed to the tier its basename names, so naming a
   file cannot collapse that split back into one pool either.
7. The **R-QA-007 `minspec_floors` table** is present and well-formed: every v1 platform row names
   a non-empty reference device, exactly one positive target rate, and a declared runner class,
   and the Android/iOS N/A scope notes stay stated.
8. **Prose status-claim drift (M9 x11)** — a `description` may not make a status claim the
   machine-readable truth contradicts. Rules 1-7 read the FIELDS and never the prose, and the cost
   was measured: after x7 fixed #437 and e12c-1 landed, the `editor-shell-cef-smoke` row still
   asserted *"macOS ctest registration DISABLED pending #437"*, so a **false status claim survived a
   full task cycle inside a CI-validated file with CI green throughout**. Three claim classes, each
   an assertion SHAPE rather than a keyword (e12c-2's corrected text mentions `DISABLED` while
   claiming the opposite and must stay green):
   - a row saying it is **pending / awaiting / blocked on `#N`** must record that issue in its own
     `issue` field;
   - a row saying something is **advisory until `<runner class>` is provisioned** must name a
     declared class that really is unprovisioned;
   - a row claiming a **registration is DISABLED** must correspond to a ctest that really carries a
     `DISABLED` property in the CMake sources.

   What rule 8 deliberately cannot see: the second half of that measured defect was prose repeating
   a root cause x7 had already disproved. No machine-readable field records "the reason we once
   believed", so that half stays a review responsibility. The third class also has a PRECONDITION
   worth stating: a `DISABLED` claim that names no *literally registered* ctest is caught only while
   NO test in the tree carries a `DISABLED` property, so one future legitimate quarantine restores the
   false green for exactly the row this rule was written for. `test_ctest_registrations_reads_the_live_tree`
   pins that precondition (`disabled == set()`) so it fails loudly rather than lapsing quietly.

`tools/tests/test_check_fleet_manifest.py` is the R-QA-013 coverage for the validator itself.

## Maintenance

- **Adding a CI gate** → add a manifest row (runner class + tier + red-X policy + `ci_job_id`) in the
  same PR; the validator fails if the `ci_job_id` has no matching job.
- **Provisioning a runner class** → flip `provisioned: true` and promote its advisory gates to
  `blocking` (and drop any `continue-on-error` in the workflow). Provisioning a runner class is a
  milestone de-risk item like any other dependency.
- **Bump `manifest_version`** on any schema change.
