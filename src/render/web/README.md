# src/render/web/ — the T1 WEB RHI backend (browser WebGPU via emscripten/emdawnwebgpu)

The **web** T1 render backend (issue **#137**, M4 **T6**). **Design records:** R-REND-002 (tiered RHI
T1/T2), R-REND-003 (sim/render one-way), R-HEAD-002 (render detachable/headless-absent), **L-56**
(WebGPU-only web in v1 — no WebGL2 tier).

A **new RHI backend, not a renderer fork.** It runs the renderer in the browser on the **browser's
own WebGPU** by compiling the **same `../src/wgpu/wgpu_rhi.cpp`** the native backend uses (the one
`rhi.h` implementation) for Emscripten + the **emdawnwebgpu** port — `webgpu.h` mapped onto
`navigator.gpu` at runtime, **not** a Dawn cross-compile (the L-56-locked web path, ARCHITECTURE.md
§4). The sim→render extract, double buffer, and detachable renderer facade stay **shared** with
native (already T1, in `context_render`). The spike (`spikes/webgpu/`) proved one source compiles
both ways with near-zero conditionals; the only `__EMSCRIPTEN__` divergences in `wgpu_rhi.cpp` are the
three it documented — the poll pump yields to the browser event loop (Asyncify `emscripten_sleep`),
the R-HEAD-002 probe uses `requestAdapter` (emdawnwebgpu has no enumerate-adapters extra), and
`backend_name()` reports `browser-webgpu`.

## Contents

- **`web_main.cpp`** — the offscreen parity harness. Runs the **same** triangle (3D-clip-space
  pipeline) + sprite (2D ortho) proofs the native offscreen exe runs
  (`../include/context/render/offscreen_scene.h`, `../sprite/.../sprite_offscreen.h`), through the
  **same** `rhi.h`. Desktop and web are therefore **identical within the T1 feature set** by
  construction (same semantics — bit-identical frames are NOT required; float→unorm rounding legally
  differs per backend, see `spikes/webgpu/FINDINGS.md`). Since M4 T7 (issue #141) it also renders
  the kernel-free **golden-corpus pair** (`golden.h`: `triangle3d` + `sprite2d`) and POSTs each raw
  frame back over its own origin (`/golden/<scene>?w=&h=`, then `/done?exit=`) to the
  `tools/web_golden_run.py` collector — the measured half of the golden-scene SSIM gate; opened
  manually (no collector) the POSTs are skipped. Exit 0 = PASS, 77 = SKIP (no browser WebGPU / no
  adapter — R-HEAD-002), 1 = FAIL.
- **`CMakeLists.txt`** — standalone (`emcmake cmake -S src/render/web`), like the spike's web leg.
  Deliberately **not** `add_subdirectory()`'d from the main `src/` tree — the rest of the engine
  (editor/runtime/cli daemon+IPC) does not compile under emscripten, so keeping this standalone leaves
  the native `dev` build matrix + the local GCC dev gate untouched.

## Build

```sh
# Emscripten (bundled emdawnwebgpu port). emcc is NOT on the local Windows executor, so this is a
# CI-only build path (like the wgpu-native native path / sanitize preset) — the render-web CI job is
# the authoritative compile signal.
emcmake cmake -S src/render/web -B build/render-web -DCMAKE_BUILD_TYPE=Release
cmake --build build/render-web        # -> context-render-web.{html,js,wasm}
# serve + open in a WebGPU browser (Chrome/Edge); PASS/HASH print to the page + console
python -m http.server 8080 -d build/render-web   # -> http://localhost:8080/context-render-web.html
```

### emdawnwebgpu build constraints (ROADMAP §5 risk table; `spikes/webgpu/FINDINGS.md`)

- `--use-port=emdawnwebgpu` — Dawn's maintained `webgpu.h` binding (replaces the removed
  `-sUSE_WEBGPU`); pins + auto-downloads the Dawn pkg at first link.
- `-sASYNCIFY` — the synchronous pump loop (`emscripten_sleep`) yields to the browser event loop so
  WebGPU promises resolve.
- Heap **sized up front** (`-sINITIAL_MEMORY`) and **NO `-sALLOW_MEMORY_GROWTH`**: growth makes the
  wasm heap a *resizable* `ArrayBuffer`, and emdawnwebgpu's string glue (`TextDecoder.decode` on a
  heap view) throws on resizable buffers → device acquisition dies.

## Bind-group parity (Tint combined-sampler split — `docs/wgsl-tool-decision.md`)

Free. The RHI's bind-group layouts are always **reflected** from the pipeline's shader
(`IRenderPipeline::bind_group_layout` → `wgpuRenderPipelineGetBindGroupLayout`, the WebGPU "auto"
layout), so Tint's post-split binding renumbering is picked up identically on web and native — no
caller ever hardcodes a pre-split binding map (see `../include/context/render/rhi.h`
`IBindGroupLayout`).

## CI

`.github/workflows/ci.yml` job **`render-web`** (→ rollup check `CI / render (web, emscripten)`) builds
this target to wasm+JS with emsdk `latest`, then **runs it in headless Chromium over SwiftShader
WebGPU** (M4 T7, issue #141; `tools/web_golden_run.py` — Chromium is the v1 blocking browser: the
only browser shipping WebGPU on headless Linux, and the design record's reference browser since the
M0 Tint/Chrome spike) and gates the collected frames against `goldens/` with
`tools/golden_compare.py` — the M4 "one browser blocking" **run** gate. Recorded in
`docs/ci-fleet-manifest.json` as the `render-web` + `golden-scene-web-chromium` gates. The
locally-runnable native guard on the same proof set is the `render-web-parity` ctest
(`../tests/test_web_parity.cpp`).

## Deferred follow-up

- The **lit/PBR** (3D lighting + shadow) web proof — it pulls the kernel + `extract` into the
  emscripten build (both are emscripten-safe pure C++, but a heavier surface, and the kernel is
  under active sibling churn). The lit scene is corpus-gated on the NATIVE leg today
  (`goldens/manifest.json` `lit3d`); its browser leg joins by adding the kernel + extract + lit
  sources to `CMakeLists.txt` here and extending `web_main.cpp` with the `lit3d` golden POST. The
  triangle (3D pipeline) + sprite (2D) proofs + goldens here are kernel-free.
