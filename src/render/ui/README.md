# src/render/ui/ — the engine-integrated GPU UI backend (M7 a6, R-UI-005)

`context_render_ui` is the GPU-driven implementation of a1's backend-agnostic **`UiProvider`** contract
(`src/packages/ui/`, `context_ui`) over the **T1 RHI** (`context_render`, `rhi.h`). It is the D3
"the GPU backend lives in `src/render/ui/`" seam: a1 ships the retained `UiTree` + damage + the provider
contract; this package presents it on the GPU. Capabilities advertised: **`gpu_driver` +
`damage_repaint` + `composited_transforms`**.

UI is **presentation** (D6): it lives OUTSIDE the sim World, registers no hashed sim component, and the
kernel never links back. The whole package drives only `rhi.h` + the pure-CPU sprite ortho/quad math, so
it builds + unit-tests under the local GPU-less GCC dev gate AND compiles into the Emscripten web target
(the `ui-hud` golden's web leg).

## Pieces

- **`snapshot.h` / `src/snapshot.cpp`** — the **UI extract**: a READ-ONLY observer walks the `UiTree`
  into a `UiRenderSnapshot` of flat draw quads (surface-space rect + solid color + effective opacity +
  paint order), riding an **L-39 double buffer** (`UiRenderDoubleBuffer`) so a present reads a stable
  copy while the next frame extracts. Drawable = visible + non-transparent-opacity + non-empty +
  non-transparent background, in pre-order (painter's algorithm).
- **`composite.h` / `src/composite.cpp`** — the **composite-time math** (R-UI-005 `composited_transforms`):
  `apply_transform` (scale-about-centre + translate, no relayout), `effective_opacity` (composed down the
  tree), `blend_over` (the opacity fold against a known backdrop — the T1 no-blend-state resolution).
- **`batch.h` / `src/batch.cpp`** — draw scheduling, reusing the sprite path's **sort-then-coalesce**
  discipline (never reorder across paint order): `sort_ui_draw_order`, `build_ui_batches` (opaque vs
  translucent key), and `select_draw_set` — the **damage → minimal draw set** (only the quads
  overlapping the repaint plan's dirty regions).
- **`provider.h` / `src/provider.cpp`** — **`GpuUiProvider`** (`UiProvider`): holds a **persistent
  offscreen UI-layer texture** and repaints it each `present()` — `LoadOp::Clear` on a full repaint /
  first frame, `LoadOp::Load` + a reduced (scissored) redraw of only the damaged quads on a damage
  repaint. This is the web-reality architecture (`gpuweb#1424`: the swapchain backbuffer is a fresh,
  non-preserved texture every frame, so partial repaint MUST target a persistent layer, not the
  backbuffer). Quads render via the reused sprite ortho + `quad_wgsl` path. The full-screen composite of
  the persistent layer onto the fresh backbuffer is the windowed/web **present** path, which `rhi.h`
  itself defers to the `ISurface`/`ISwapchain` wave; at this offscreen tier the layer is the presentable
  result (read back for the golden + the CI proof).
- **`hud_scene.h`** (header-only, like `sprite_offscreen.h`) — the **`ui-hud` golden** corpus scene: a
  reference HUD of solid colored rectangles (health bar + fill, minimap, a GPU-composited badge, status
  bar) authored as a `UiTree`, rendered through the provider (`render_golden_ui_hud`), plus the analytic
  GPU-free rasterization of the same extracted quads (`render_ui_hud_reference_cpu`) that generates the
  committed `goldens/ui-hud.ppm`. Text/glyphs are deliberately absent — they arrive with a7/a8.

## Tests + gates (R-QA-013)

- **Local (GPU-free, `render-ui-*` ctests, all 3 build legs via the general step):** extract selection +
  double buffer, composite math, draw-order/batching + **damage → minimal draw set** (structural
  draw-count assertions), the `GpuUiProvider` full-clear-vs-damage-load path over the fake RHI, and the
  reference-HUD extract + analytic golden pixels.
- **CI GPU (`render-wgpu-ui`, auto-picked by the `render` job's `^render-wgpu-` regex):** the provider
  renders the HUD on lavapipe and the readback is asserted.
- **Golden SSIM (`ui-hud`, native + web, blocking):** the `render` job dumps `context_render_wgpu_offscreen
  golden ui-hud` (lavapipe) and the `render-web` job's harness POSTs the browser (SwiftShader) frame,
  both gated against `goldens/ui-hud.ppm` (`tools/golden_compare.py`, `goldens/manifest.json`, min-SSIM
  0.99). Rebaselines are reviewed changes, never automatic (`goldens/README.md`).
