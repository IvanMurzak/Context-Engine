---
id: a23-m85-exit-gate
title: M8.5 exit gate — blocking m85-exit-* ctests + CI wiring (the v1 wedge bar)
group: a
sequence: 9
repo: "."
base_branch: "main"
depends_on: [ops1]
importance: 10
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [ROADMAP §1-M8.5 exit, R-SEC-*, R-COLLAB-*, L-50, L-47, R-HUX-006, R-FILE-011]
---
## Goal
Encode the M8.5 (= v1 wedge-hardening) exit criteria as permanent blocking `m85-exit-*` gates.

## Scope & seams
- Gates per the ROADMAP M8.5 exit: operator-scoped agent + human co-edit/merge across worktrees
  with no lost updates (a16); sandboxed package cannot exceed granted capabilities + tampered
  signed artifact refused fail-closed (a17); profiling JSON-queryable on a live session (a15);
  trailing-GUI exercise — tilemap paint, in-context override edit, contextual help (a18/a19/
  a20); orchestration-density targets committed + benchmarked (a21, honesty per ops1 state).
- CI-wiring tripwire discipline (target lists + named `ctest -R "^m85-exit-"` step + general-
  step exclusion + fleet-manifest rows).

## Definition of Done
- [ ] All `m85-exit-*` gates live + green on their legs; "Not Run = RED" audit passes.
- [ ] Fleet manifest + §6 updated; every M8.5 exit clause maps to exactly one gate.
- [ ] ROADMAP M8.5 (and v1 scope-complete) flipped only after this lands (single-writer rule).
