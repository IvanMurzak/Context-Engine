---
id: a6-screenspace-backend
title: Engine-integrated GPU backend — screen-space overlay riding the L-39 extract/double-buffer
group: A
sequence: 6
repo: "."
base_branch: "main"
depends_on: []
importance: 9
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [T6, D1, D3]
---
## Goal
`src/render/ui/` implementing the UiProvider contract over `rhi.h`: UI extract →
`UiRenderSnapshot` riding the L-39 double-buffer; quad batching (reuse `sprite/` sort/batch
concepts); overlay pass after 3D; damage-based repaint consuming a1's damage lists;
GPU-composited transforms/opacity (composite-time, no relayout). Capabilities: gpu_driver +
damage_repaint + composited_transforms = true.

## Scope & seams
`src/render/ui/`, `src/render/CMakeLists.txt`, `offscreen_scene.h`-adjacent golden plumbing.
Golden scene `ui-hud` (colored rects — text arrives a7/a8).
**Architecture pin (web reality):** damage repaint MUST target a persistent offscreen UI-layer
texture (LoadOp::Load + scissored redraw of damaged rects), composited full-screen each frame —
NEVER the swapchain backbuffer: on the web leg `getCurrentTexture()` returns a fresh,
non-preserved texture every frame (gpuweb#1424), so backbuffer partial repaint is impossible.
**Web-target note:** the render-web golden additionally requires compiling `context_ui` +
`src/render/ui/` into the Emscripten target (sources + `web_main.cpp` + harness scene list) —
a 4th edit beyond the ci.yml 3-edit drill.

## Definition of Done
- [ ] Fake-backend ctests on all legs: damage → minimal draw set (structural draw-count
      assertions, NO wall-clock asserts — else CONTEXT_TSAN_BUILD widening in the same PR);
      composite math.
- [ ] `ui-hud` golden SSIM-gated native + web. **CI 3-edit drill:** (1) bake the `golden ui-hud`
      subcommand into the existing `context_render_wgpu_offscreen` exe (render job `--target`
      list stays UNCHANGED — no second exe); (2) dump+compare lines in the render job loops;
      (3) mirror in render-web + commit `goldens/` baselines + manifest entry.
