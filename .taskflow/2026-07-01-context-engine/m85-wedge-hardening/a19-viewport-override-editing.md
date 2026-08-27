---
id: a19-viewport-override-editing
title: In-context viewport instance/override editing (R-HUX-006 MUST core)
group: a
sequence: 5
repo: "."
base_branch: "main"
depends_on: []
importance: 8
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [R-HUX-006, L-35, R-CLI-006, ROADMAP §1-M8.5 trailing-GUI bucket, R-A11Y-001]
---
## Goal
The GUI face of L-35: manipulate a composed scene-instance entity directly in the viewport, and
the edit lands as the correct override write with visible provenance.

## Scope & seams
- Viewport gizmos (move/rotate/scale + property edits) on composed entities → override entries
  via the SAME composed write path as `context set` (default-outermost; `--edit-template` /
  `--at-instance` surfaced as GUI affordances) — never a parallel write path.
- Provenance chain (R-CLI-006) rendered at the point of edit (which template supplied the
  value, which level overrode it); override-hygiene affordances stay advisory.
- Gesture semantics per L-20/L-30 (commit at gesture end; rebase-or-drop-loudly on conflict).
- a11y coverage for any new panel/affordance in the same PR (repo convention).

## Definition of Done
- [ ] Move an instanced entity in the viewport → override written in the outermost scene;
      retarget affordances write template/mid-level correctly (file-diff asserted).
- [ ] Provenance display matches `context query` provenance for the same entity (parity test).
- [ ] Gesture-conflict drop/rebase paths exercised headless via the UI-logic tree.
