# goldens/ — the M4 golden-scene visual-equivalence corpus

The committed baselines for the **M4 exit** visual-equivalence gate (ROADMAP §1 M4 exit; issue
**#141**; R-REND-002 T1 semantics): each corpus scene is rendered **offscreen per backend** — the
native wgpu-native backend (CI `render` job, Linux/lavapipe) and the **browser's** WebGPU via
emscripten (CI `render-web` job, headless Chromium/SwiftShader) — and compared against the baseline
here with a **named SSIM-class perceptual metric + per-scene tolerances** (`manifest.json`).

- **"Identical desktop + web" = the same T1 semantics**, NOT bit-identical frames: each backend
  rasterizes through its own driver/translator (Naga natively, Tint in the browser), and
  float→unorm rounding legally differs. The SSIM tolerance encodes exactly that ruling.
- **Blocking policy (minimal v1)**: Linux-Vulkan + one browser (Chromium) BLOCKING; other backends
  advisory until their R-QA-012 runner rows are provisioned (`docs/ci-fleet-manifest.json`).
- **The scenes are the proofs' frames by construction**: every corpus render goes through the SAME
  factored render path the offscreen proof assertions use (`render_offscreen_triangle_pixels`,
  `render_sprite_scene_pixels`, `LitOffscreen`), so a golden can never drift from what the proofs
  assert.

## Files

| File | Scene | What it pins |
|---|---|---|
| `triangle3d.ppm` | 3D render pipeline | clear + clip-space triangle through the T1 RHI |
| `sprite2d.ppm` | 2D sprite path (R-2D-001) | ortho projection + sorting-layer overdraw |
| `lit3d.ppm` | representative lit scene | PBR + dir/point lights + shadow depth pass (R-REND-004/006) |
| `viewport.ppm` | M5-F1 observer viewport (3D+2D) | the 3D triangle base + the 2D sprites composited on top (issue #164) |
| `ui-hud.ppm` | M7 a6 runtime UI HUD + a8 shaped text (R-UI-005) | a colored-rect HUD (health bar + fill, minimap, GPU-composited badge, status bar) PLUS a shaped-text label ("SCORE 1200") drawn as atlas-textured glyph cutout quads, extracted through the GpuUiProvider — native + web |
| `ui-worldpanel.ppm` | M7 a9 world-space RTT UI panel (R-UI-003, D4) | a UI panel rendered into a dynamic-texture registry target, then sampled onto a rotated world quad over a lit-ground base (native only) |

Format: binary PPM (P6, maxval 255) — stdlib-parseable, no image dependency anywhere in the chain.
Baselines were rendered through the native backend on real hardware; the per-scene `min_ssim` in
`manifest.json` absorbs software-rasterizer (lavapipe/SwiftShader) differences.

## Reproducing / gating locally

```sh
# native (needs a rasterizing adapter; CI uses lavapipe):
context_render_wgpu_offscreen golden triangle3d out/triangle3d.ppm
python3 tools/golden_compare.py --scene triangle3d --candidate out/triangle3d.ppm

# browser (headless Chromium + SwiftShader; serves + runs the T6 web harness):
python3 tools/web_golden_run.py --html build/render-web/context-render-web.html --out-dir out/web
python3 tools/golden_compare.py --scene triangle3d --candidate out/web/triangle3d.ppm

# M5-F1 observer viewport composite (native only, like lit3d):
context_render_wgpu_offscreen golden viewport out/viewport.ppm
python3 tools/golden_compare.py --scene viewport --candidate out/viewport.ppm

# M7 a6 runtime UI HUD (native AND web — the ui backend compiles into the emscripten target too):
context_render_wgpu_offscreen golden ui-hud out/ui-hud.ppm
python3 tools/golden_compare.py --scene ui-hud --candidate out/ui-hud.ppm
# browser leg: web_golden_run.py delivers ui-hud alongside triangle3d,sprite2d (see ci.yml render-web).

# M7 a9 world-space RTT UI panel (native only, like lit3d/viewport):
context_render_wgpu_offscreen golden ui-worldpanel out/ui-worldpanel.ppm
python3 tools/golden_compare.py --scene ui-worldpanel --candidate out/ui-worldpanel.ppm
```

The **`ui-hud`** scene is the M7 a6 engine-integrated GPU UI backend + the M7 a8 shaped text (issue #141
golden discipline; R-UI-005): a HUD of solid opaque colored rectangles authored as an a1 `UiTree`,
extracted to draw quads and repainted into the `GpuUiProvider`'s persistent UI-layer texture over the
SAME ortho quad path the `sprite2d` golden already SSIM-gates, PLUS a shaped-text label ("SCORE 1200")
drawn on top as atlas-textured glyph **cutout** quads (`measure` → FreeType glyph atlas → textured-glyph
draw; the coverage thresholded at 0.5, integer 1:1 pixel↔texel, so the GPU nearest-sample+`discard` and
the CPU mirror emit the same 1-bit mask — the T1 RHI has no alpha blend and a cutout needs none). Opaque
replace, no blend / no MSAA — a tight tolerance. It is rendered on BOTH the native (lavapipe) and web
(SwiftShader) legs — unlike native-only `lit3d`/`viewport` — because the UI backend AND the shaping text
stack (FreeType/HarfBuzz/SheenBidi/libunibreak) compile into the Emscripten web target. The committed
baseline is the analytic (GPU-free) rasterization of the same extracted quads + 1-bit text mask
(`hud_scene.h::render_ui_hud_reference_cpu`). Shaping correctness is proven headless (`ui-test_shaping` /
`ui-test_line_break`); this golden pins that shaped text reaches pixels.

The **`ui-worldpanel`** scene is the M7 a9 world-space render-to-texture UI panel (issue #141 golden
discipline; R-UI-003, lock D4): a teal panel with an amber inner rect is rendered into a persistent 64×64
**dynamic-texture registry** target (the FIRST dynamic-texture entry — the "later wave" the
`render_world.h` texture handle fields reserved), then **sampled** (nearest) onto a flat quad rotated
(yaw + roll) / positioned by a `render::Transform` in a lit 3D scene — a receding-floor lit-ground
trapezoid under the panel, over a dark-slate sky clear. The projection is a straight-on **orthographic**
camera, so the rotated quad projects affinely (linear UV) and the CPU mirror matches the GPU rasterizer
within tolerance. It is **native-only first** (like `lit3d`/`viewport`): the web golden target compiles
only `triangle3d` + `sprite2d` today, so browser coverage joins when the lit web proof lands. The
committed baseline is the analytic (GPU-free) rasterization of the SAME projected quads + panel content
(`worldpanel_scene.h::render_worldpanel_reference_cpu`), which the CI `render` job re-renders on the
wgpu-native (lavapipe) backend and SSIM-gates.

The **`viewport`** scene is the M5-F1 observer-viewport composite (issue #164): the 3D triangle base
(`triangle3d`) with the 2D sprites (`sprite2d`) painted on top in one frame — the "live scene (3D+2D)"
the native viewport panel renders over `context_render(_wgpu)`. Because the T1 primitives use opaque
replace with **no alpha blending and no MSAA**, the composite is deterministic and its baseline equals
compositing the committed `triangle3d.ppm` + `sprite2d.ppm` (sprite-over-triangle-over-clear) — so
`goldens/viewport.ppm` is **byte-identical** to `context_render_wgpu_offscreen golden viewport`, which
the CI `render` job re-renders and SSIM-gates. It is **native-only** (like `lit3d`); web T1
equivalence is gated by the `triangle3d` + `sprite2d` browser goldens, whose exact primitives the
composite reuses.

## Rebaselining — REVIEWED changes only

Rebaselines are **reviewed changes, never automatic** (ROADMAP §1 M4 exit). When a PR intentionally
changes rendering output:

1. Re-render the affected scene(s) with the commands above (native render on a real adapter).
2. Replace the `goldens/*.ppm` **in the same PR** as the rendering change.
3. Say in the PR body **what** changed visually and **why** — the diff of a binary golden is the
   review artifact, so the prose carries the intent.

A CI golden-gate failure with NO intentional rendering change is a regression — fix the code, do
not rebaseline.
