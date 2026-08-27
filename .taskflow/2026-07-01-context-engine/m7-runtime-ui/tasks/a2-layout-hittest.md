---
id: a2-layout-hittest
title: Headless layout (anchor/absolute + flex-lite) + hit-testing + computed rects
group: A
sequence: 2
repo: "."
base_branch: "main"
depends_on: []
importance: 8
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [T2, D2]
---
## Goal
Computed geometry with no GPU: anchored/absolute positioning + a stack/flex-lite flow container;
per-node computed rects; top-most point hit-testing respecting visibility/opacity; deterministic
focus order. Headless because the R-UI-006 assertion surface AND input hit-testing (a3) need
rects with no renderer attached.

## Scope & seams
`src/packages/ui/` only (layout engine + tests). No CI wiring changes.

## Definition of Done
- [ ] Layout unit tests incl. resize/reflow → damage propagation.
- [ ] Hit-test edge cases: overlap, nesting, hidden/zero-opacity nodes.
- [ ] Deterministic focus order asserted.
- [ ] General CI step green on all 3 legs (R-QA-013 same-PR tests).
