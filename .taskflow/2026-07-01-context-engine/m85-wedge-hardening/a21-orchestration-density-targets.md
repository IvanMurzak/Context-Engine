---
id: a21-orchestration-density-targets
title: Commit + benchmark the orchestration-density targets (R-FILE-011 re-anchor)
group: a
sequence: 7
repo: "."
base_branch: "main"
depends_on: [ops1]
importance: 8
complexity: 5
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [R-FILE-011, ROADMAP §2 (re-anchor), R-QA-009, R-QA-012, ARCHITECTURE §1.1]
---
## Goal
Close the re-anchored wedge-pillar-1 commitment: measure and COMMIT the **ticks/sec/instance**
and **instances-per-box** numbers on packed builds, benchmarked under the R-QA-009 methodology.

## Scope & seams
- Bench harness: N headless packed instances (a06 server artifact) stepped/seeded/hashed in
  parallel from one controller over the R-QA-005 session surface — the pillar-1 demo shape
  (ARCHITECTURE §1.1); measure ticks/sec/instance and instances-per-box on the named runner.
- Commit the numbers into docs (machine-readable) + bench gates; honesty note vs GPU-vectorized
  simulators stays (README gaps row).
- **Dep note:** dev/measurement can start on existing runners (advisory numbers); the COMMITTED
  blocking numbers want the ops1 perf-isolated box — the M8.5 exit records whichever state is
  true, honestly.

## Definition of Done
- [ ] Bench in CI (nightly at minimum) with median-of-5 + dispersion + time series.
- [ ] Committed targets recorded in docs + fleet manifest; R-FILE-011's "TBD" resolved.
- [ ] Design-folder cross-refs (R-FILE-011 / ARCH §1.1 / README row) updated via the software
      repo after landing (TD captures).
