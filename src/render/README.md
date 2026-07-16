# src/render/ — the render subsystem (M4 foundation)

The first production render module (issue #114), promoting the throwaway `spikes/webgpu/` M0 de-risk
into a real, tested subsystem. Design records: **R-REND-001/002** (WebGPU baseline + tiered RHI),
**R-REND-003** (sim/render one-way data flow), **R-HEAD-002** (rendering strictly optional/
detachable), **L-11/L-56** (WebGPU baseline, WebGPU-only web in v1), **L-39** (extract into a
double-buffered render world), **R-SEC-009** (SHA-pinned signed-prebuilt exception).

## Two libraries by design

### `context_render` — RHI abstraction + extract + detachable renderer (backend-free)

Builds and tests under **every** toolchain, including the local Ninja + Strawberry-GCC Windows dev
gate. Pulls in NO GPU backend, so a headless build never links one.

- **`rhi.h`** — the tiered **Render Hardware Interface** (R-REND-002). WebGPU T1 semantics as
  abstract interfaces: `IRhi` (probe / create device), `IDevice`, `IQueue`, `IBuffer`, `ITexture`,
  `ITextureView`, `ISampler`, `IRenderPipeline`, `IBindGroupLayout`/`IBindGroup`,
  `ICommandEncoder`, `IRenderPassEncoder`, `ICommandBuffer`, plus the `ISurface`/`ISwapchain` seam
  for the later windowed-present path. Depth textures/attachments, depth-only pipelines,
  comparison samplers (the shadow-PCF primitive), uniform buffers + queue writes, and REFLECTED
  ("auto") bind-group layouts landed with the lit path (issue #135) — reflection is deliberately
  the ONLY layout source, which keeps the seam stable under Tint's combined-sampler binding
  renumbering (`docs/wgsl-tool-decision.md`). `RhiTier { T1_WebGPU, T2_Native }` is the T1/T2 tier
  seam (L-56 collapsed the tiers to T1 floor / T2 advanced); only T1 native is implemented in this
  foundation. The header is dependency-free — the R-HEAD-002 detach seam.
- **`render_world.h` / `extract.h`** — the **L-39 sim→render extract**. `extract_render_world()` is a
  read-only observer of the kernel World (R-REND-003): it walks archetypes through the World's
  `for_each_archetype` seam and copies render-relevant components (`Transform`, `Renderable`, the
  R-REND-004 `PbrMaterial`/`DirectionalLight`/`PointLight`) into a `RenderSnapshot` — a
  zero-length light direction or a point light without a Transform is malformed/absent and
  skipped. `RenderDoubleBuffer` gives the sim/extract side a back buffer to write while the
  render side reads a stable front buffer — no tearing, and the two endpoints render-side
  interpolation needs. (The visible-set bound — L-39's R-SIM-007 broad-phase query so extract cost
  scales with the visible set — wires in a later wave; the walk is structured for it to drop in.)
- **`renderer.h`** — the **detachable renderer** (R-HEAD-002 / R-KERNEL-002). `Renderer::try_attach`
  probes for an adapter and creates a device only if one exists, returning `nullptr` otherwise — so
  "headless" is the *absence* of a `Renderer`, not a flag on one. The engine runs fully with the
  render module absent; the kernel never depends on render (render depends on the kernel).

### `context_render_wgpu` — the T1 native backend + offscreen proof (CI-gated dependency path)

The wgpu-native implementation of the RHI, behind the **`CONTEXT_BUILD_RENDER_WGPU`** option (default
OFF). The only off-the-shelf Windows wgpu-native prebuilt is MSVC-ABI, so — exactly like the V8
dependency path (`src/runtime/js/`) — the local Strawberry-GCC dev gate cannot link it; it is a
**CI-gated dependency path**, built + tested by the dedicated `render` CI job.

- **`wgpu/wgpu_rhi.cpp`** — implements every `rhi.h` interface over `webgpu.h`, reusing the wgpu calls
  proven by the `spikes/webgpu` spike. The **only** TU that includes `webgpu.h`.
- **`offscreen_scene.h`** — the offscreen render + pixel-readback proof, written entirely against the
  RHI abstraction (`render_offscreen_triangle`). The **same** code runs on the fake backend (local
  ctest) and the wgpu backend (CI), so a green fake test means the abstraction is coherent and the
  real backend only has to implement `rhi.h` correctly.
- **`wgpu/offscreen_main.cpp`** — the `context_render_wgpu_offscreen` executable: `probe` (R-HEAD-002,
  no device) / `render` (offscreen readback, exit 0 pass / 77 skip / 1 fail) / `sprite` (the
  R-2D-001 sprite proof) / `lit` (the R-REND-004/006 PBR + shadow + lightmap-hook proof —
  see `lit/README.md`) / `viewport` (the M5-F1 3D+2D observer composite) / `ui` (the M7 a6 GPU UI HUD
  proof — see `ui/README.md`) / `golden <scene> <out.ppm>` (dump a golden-corpus frame for the SSIM
  gate — `goldens/README.md`) / `bench [frames] [warmup] [WxH]` (the R-QA-007 min-spec floor bench
  subject, driven by `bench/minspec_floor.py`). M4 T7, issue #141.
- **`golden.h`** (+ `lit/golden_lit.h`, `viewport_scene.h`, `ui/hud_scene.h`) — the golden-scene corpus
  renderer: each corpus scene (`triangle3d`, `sprite2d`, `lit3d`, `viewport`, `ui-hud`) renders through
  the SAME factored proof path (`render_offscreen_triangle_pixels` / `render_sprite_scene_pixels` /
  `LitOffscreen` / `render_offscreen_viewport_pixels` / `ui::render_golden_ui_hud`), so the committed
  baselines under `goldens/` are the proofs' frames by construction. `golden.h` is kernel-free (the web
  harness renders `triangle3d` + `sprite2d` + `ui-hud`); `golden_lit.h` adds the kernel-backed lit scene
  + the bench frame loop.

### `context_render_web` — the T1 browser WebGPU backend (emscripten/emdawnwebgpu; CI-gated)

The **web** T1 RHI backend (`src/render/web/`, issue #137 T6; R-REND-002, **L-56**). It is a **new
RHI backend, not a renderer fork** — the sim→render extract + double-buffer + detachable renderer
facade stay shared with native. It reuses the **same `wgpu/wgpu_rhi.cpp`** the native backend uses,
compiled for the **browser's** WebGPU via Emscripten + the **emdawnwebgpu** port (`webgpu.h` →
`navigator.gpu`; the L-56-locked web path, **not** a Dawn cross-compile). The spike proved one
source compiles both ways with near-zero conditionals; the only `__EMSCRIPTEN__` divergences are the
three the spike documented: the poll pump yields to the browser event loop (Asyncify
`emscripten_sleep`), the R-HEAD-002 probe uses `requestAdapter` (emdawnwebgpu has no
enumerate-adapters extra), and `backend_name()` reports `browser-webgpu`.

- **`web/web_main.cpp`** — the offscreen parity harness. Runs the **same** triangle (3D pipeline) +
  sprite (2D) + **ui-hud** (M7 a6 runtime UI, `ui/hud_scene.h`) proofs the native offscreen exe runs,
  through the **same** `rhi.h` — so desktop/web are identical **within the T1 feature set** by
  construction (same semantics; bit-identical frames are NOT required — float→unorm rounding legally
  differs per backend). Kernel-free (no extract/kernel surface under emscripten; the UI backend is
  presentation-only, D6).
- **`web/CMakeLists.txt`** — standalone (`emcmake cmake -S src/render/web`), like the spike's web
  leg. Deliberately **not** part of the native build. emdawnwebgpu constraints honored: `-sASYNCIFY`,
  `-sEXIT_RUNTIME=1`, heap sized up front (`-sINITIAL_MEMORY`) and **no `-sALLOW_MEMORY_GROWTH`** (it
  breaks emdawnwebgpu device acquisition — a resizable ArrayBuffer throws in its `TextDecoder` glue).
- **Bind-group parity (Tint combined-sampler split, `docs/wgsl-tool-decision.md`):** free — the RHI's
  bind-group layouts are always **reflected** from the pipeline's shader
  (`IRenderPipeline::bind_group_layout` → `wgpuRenderPipelineGetBindGroupLayout`), so post-split
  binding renumbering is picked up identically on web and native with no caller hardcoding a pre-split
  map (`rhi.h` `IBindGroupLayout` note).

Built by the `render-web` CI job (`.github/workflows/ci.yml`; emsdk `latest`, emits
`context-render-web.{html,js,wasm}`), then **RUN in headless Chromium + SwiftShader WebGPU** by the
same job (M4 T7, issue #141): `tools/web_golden_run.py` serves the page, collects the golden-corpus
frames the harness POSTs back, and `tools/golden_compare.py` gates them against `goldens/` — the M4
"one browser blocking" run gate. The lit/PBR (3D lighting/shadow) web proof — which pulls the kernel
\+ extract into the emscripten build — is a deferred follow-up (`web/README.md`). Locally guarded by
the native `render-web-parity` ctest (the web harness's proof set asserted on the fake backend / CPU
under the dev gate).

## wgpu-native prebuilt (R-SEC-009)

The native backend links the **wgpu-native `v29.0.1.1` PREBUILT** (gfx-rs GitHub release),
SHA256-pinned + verified at configure time before use — the R-SEC-009 signed-prebuilt exception. The
acquisition channel (SHA-pinned prebuilt + publisher-TLS + verify-before-use) was owner-decided by
the #76 (V8) Option-A precedent. **M8 ship decision (issue #253, a04):** the deferred M4 Dawn
re-evaluation is **closed** — wgpu-native is **RETAINED** as the *shipped* native backend (not merely
the dev pick); Dawn's now-real official vcpkg from-source port did not justify a switch. Measured
rationale + re-evaluation triggers: `docs/native-webgpu-backend-decision.md`. wgpu-native is
dual-licensed `MIT OR Apache-2.0` (we elect Apache-2.0); it is already recorded in
`tools/license-allowlist.json`. Never shipped in the default build.

## Build & test

```sh
# The library half — builds + tests everywhere (the local dev gate):
cmake -S src --preset dev   # configure from the repo root — note the explicit -S src
cd src                      # build/test presets resolve CMakePresets.json from the working dir
cmake --build --preset dev && ctest --preset dev -R "^render-"   # render-test_extract / -test_headless / -test_rhi

# The native backend + offscreen proof (needs a wgpu-native prebuilt for this platform):
cmake -S src --preset dev -DCONTEXT_BUILD_RENDER_WGPU=ON
cd src && cmake --build --preset dev --target context_render_wgpu_offscreen && ctest --preset dev -R "^render-wgpu-"   # render-wgpu-probe (+ -offscreen where an adapter exists)
```

CI: the `render` job (`.github/workflows/ci.yml`) builds the native backend on ubuntu/macos/windows
and runs `probe` everywhere + the offscreen readback where a software adapter exists (lavapipe on
ubuntu; not registered on Windows — the same Session-0 teardown-crash carve-out the spike documents).
Registered in `docs/ci-fleet-manifest.json` as the `render-offscreen` gate. Since M4 T7 (issue
#141) the ubuntu leg also runs the **golden-scene SSIM corpus** (dump + gate vs `goldens/`,
`golden-scene-native-linux`) and the **R-QA-007 min-spec floor bench** (`bench/minspec_floor.py`
measure blocking / floor gate advisory — see the manifest's `minspec_floors` table). The real-GPU
visual-equivalence leg stays advisory until a GPU runner class is provisioned.
