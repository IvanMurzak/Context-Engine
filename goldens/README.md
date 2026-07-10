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
```

## Rebaselining — REVIEWED changes only

Rebaselines are **reviewed changes, never automatic** (ROADMAP §1 M4 exit). When a PR intentionally
changes rendering output:

1. Re-render the affected scene(s) with the commands above (native render on a real adapter).
2. Replace the `goldens/*.ppm` **in the same PR** as the rendering change.
3. Say in the PR body **what** changed visually and **why** — the diff of a binary golden is the
   review artifact, so the prose carries the intent.

A CI golden-gate failure with NO intentional rendering change is a regression — fix the code, do
not rebaseline.
