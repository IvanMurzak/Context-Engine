# src/render/ — the render subsystem

`README.md` here is the full write-up; this is the map.

- `include/context/render/` + `src/` — `context_render`: the backend-free tiered RHI (`rhi.h`), the
  L-39 sim→render extract + double buffer, the detachable `Renderer`, and the editor viewport path
- `src/wgpu/` — `context_render_wgpu`, the native wgpu-native backend (`CONTEXT_BUILD_RENDER_WGPU`, default OFF; CI-gated)
- `web/` — `context_render_web`, the browser backend; standalone Emscripten build (`emcmake cmake -S src/render/web`), not part of the native build
- Feature sub-packages, each with a README: `material/`, `sprite/`, `lit/`, `ui/` (M7 GPU UI backend, `render-ui-*`), `present/` (surface/swapchain, OSR import, composite, CPU fallback, `render-present-*` — `docs/present-path.md`), `shadercc/` (shader cross-compile, `shader-*`)
- `tests/` — fake-backend ctests that run under every toolchain

## Area rules

- `context_render` and `present/` stay GPU-backend-free and build under the local GCC gate; only
  `src/wgpu/` includes `webgpu.h`. The kernel never depends on render.
- Bind-group layouts come only from pipeline reflection (`docs/wgsl-tool-decision.md`) — never hardcode one.
- A new golden scene edits the hardcoded scene loops of the `render`/`render-web` jobs in `ci.yml`
  and adds its baseline under `goldens/` (a reviewed change).
