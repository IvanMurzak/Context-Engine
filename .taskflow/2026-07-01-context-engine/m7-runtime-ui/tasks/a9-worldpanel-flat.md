---
id: a9-worldpanel-flat
title: World-space RTT panel (flat) + the first dynamic-texture registry entry
group: A
sequence: 9
repo: "."
base_branch: "main"
depends_on: []
importance: 8
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [T8-split, D4]
---
## Goal
Persistent per-panel offscreen target — NEW code against `rhi.h` (`offscreen_scene.h` is a
one-shot fixed-size proof helper; the reusable part is the RHI + golden/readback plumbing, not
the helper itself); the FIRST dynamic-texture registry entry (the "later wave" the
`render_world.h` handle fields reserved) binding it to a render-side `UiPanel` component (flat
quad, scalable/rotatable/positionable via `render::Transform`); the L-39 extract picks it up
(D4). `UiPanel` is float presentation state — never hashed (D6).

## Scope & seams
`src/render/ui/`, `src/render/include/context/render/render_world.h` (UiPanel component,
additive), `src/render/src/extract.cpp` (additive walk).

## Definition of Done
- [ ] Fake-backend RTT logic tests: panel-target → dynamic-texture → extract chain headless.
- [ ] Golden scene `ui-worldpanel` (panel on a rotated quad in a lit 3D scene) —
      **native-blocking first** (the lit3d/viewport manifest precedent: the web golden target
      compiles only triangle3d+sprite2d today; browser coverage joins when the lit web proof
      lands). Golden wiring: subcommand into the existing exe + render job loops + baselines +
      manifest entry marked native-only.
- [ ] All 3 legs + render green (render-web unaffected until the lit-web extension lands).
