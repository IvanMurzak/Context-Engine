# `bench/baselines/` — committed rolling perf baselines (R-QA-009)

This directory holds the **rolling baselines** the `bench/perf_gate.py` gate compares fresh medians
against (rule 3 of [`docs/perf-gate-methodology.md`](../../docs/perf-gate-methodology.md)). Unlike
`bench/results/` and `bench/corpora/` (gitignored, generated), baselines are **committed** — a gate
needs a stable reference to breach against.

## What lives here

- `<scenario>-<corpus>.json` — one baseline per scenario/metric on a **named runner class**, written
  by `perf_gate.py --record`. Shape: `{scenario, metric, median, band_pct, runner_class, recorded_utc}`.

## The one rule

**Only record baselines from a trusted, perf-isolated runner class** (the
`perf-linux-bare-metal` class in `docs/ci-fleet-manifest.json`). Never commit a baseline measured on a
shared GitHub-hosted runner — a shared runner cannot satisfy perf-isolation, so its numbers are a
trend gauge only (the `bench-baseline` CI job runs the gate **advisory** for exactly this reason).

No baseline is committed yet: the `perf-linux-bare-metal` runner class is **not yet provisioned**, so
the M1 perf gate is advisory. When the perf box lands, record its baselines here and promote the gate
to blocking (see the maintenance note in `docs/ci-fleet-manifest.md`).
