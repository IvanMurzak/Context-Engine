# Performance-gate methodology (R-QA-009)

> Design authority: `.claude/design/context-engine/core/REQUIREMENTS.md` **R-QA-009** (normative), `ROADMAP.md` §6 CI
> tiering. This document is the **published methodology** every CI-enforced performance target runs
> under. It is the M1 stand-up of the gate discipline; the numeric floors it governs (R-FILE-011,
> R-LANG-012, R-QA-007, R-BUILD-006, the R-BRIDGE-008 session-query p99) attach to it as they land.

## Why a methodology, not just a number

A numeric floor enforced on a noisy shared runner with single-run sampling produces flake-driven
reverts and quietly widened budgets. The methodology is what makes the floors **real** rather than
aspirational. Every performance gate obeys the four rules below — no exceptions, no "some CI box".

## The four rules

1. **Named, perf-isolated runner classes.** A performance benchmark runs only on a runner class
   **named in the CI fleet manifest** (`docs/ci-fleet-manifest.json`, R-QA-012) whose `isolation` is
   `perf-isolated` — never a shared GitHub-hosted runner. The proxy device for each floor (R-QA-007)
   is named in the manifest, never implied.
2. **Median of N ≥ 5 runs, dispersion recorded.** Each result is the **median of at least 5 runs**,
   and the run-to-run dispersion (min / max / stdev / spread %) is recorded alongside it. The
   `bench/harness.py` runner already implements this (`--runs 5`, `dispersion()` — median + spread).
3. **Breach confirmed against a rolling baseline within a documented variance band.** A gate **fails
   only on a breach** — the fresh median exceeding the rolling baseline by more than the documented
   band. A single slow run never fails the gate; an improvement below the band is not a breach (it is
   a rebaseline signal). **Minimal v1 band: ±10%.**
4. **Results archived as a time series.** Every measured median is appended to a time-series archive
   so drift is **visible before it breaches**. Trend is a first-class output, not an afterthought.

## Minimal v1 (what stands up at M1)

Per R-QA-009's own minimal-v1 clause and ROADMAP M1:

- **One bare-metal Linux perf box** — the `perf-linux-bare-metal` runner class in the fleet manifest.
  It is **not yet provisioned**, so every gate mapped to it is **advisory until provisioned** (marked
  so in the manifest — visibly degraded, never silently green).
- **median-of-5** — via `bench/harness.py --runs 5`.
- **±10% band** — the default in `bench/perf_gate.py`.

Until the perf box is provisioned, the per-PR benchmark is the **10k-corpus proxy** on the shared
`gh-ubuntu-shared` runner (the `bench-baseline` CI job). That job is a **trend gauge + harness smoke**
(it proves the harness runs and the canonical fixpoint holds) — it is **advisory**, it does **not**
gate on the perf numbers, exactly because a shared runner cannot satisfy rule 1. The full 100k-file
floor runs on the perf box (nightly) once provisioned; see ROADMAP §6.

## The runnable gate — `bench/perf_gate.py`

`bench/perf_gate.py` is the runnable **±10%-band gate** layered on the median-of-5 harness. It reads a
`bench/harness.py` result JSON, extracts the median for a chosen scenario/metric, and classifies it
against a rolling baseline:

```bash
# 1. Measure (median of 5) — the existing harness:
python3 bench/harness.py --subject <benchmark-exe> --corpus <corpus> --runs 5 \
    --out bench/results/local-10k --label local-10k

# 2. Gate the median against the rolling baseline within the ±10% band:
python3 bench/perf_gate.py --result bench/results/local-10k.json \
    --scenario attach --baseline bench/baselines/attach-10k.json \
    --archive bench/results/archive

# Record / refresh the rolling baseline from a trusted perf-box run:
python3 bench/perf_gate.py --result bench/results/perfbox-100k.json \
    --scenario attach --baseline bench/baselines/attach-100k.json --record
```

Exit codes: **0** = within band (or improved, or recorded); **1** = breach (median above
baseline·(1+band)); **2** = configuration error (unreadable result/baseline, unknown
scenario/metric). Self-check: `python3 bench/perf_gate.py --selftest` asserts the band math.

Because M1 has no provisioned perf box, the CI wiring runs the gate **advisory** (the
`bench-baseline` job invokes it with `continue-on-error`): a breach is reported and archived but does
not red-X the rollup — matching rule 1 and the R-QA-012 "advisory until provisioned" contract. When
the perf box lands, flip the runner class to `provisioned` in the manifest and drop `continue-on-error`
to make the floor blocking.

## Baselines + the time-series archive

- **Rolling baseline** — `bench/baselines/<scenario>-<corpus>.json`: the current committed reference
  median for a scenario/metric on a named runner class. Refreshed with `--record` from a trusted
  perf-box run (never from a shared-runner number).
- **Time-series archive** — `--archive <dir>` appends one JSONL row per measurement
  (`{timestamp_utc, scenario, metric, median, spread_pct, runner_class}`), so drift is visible over time.
  Archive contents are generated artifacts (`bench/results/` is gitignored + uploaded as a CI
  artifact); the committed deliverable is the **baseline**, not the raw series.

## Red-X policy

Per ROADMAP §6, every gate carries one written red-X policy, recorded in the fleet manifest:

- **blocking** — merge stops on a confirmed breach (only once the gate's perf-isolated runner class is
  provisioned).
- **advisory** — reported + archived, does not stop the merge (every perf gate today, because the perf
  box is unprovisioned).
- **quarantine-with-issue** — a known-flaky gate auto-quarantined WITH an owned issue, never silently
  retried to green.

Nightly breaches file an issue against the owning milestone; they never widen the budget silently.
