---
id: a07-smoke-run-packed-determinism
title: R-BUILD-009 headless smoke-run + packed-build determinism/replication gate (blocking per-PR)
group: a
sequence: 7
repo: "."
base_branch: "main"
depends_on: []
importance: 10
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [R-BUILD-009, R-QA-005, L-54, R-NET-001, ROADMAP §1-M8 exit, ROADMAP §6]
---
## Goal
The machine answer to "does the packed artifact actually run?": the build CLI launches the
artifact it just produced, steps N ticks against the **SHIPPED** RuntimeKernel, and gates CI.

## Scope & seams
- Smoke verb/flag on the a05 build core: launch pack → assert boot signal + `simTick` progress
  (or state hash) via the R-QA-005 surface → return inside the R-CLI-008 envelope;
  cannot-smoke-headless targets declare it machine-readably.
- **Packed determinism:** the L-54 state-hash gate re-run against the packed wedge builds (not
  the editor-embedded kernel); the M6 L-48/R-NET-001 replication-metadata harness re-run against
  packs.
- CI: blocking per-PR wedge-target smoke (§6); respects the "Not Run = RED" tripwire (executable
  in the job's target list + named ctest step); fleet-manifest rows added.

## Definition of Done
- [ ] Smoke green on Linux desktop + server artifacts per-PR; envelope result asserted.
- [ ] Packed determinism hash matches the editor-run hash for the golden wedge scene on the
      3-OS determinism matrix; replication harness green against packs.
- [ ] Fleet manifest updated + validated; §6 gate live and blocking.
