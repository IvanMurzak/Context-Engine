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
  see `lit/README.md`).

## wgpu-native prebuilt (R-SEC-009)

The native backend links the **wgpu-native `v29.0.1.1` PREBUILT** (gfx-rs GitHub release),
SHA256-pinned + verified at configure time before use — the R-SEC-009 signed-prebuilt exception. The
acquisition channel (SHA-pinned prebuilt + publisher-TLS + verify-before-use now, a from-source
vcpkg/cargo port deferred pre-1.0) was owner-decided by the #76 (V8) Option-A precedent, which
explicitly governs M4's wgpu-native. wgpu-native is dual-licensed `MIT OR Apache-2.0` (we elect
Apache-2.0); it is already recorded in `tools/license-allowlist.json`. Never shipped in the default
build.

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
Registered in `docs/ci-fleet-manifest.json` as the `render-offscreen` gate. The real-GPU
visual-equivalence gate stays advisory until a GPU runner class is provisioned.
