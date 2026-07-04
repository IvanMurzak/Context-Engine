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
| `gpu-runner` | Linux-x64 + GPU | shared | **yes** | ❌ (M4) |
| `n-daemons-host` | Linux-x64 multi-worktree host | perf-isolated | no | ❌ (R-FILE-011) |

A shared GitHub-hosted runner **cannot** host an R-QA-009 performance floor (rule 1 of the
[perf-gate methodology](perf-gate-methodology.md)); those floors map to `perf-linux-bare-metal`.

## Red-X policy taxonomy (ROADMAP §6)

Every gate carries exactly one written red-X policy:

- **blocking** — merge stops when the gate red-Xes.
- **advisory** — reported + tracked, does **not** stop the merge. **Every gate whose runner class is
  not yet provisioned is advisory** (the R-QA-012 rule the validator enforces).
- **quarantine-with-issue** — a known-flaky gate, auto-quarantined WITH an owned issue, kept running +
  visible for trend, **never silently retried to green**.

## Gates (M1 snapshot)

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
| `sanitize-asan-ubsan` | L-38 | gh-ubuntu-shared | per-PR | blocking | `sanitize` |
| `sanitize-tsan` | L-38 | gh-ubuntu-shared | per-PR | blocking | `sanitize` |
| `bench-baseline-10k` | R-FILE-011 | gh-ubuntu-shared | per-PR | **advisory** | `bench-baseline` |
| `spike-wasm-interpreters` | R-LANG-003 | gh-ubuntu-shared | per-PR | blocking | `spike-wasm` |
| `spike-wasm-wamr-aot` | R-LANG-003 | gh-ubuntu-shared | per-PR | **quarantine** (#24) | `spike-wasm` |
| `spike-webgpu-native` | R-REND-005 | gh-ubuntu-shared | per-PR | blocking | `spike-webgpu` |
| `spike-webgpu-web` | R-REND-005 | gh-ubuntu-shared | per-PR | blocking | `spike-webgpu-web` |
| `fleet-manifest-validation` | R-QA-012 | gh-ubuntu-shared | per-PR | blocking | `python-tests` |
| `perf-filesync-attach-100k` | R-FILE-011 | perf-linux-bare-metal | nightly | **advisory** ⏳ | — |
| `determinism-state-hash` | R-QA-005 | determinism-matrix | per-PR | **advisory** ⏳ | — |
| `visual-equivalence` | R-REND-005 | gpu-runner | nightly | **advisory** ⏳ | — |
| `n-daemons-budget` | R-FILE-011 | n-daemons-host | nightly | **advisory** ⏳ | — |

⏳ = advisory **until its runner class is provisioned**. The R-QA-008 M1-exit suites (kernel,
file-sync, and the R-QA-010 fault-injection harness) ride the `build` gate — their ctest
registrations run on every `build (<os>)` leg. The five `m1-exit-*` gates are the ROADMAP §1
M1 Exit criteria themselves (issue #36): the `m1-exit-<n>-*` ctest family under
`src/tests/integration/`, run by the dedicated "M1 exit gate (5 criteria, blocking)" step of the
`build` job on all three OS legs (the runner class shown is the representative leg).

## How CI consumes it (the R-QA-012 tie)

`tools/check_fleet_manifest.py` runs in the `python-tests` job on every PR:

```bash
python3 tools/check_fleet_manifest.py --manifest docs/ci-fleet-manifest.json \
    --ci-workflow .github/workflows/ci.yml
```

It fails the build (exit 1) when the manifest drifts out of self-consistency, and exit 2 on a config
error. It enforces:

1. Every gate references a **declared runner class**.
2. Every `red_x_policy` is one of the three taxonomy values; every `tier` is `per-PR` or `nightly`.
3. **Advisory-until-provisioned**: a gate whose runner class has `provisioned: false` MUST be
   `advisory` (never blocking, never silently green).
4. Every `quarantine-with-issue` gate **names an `issue`**.
5. Every gate with a non-null `ci_job_id` maps to a **real job** in the live workflow (the
   consumption/sync check — the manifest cannot claim a CI job that does not exist).

`tools/tests/test_check_fleet_manifest.py` is the R-QA-013 coverage for the validator itself.

## Maintenance

- **Adding a CI gate** → add a manifest row (runner class + tier + red-X policy + `ci_job_id`) in the
  same PR; the validator fails if the `ci_job_id` has no matching job.
- **Provisioning a runner class** → flip `provisioned: true` and promote its advisory gates to
  `blocking` (and drop any `continue-on-error` in the workflow). Provisioning a runner class is a
  milestone de-risk item like any other dependency.
- **Bump `manifest_version`** on any schema change.
