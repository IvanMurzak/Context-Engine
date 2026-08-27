---
id: a16-l50-concurrency-validation
title: L-50 multi-client concurrency validation end-to-end (co-edit + multi-worktree merge)
group: a
sequence: 2
repo: "."
base_branch: "main"
depends_on: []
importance: 8
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [L-50, R-COLLAB-001/002, R-FILE-012, R-QA-010, ROADMAP §1-M8.5]
---
## Goal
Prove the multi-client file-authority model end-to-end: a human+agent co-editing scenario and a
multi-worktree merge scenario, driven on the R-QA-010 fault-injection seams.

## Scope & seams
- Scenario 1 (co-edit): two attached clients — write-queue serialization, field-path gesture
  conflicts (L-30), CAS `--if-match`, lost-update events — asserted under injected watcher
  loss/dup/reorder.
- Scenario 2 (worktrees): parallel worktrees diverge → R-FILE-012 structural merge (conflict
  envelope, `resolve-conflict`, id-based merge identity, sidecar ours/theirs) → post-merge
  `context validate` convergence gate + the R-QA-005 sim-level pass (semantic-conflict
  mitigation, R-FILE-012(c)).
- Failing seeds minimized + committed (R-QA-011).

## Definition of Done
- [ ] Both scenarios are deterministic integration tests, green on 3 OS legs.
- [ ] No lost update in 10k randomized co-edit rounds (seeded property run).
- [ ] Every documented conflict class in the merge corpus is exercised by scenario 2.
