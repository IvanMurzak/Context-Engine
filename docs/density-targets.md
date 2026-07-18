# Orchestration-density targets (R-FILE-011 — M8.5 a21)

> Design authority: `engine-design/REQUIREMENTS.md` **R-FILE-011** (sim-throughput /
> orchestration-density targets, re-anchored 2026-07-15 to no later than the M8.5 exit),
> **R-QA-005** (headless session surface), **R-QA-009** (perf-gate methodology), **R-QA-012**
> (fleet-manifest advisory-until-provisioned), **R-SIM-002** (fixed-timestep contract),
> ARCHITECTURE §1.1 (wedge pillar 1). This document is the **normative definition +
> justification** of the committed orchestration-density targets. The machine-readable copy CI
> consumes is `bench/density-targets.json`; the runnable controller harness + gate is
> `bench/density.py`.

## What is being committed

The wedge's RL/server-sim pillar (ARCHITECTURE §1.1, pillar 1) is **parallel headless instance
orchestration**: many engine instances stepped / seeded / hashed in parallel from ONE controller
over the R-QA-005 session surface. Its re-anchored commitment is two concrete numbers, committed
no later than the M8.5 exit:

| metric | committed floor | definition |
|---|---|---|
| **ticks/sec/instance** | **≥ 100,000** | the SINGLE-instance simulation rate on the packed bench subject: `--ticks / instance-wall`, rung N=1, median-of-runs, **process boot + pack load included** (the honest orchestration cost of a fleet spawn). |
| **instances-per-box** | **≥ 16** | the largest measured instance rung where **EVERY** instance — the straggler, not the average — still sustains the real-time floor of **60 ticks/s** (the same 60 Hz the R-SIM-002 fixed-timestep contract and the fleet manifest's `linux-server` minspec row commit). |

Both are **floors** (one-sided: only slower/fewer is a breach; faster is never a failure),
compared within the R-QA-009 ±10% band. For instances-per-box the band applies to the *sustain
criterion* (a rate), not the integer rung count — a breach is confirmed only when even the
band-relaxed sustain floor yields fewer rungs than committed (no single-run flake failures).

## The bench subject (stated honestly)

The subject is the **a06 packed server artifact**: `context_runtime_server` booting a
`context build` v1 pack and stepping the shipped RuntimeKernel **demo-scenario** session
(`--pack/--ticks/--seed/--scenario` — the packed projection of the R-QA-005 session surface).
`bench/density.py` is the controller: it seeds each instance (`seed-base + i`), launches the
rung's N instances in parallel, and collects each instance's `simTick` + L-54 `simStateHash`.

Two honesty properties are enforced, not assumed:

- **Determinism pre-flight** — a same-seed pair must report bit-identical
  `simStateHash`/`worldHash`/`simTick` before any rate is recorded, and every measured
  `(seed, ticks)` pair must reproduce the same hash across rungs and runs. A rate measured over
  a nondeterministic sim would be a number about nothing.
- **Methodology subject, not a game-workload claim** — the packed scene is minimal and the
  demo-scenario systems are deliberately light (the per-tick cost is sub-microsecond). These
  numbers commit the *orchestration + session-surface overhead envelope*; a representative
  packed **game** workload rides the same harness (same metrics, same gates) once one is
  committed. Stating the committed floors as game-workload throughput would be dishonest —
  this table does not.
- **Competitor honesty (the acknowledged-gaps row)** — vs GPU-vectorized simulators
  (Isaac/Brax/Madrona-class) Context does **NOT** compete on raw samples/sec. The pillar-1
  pitch is **agent-authored environments + CPU-parallel commodity orchestration** — many cheap
  headless instances, no GPU.

## Methodology (R-QA-009)

- **median-of-N ≥ 5** runs per rung, dispersion recorded (`rate_median`, `rate_min`,
  `batch_wall_seconds` each carry min/max/stdev/spread).
- **±10% band** vs the committed floor; breaches confirmed against the band, never single-run.
- **time series** — `density.py gate --archive` appends one JSONL row per metric
  (`density-ticks-per-sec-per-instance.jsonl`, `density-instances-per-box.jsonl`), archived as
  CI artifacts every run so drift is visible before it breaches.
- **named runner class** — the committed floors are stated for the perf-isolated
  `perf-linux-bare-metal` runner class (R-QA-012).

## Runner-class honesty (ops1 deferred — advisory until provisioned)

**The perf-isolated runner class is NOT provisioned** (ops1 deferred, owner ruling 2026-07-18).
Per R-QA-012, every numeric gate mapped to it is **advisory until provisioned** — visibly
degraded, never silently green:

- **per-PR** (`ci.yml` job `density-bench`): the MEASURE step is **blocking** (the controller
  + N packed instances must run green end-to-end, determinism pair verified); the numeric GATE
  is **advisory** (`continue-on-error`) — a shared GH runner is not the named perf box.
- **nightly** (`bench-nightly.yml` job `density-nightly`): the full ladder (1…32) on the same
  advisory/trend basis, archiving the long-run time series.
- The blocking flip is a FUTURE step gated on ops1: provision the box, set the runner class
  `provisioned` in `docs/ci-fleet-manifest.json`, drop `continue-on-error`, and ratify (or
  tighten) the committed floors by reviewed PR.

## Initial advisory measurements (2026-07-18, NOT the committed basis — headroom evidence)

| host | rung N=1 rate | deepest rung | straggler at deepest | capacity @ 60 t/s |
|---|---|---|---|---|
| local dev box (win-x64, 32 threads, non-isolated) | ~4.3×10⁶ t/s | 32 | ~3.5×10⁵ t/s | 32 (ladder-capped) |

The committed floors carry >40× (rate) and 2× (density, ladder-capped) headroom against these
advisory measurements, so they act as regression tripwires rather than aspirations, while the
perf box will state the ratified numbers.

## Revision policy

The committed floors are the contract. They change only by reviewed PR (like
`bench/build-time-budget.json`), and MUST be re-ratified when `perf-linux-bare-metal` is
provisioned or when a representative packed game workload joins the subject set.
