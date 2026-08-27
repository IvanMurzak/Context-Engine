---
id: a11-web-export-adapter
title: Web export adapter — Emscripten/emdawnwebgpu template + streamed packs
group: a
sequence: 11
repo: "."
base_branch: "main"
depends_on: []
importance: 8
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [R-BUILD-001, L-56, R-ASSET-003/005 (Web MUST), ROADMAP §1-M8, ROADMAP §5 emdawnwebgpu row]
---
## Goal
One Project → a runnable WebGPU-only web build: the Emscripten export template, wired to the
browser's WebGPU and streaming the v1 chunked pack.

## Scope & seams
- Export template over the existing render-web path (emdawnwebgpu, browser WebGPU binding,
  Asyncify — JSPI not yet broad); **fixed-memory heap sizing** per the spike constraint.
- **Re-test the `-sALLOW_MEMORY_GROWTH` constraint against the then-current emdawnwebgpu and
  file/link the upstream issue** (R6 finding 20 — the doc's mitigation must become actionable).
- Chunked pack loading over HTTP (range/streamed) feeding the a02 loader inside the browser
  memory budget (the R-ASSET-003/005 Web conditional-MUST).
- User docs: Linux-browser WebGPU rollout caveat; WebGL2 escape hatch stays post-v1 (L-56).

## Definition of Done
- [ ] Packed web build boots in headless Chromium + SwiftShader (render-web job) and passes the
      golden-scene SSIM corpus.
- [ ] A sample streams its pack in chunks within a configured memory budget.
- [ ] Memory-growth re-test recorded (doc + upstream issue link); template pins emdawnwebgpu.
