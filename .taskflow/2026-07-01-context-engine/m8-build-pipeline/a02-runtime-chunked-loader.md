---
id: a02-runtime-chunked-loader
title: RuntimeKernel chunked content loading — async load/instantiate/unload by GUID + streaming scheduler
group: a
sequence: 2
repo: "."
base_branch: "main"
depends_on: []
importance: 9
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [R-ASSET-005, R-ASSET-003, R-FILE-009, R-SIM-007, ROADMAP §1-M8]
---
## Goal
Give RuntimeKernel the runtime half of R-ASSET-005: async **load/instantiate/unload of packed
content units by GUID**, with the streaming scheduler and memory budgets that make the Web
conditional-MUST (R-ASSET-003) real.

## Scope & seams
- One loading seam only (R-FILE-009): the same derived-artifact consumer fed live in-editor and
  from v1 packs in shipped builds — no authored-file parsing in RuntimeKernel.
- Async streaming scheduler with configurable memory budgets; proximity-driven decisions hook
  the R-SIM-007 spatial index; handle-invalidation path shared with hot reload (L-22/L-24).
- Sequential in-lane after a01 (consumes the v1 format).

## Definition of Done
- [ ] Load/instantiate/unload by GUID works headless from a packed archive; unload provably
      releases residency (budget assertions, sanitizer-aware ceilings per repo convention).
- [ ] Streaming stays inside a configured memory budget on a synthetic over-budget world.
- [ ] Editor-fed vs pack-fed parity test: identical derived world, identical state hash.
- [ ] Tests land in the same PR (R-QA-013), green on 3 OS legs.
