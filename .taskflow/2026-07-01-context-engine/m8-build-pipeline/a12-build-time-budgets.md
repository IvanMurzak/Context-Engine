---
id: a12-build-time-budgets
title: Committed build-time budgets + CI build-time benchmark (R-BUILD-006)
group: a
sequence: 12
repo: "."
base_branch: "main"
depends_on: [ops1]
importance: 7
complexity: 5
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [R-BUILD-006, R-QA-009, R-QA-012, L-28, L-42, ROADMAP §1-M8]
---
## Goal
Commit the cold / incremental / clean-CI build-time budgets and enforce them with a CI
benchmark, with the per-build (cache-exempt) costs budgeted separately.

## Scope & seams
- Budget table (docs + machine-readable): from-source C++ compile vs the recurring per-build
  costs — per-platform transcode (a03) and the LTO/DCE final links (a05) — as separate lines;
  WASM-AOT/bytecode-precompile lines stay v2 (R-BUILD-006).
- Benchmark job over the a05/a06 pipeline under the R-QA-009 methodology (median-of-5, variance
  band, time-series archive); warm remote-cache-assisted CI path measured as the default, fully
  cold as the tracked worst case.
- **Dep note:** dev can start now — numbers run **advisory** until ops1 provisions the
  perf-isolated runner class (R-QA-012 advisory-until-provisioned); the BLOCKING flip is gated
  on ops1, not the code.

## Definition of Done
- [ ] Budgets committed + justified; benchmark job green and archiving a time series.
- [ ] Fleet manifest maps the gate to its runner class (advisory-until-provisioned recorded).
- [ ] Budget breach on a synthetic regression is detected (advisory red) in a test run.
