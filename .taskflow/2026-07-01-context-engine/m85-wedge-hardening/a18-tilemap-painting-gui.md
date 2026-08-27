---
id: a18-tilemap-painting-gui
title: Tile-painting GUI + 2D viewport-authoring mode (R-2D-003 GUI half)
group: a
sequence: 4
repo: "."
base_branch: "main"
depends_on: []
importance: 7
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [R-2D-003, L-55, ROADMAP §1-M8.5 trailing-GUI bucket, R-A11Y-001, R-EDIT-001]
---
## Goal
The trailing-v1 2D authoring surface: a tile-painting GUI over the M2 tilemap asset kind and a
2D viewport-authoring mode in the editor.

## Scope & seams
- Editor GUI (CEF panels + native viewport): 2D ortho viewport mode, tile palette, paint/erase/
  fill tools — every paint gesture commits as canonical file writes at gesture end (L-20; the
  tilemap kind + sidecar rules are M2 — do NOT change the format).
- CLI/RPC parity: painting is expressible as verbs too (R-CLI-001 — the GUI is sugar over the
  same write path).
- New panel ships its a11y coverage in the same PR (repo convention: registry + manifest +
  keyboard-nav test); panel logic headless-testable via the R-EDIT-001 UI-logic tree.

## Definition of Done
- [ ] Paint a tilemap in the GUI; the authored file diff is canonical and hot-reloads.
- [ ] Same edit reproducible via CLI verbs (parity test).
- [ ] a11y scan + keyboard-only nav green for the new panel; headless panel-logic tests green.
