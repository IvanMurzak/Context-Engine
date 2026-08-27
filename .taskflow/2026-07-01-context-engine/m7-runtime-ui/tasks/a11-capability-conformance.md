---
id: a11-capability-conformance
title: Published capability matrix + provider conformance ctest suite
group: A
sequence: 11
repo: "."
base_branch: "main"
depends_on: []
importance: 7
complexity: 5
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [T9, ruling-b, ruling-c, L-53]
---
## Goal
`docs/ui-capability-matrix.md` — the published L-53 per-platform capability matrix (v1 rows:
desktop + web; columns: null + engine-integrated providers; `text_shaping`/`bidi` = TRUE per
ruling (c), `ime` = false with the documented-deferral note) + a reusable provider conformance
ctest suite ANY provider must pass — the R-UI-002 pluggability proof and the R-UI-008 on-ramp
without shipping extra backends now (ruling b).

## Scope & seams
`docs/ui-capability-matrix.md`, `src/packages/ui/tests/` (conformance suite consumable by
out-of-tree providers).

## Definition of Done
- [ ] Both in-repo providers pass the SAME conformance suite.
- [ ] Matrix cross-checked against the live `Capabilities` structs by a test (rots-if-broken).
- [ ] General CI step green on all 3 legs.
