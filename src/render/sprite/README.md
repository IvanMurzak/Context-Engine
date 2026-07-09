# src/render/sprite/ — the 2D sprite path (M4, R-2D-001, L-55)

A first-class 2D draw path built **in the same renderer**, over the T1 RHI abstraction
(`context/render/rhi.h`) and the L-39 sim→render extract — **not** a fork of the pipeline, and **not**
2D bolted onto a 3D engine (L-55). Issue #122.

## What it provides

- **Orthographic projection** (`ortho.h` / `ortho.cpp`) — a 2D `Camera2D` (center + half-extents) that
  builds a column-major orthographic `Mat4` mapping world-space (y-up) onto the WebGPU clip cube
  (x,y ∈ [-1,1], z ∈ [0,1]). `quad_clip_corners()` projects a world sprite rectangle to its four clip
  corners — the shared geometry the GPU proof draws.
- **Sprite batching + sorting layers** (`batch.h` / `batch.cpp`) — `sort_draw_order()` orders sprites
  back-to-front by `(sort_layer, order_in_layer, authoring index)` (a stable painter's-order sort);
  `build_batches()` then coalesces **adjacent same-atlas** sprites into one draw call. Batching never
  reorders across the draw order — correctness (transparency/overdraw) over draw-call minimization.
- **Texture atlases** (`atlas.h` / `atlas.cpp`) — a `TextureAtlas` of named pixel sub-rectangles with
  normalized-UV lookup, plus a deterministic shelf `pack_atlas()` (descending-height placement,
  padding/bleed guard, overflow reporting).

## Two halves (mirrors the parent render module)

- **`context_render_sprite`** — the pure-CPU logic above. GPU-free, so it builds and is **unit-tested
  locally** under every toolchain (the local Ninja+Strawberry-GCC dev gate included): `sprite-test_ortho`,
  `sprite-test_atlas`, `sprite-test_batch`, `sprite-test_sprite_offscreen`.
- **The GPU sprite-draw offscreen proof** (`sprite_offscreen.h`, header-only) — `render_offscreen_sprite()`
  draws an ortho-projected two-sprite scene (overlapping, different sorting layers) through the RHI into
  an offscreen RGBA8 texture, reads it back, and asserts each sprite's color, the background, and — in
  the overlap — that the **higher sorting layer wins**. It runs on the wgpu-native backend via the
  parent module's `context_render_wgpu_offscreen sprite` executable, gated as the **`render-wgpu-sprite`**
  ctest on Linux CI (like `render-wgpu-offscreen`; not on Windows — the Session-0 native-render
  carve-out). The quad corners it draws come from the unit-tested CPU projection, so a green CI readback
  proves the ortho path places the mathematically-verified geometry.

## Build & test

```sh
# CPU logic — builds + tests everywhere (the local dev gate):
cmake -S src --preset dev
cd src && cmake --build --preset dev && ctest --preset dev -R "^sprite-"

# The GPU sprite proof (needs a wgpu-native prebuilt + a rasterizing adapter; Linux CI):
cmake -S src --preset dev -DCONTEXT_BUILD_RENDER_WGPU=ON
cd src && cmake --build --preset dev --target context_render_wgpu_offscreen \
  && ctest --preset dev -R "^render-wgpu-sprite"
```
