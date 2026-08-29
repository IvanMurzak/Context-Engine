---
id: "g1-verification-closeout"
title: "Chrome verification, docs/shell.md chrome section, e16 handoff, residue audit"
group: "G"
sequence: 1
repo: "."
base_branch: "main"
depends_on: ["a1-chrome-contract", "a2-strips-scaffold", "b1-windows-frameless", "c1-macos-hybrid", "d1-playbar-strip", "d2-statusbar", "d3-menu-system", "e1-playbar-dock-retirement", "f1-secondary-window-chrome"]
importance: 7
complexity: 6
security_critical: false
production_touching: false
model_hint: "mid"
taskflow_refs: ["README.md", "02-target-architecture.md", "ROADMAP.md"]
---

## Goal

Prove the finished chrome where CI can see it, document it, and close the set cleanly: live
windowed assertions, the `docs/shell.md` chrome section, the handoff note to e16
(visual-regression pins the finished chrome), and a residue audit across a1–f1.

## Scope & seams

- **Live assertions where CI can carry them**: X11 smoke drag/hit-rect checks; Windows headless
  `hit_test_frame` sweep corpus (dense point grid across bands/regions/DPIs). New tests land in
  the existing families where possible; any NEW CEF smoke registration pays the full
  "Not Run = RED" bookkeeping (executable in the job's `--target` list AND the named `ctest -R`
  step) in the same PR.
- **Risk closeout**: confirm each ROADMAP known-risk (1–6) has its pinned test or documented
  disposition, and name where each lives in the PR body.
- **`docs/shell.md` chrome section**: the chrome contract and modes, region flow, per-OS
  behavior (frameless/hybrid/SSD), the interim-honesty staging as history, and the deferred
  interactive verifications list (extending the existing precedent).
- **e16 handoff note**: what chrome surface is now stable for visual-regression to pin
  (02 §12 — this set hands e16 stable chrome); recorded where the m9-editor set tracks handoffs,
  plus this set's ROADMAP progress log.
- **Residue audit**: no dangling `builtin.playbar` references; no read `-webkit-app-region`
  (documentation-only occurrences allowed, 02 §12); no unowned TODO seams introduced by a1–f1;
  every standing gate (tasks/README.md) verified to have held. Findings are enumerated in the PR
  body — an empty list is itself a reported finding. PR-sized fixes land here; anything larger
  files an issue.

## Definition of Done

- New live assertions green on their legs; the hit-test sweep corpus registered and running.
- `docs/shell.md` section merged; e16 handoff note recorded.
- Residue audit enumerated in the PR body with file evidence for each check.
- Tests plant-verified where a new assertion can be planted (R-QA-013); full 42-check CI green.
