---
id: ops1-perf-runner-provisioning
title: Provision the perf-isolated Linux runner class (R-QA-009/R-QA-012)
group: ops
sequence: 1
repo: "."
base_branch: "main"
depends_on: []
importance: 6
complexity: 3
security_critical: false
production_touching: false
model_hint: fast
taskflow_refs: [R-QA-009, R-QA-012, ROADMAP §1-M8 de-risks, engine docs/perf-gate-methodology.md]
---
## Goal
Provision the `perf-linux-bare-metal` runner class so the numeric perf gates (R-FILE-011 bench,
R-LANG-012, R-QA-007, a12 build-time budgets, a21 density targets) can flip from
advisory-until-provisioned to blocking.

## Scope & seams
- **HUMAN GATE (owner): money/hardware decision** — options: a dedicated bare-metal box (cheap
  dedicated host), or a reserved isolated machine in the existing fleet (the IVANPC self-hosted
  pattern — but perf isolation requires it NOT share load with the 3 existing Windows runners).
- Operator work: install the runner (LocalSystem-service gotchas are documented in memory —
  safe.directory, no `shell: bash` under LocalSystem), label it per the fleet manifest row,
  pin CPU governor/turbo settings per the R-QA-009 methodology.
- Engine-repo config PR: flip the manifest row from advisory-until-provisioned; enable the
  blocking numeric checks (median-of-5, ±10% band) — done in-lane (a12/a21 record the flip).

## Definition of Done
- [ ] Runner online with the manifest-named labels; isolation verified (no concurrent jobs).
- [ ] Fleet manifest row flipped; at least one perf gate runs blocking and green on it.
- [ ] Runner setup documented in engine `docs/self-hosted-runners.md`.
