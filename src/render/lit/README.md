# src/render/lit/ — PBR + real-time lighting/shadows + lightmap input hooks (M4)

Issue #135 (M4-T5). Design records: **R-REND-004** (modern PBR & real-time lighting/shadows,
MUST), **R-REND-006** (baked-lighting format hooks — the **hooks half only**; the baker is
COULD/post-v1 and deliberately absent), **R-REND-003** (sim→render one-way), **R-HEAD-002**
(render detachable), **R-DATA-006** (schema vocabulary: SI meters, unitless [0,1] factors).

## What this module is

The baseline **metallic-roughness PBR** path over the T1 RHI: directional + point lights as
**World components** (`DirectionalLight` / `PointLight` / `PbrMaterial`, `render_world.h`) that
flow through the L-39 **extract** into the `RenderSnapshot` (lights/materials are extracted
snapshot state — render never mutates or blocks the sim), a **shadow-mapping** directional-light
path (depth-only pass + comparison-sampler PCF), and the **R-REND-006 lightmap INPUT hooks**
(material-contract lightmap slot + UV2-channel selection + a trivial runtime sample path).

Two halves, mirroring the sprite module's split:

- **`context_render_lit`** — the pure-CPU half; builds + unit-tests under every toolchain
  including the local Ninja+Strawberry-GCC dev gate (no GPU):
  - `lit_math.*` — column-major Mat4/Vec3, `look_at`, WebGPU-clip `ortho`.
  - `pbr.*` — the **CPU reference** of the WGSL shading (Lambert + GGX/Schlick Cook-Torrance,
    constant-ambient IBL stub): the analytic oracle the GPU proof asserts readbacks against.
    It mirrors the WGSL in `lit_scene.cpp` formula-for-formula — change only in lockstep.
  - `lit_scene.*` — the reference scene: authored World components → extract →
    `pack_scene_uniform()` (the GPU uniform block), the generated lit WGSL (`lit_wgsl()`),
    and analytically-pinned sample points/expected colors.
- **`lit_offscreen.h`** — the header-only **GPU proof**, wired into the parent module's
  `context_render_wgpu_offscreen` executable (`lit` mode, ctest `render-wgpu-lit`): a real
  kernel World is extracted and rendered (shadow depth pass → PBR main pass) under six uniform
  variants, and readback pixels are asserted against the CPU reference — lit-vs-unlit and
  light/material parameter deltas, a known-shadowed probe vs its lit sibling (and their
  convergence with shadows disabled), point-light falloff, and the lightmap-hook round-trip
  (enabling it shifts a ground probe by exactly `lightmap * albedo`; the hook-free blocker is
  untouched). Runs where an adapter rasterizes — the **blocking `render (ubuntu)` lavapipe leg**;
  assertions are tolerance-aware (never exact-pixel: software-Vulkan depth precision differs from
  hardware, absorbed by the shadow bias + probe placement away from penumbras/edges).

## Shadow technique

Directional shadow mapping: a **depth-only pipeline** (no fragment stage) renders the scene from
the light's ortho frustum into a `Depth32Float` map; the main pass projects each fragment into
light space and resolves occlusion with a **comparison sampler** (`sampler_comparison`,
LessEqual) — the hardware 2×2 PCF primitive — widened to a 4-tap kernel (PCF-class,
R-REND-004). A constant depth bias absorbs interpolation/precision error; out-of-frustum points
are lit. Feature degradation stays within T1 (R-REND-002): no sub-T1 fallback path exists.

## Bind-group model (the T3d seam flag)

The lit WGSL is hand-authored, so its `@group/@binding` numbers are the shader's own contract.
Pipelines expose **reflected** ("auto") bind-group layouts through the RHI
(`IRenderPipeline::bind_group_layout`) — the ONLY layout source at this seam. That is what keeps
this code correct under the T3d finding (`docs/wgsl-tool-decision.md`): when shaders arrive via
the cross-compile chain, Tint **splits** combined GLSL samplers into texture+sampler pairs and
**renumbers** bindings — a layout reflected from the translated WGSL already carries the
post-split numbering, so no caller ever hardcodes a pre-split binding map. The web leg (T6)
inherits the same model.

## Out of scope (do not gold-plate here)

IBL beyond the constant ambient term, GI, ray tracing, upscalers (all R-GFX-* v2); the lightmap
**baker** (R-REND-006 COULD — only the input hooks land here); photometric light units; normal
mapping at runtime (the material contract carries the slot; the proof shades with geometric
normals); a mesh/asset registry (the proof bakes its two-quad geometry into the WGSL, exactly
the sprite-proof vertex-index pattern — the registry is a later wave).

## Build & test

```sh
cmake -S src --preset dev            # from the repo root
cd src
cmake --build --preset dev && ctest --preset dev -R "^lit-"   # the CPU half (local, no GPU)
# GPU proof (CI-gated dependency path, lavapipe on the render job):
cmake -S src --preset dev -DCONTEXT_BUILD_RENDER_WGPU=ON
cmake --build --preset dev --target context_render_wgpu_offscreen && ctest --preset dev -R "^render-wgpu-lit"
```
