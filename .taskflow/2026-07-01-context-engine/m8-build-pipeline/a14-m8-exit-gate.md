---
id: a14-m8-exit-gate
title: M8 exit gate — blocking m8-exit-* ctests + CI wiring encoding the milestone bar
group: a
sequence: 14
repo: "."
base_branch: "main"
depends_on: [ops1]
importance: 10
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [ROADMAP §1-M8 exit, R-BUILD-002/005/009, L-54, R-NET-001, R-QA-012, §6]
---
## Goal
Encode the M8 exit criteria as permanent blocking `m8-exit-*` gates (the M6/M7 exit-gate
pattern), so the bar survives as CI, not prose.

## Scope & seams
- Gates (per the ROADMAP M8 exit, platform rulings re-read): full v1-set builds produced
  headless per-agent (Linux desktop/server + Windows + Web green; macOS on its leg);
  **desktop artifacts signed** (macOS notarized, Windows Authenticode) with verify-before-use
  failing closed under the pinned key (a08); headless smoke green on every headless-capable
  target; packed wedge builds pass the L-54 determinism gate + carry validated L-48/R-NET-001
  metadata (a07); wedge smoke blocking per-PR (§6).
- CI-wiring tripwire discipline: gate executables in the job target lists + named `ctest -R
  "^m8-exit-"` step per leg; general step excludes the family; fleet-manifest rows.
- **Dep note:** ops1 only gates whether the a12 budget number is blocking or advisory at exit —
  the exit gate records which honestly.

## Definition of Done
- [ ] All `m8-exit-*` gates live + green on their legs; "Not Run = RED" audit passes.
- [ ] Fleet manifest rows added + validated; §6 table matches reality.
- [ ] ROADMAP M8 marked complete only after this lands (single-writer status rule).
