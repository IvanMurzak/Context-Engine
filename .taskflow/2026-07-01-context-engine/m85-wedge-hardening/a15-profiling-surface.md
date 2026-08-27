---
id: a15-profiling-surface
title: Profiling surface — always-on HUD + Tracy/RenderDoc export + `context profile --json` (L-47)
group: a
sequence: 1
repo: "."
base_branch: "main"
depends_on: []
importance: 7
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [L-47, R-OBS-002, R-OBS-004, R-SIM-008, ROADMAP §1-M8.5]
---
## Goal
The wedge's performance-visibility floor: counters + lightweight HUD, deep capture via
Tracy/RenderDoc export, and ALL profiling data CLI/RPC-queryable as JSON.

## Scope & seams
- Per-system spans across C++/TS/WASM from the L-38 declarations (R-OBS-004); the existing M6
  GC-pause channel folds into the same surface (R-SIM-008) — extend, don't duplicate.
- `context profile --json` in the registry (live session query, headless-capable); HUD is a
  render-module overlay (absent headless).
- Tracy (CPU) export + RenderDoc (GPU) capture hooks — export to world-class tools, don't
  rebuild them.

## Definition of Done
- [ ] `context profile --json` returns spans/counters (incl. GC-pause attribution) on a live
      headless session; schema introspectable.
- [ ] Tracy capture demonstrated on a sample; RenderDoc hook documented on the GPU leg.
- [ ] HUD toggles on a rendered sample; zero cost measured when disabled.
