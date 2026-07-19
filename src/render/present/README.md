# `src/render/present/` — the presentation path (M9 e03)

`context_render_present`: everything only a WINDOWED shell needs — OSR import policy, the composite
pass, and the GPU-less present fallback. GPU-backend-free — it links `context_render` (the `rhi.h`
abstraction), no GPU backend and nothing from the editor tree, plus `gdi32` for the Windows GDI
fallback — so it builds and is fully unit-tested under the local dev gate and on all three CI `build`
legs. The wgpu-native `ISurface`/`ISwapchain` implementation lives in the CI-gated
`context_render_wgpu`, not here.

**The full engineering write-up is [`docs/present-path.md`](../../../docs/present-path.md)** — design
rationale, the per-platform import table, the owner ruling that re-scoped it, and the test map. This
README is the file-level index.

| File | Purpose |
|---|---|
| `osr_import.h` / `.cpp` | The per-platform accelerated-vs-CPU-upload **decision** (a pure function taking the platform explicitly, so all three branches run on every OS) + `OsrTextureImporter`, the dirty-rect upload driver. |
| `osr_composite.h` / `.cpp` | The fullscreen-triangle premultiplied-alpha composite: WGSL, blend state, `visible_rect/coded_size` UVs, and `composite_reference_cpu` — the GPU-free oracle. |
| `present_blit.h` / `.cpp` | Review C-F2: the OS blit seam, the Windows GDI implementation, the portable `MemoryBlitter`, and the aspect-preserving `compute_blit_plan`. |
| `osr_scene.h` | The GPU proof's fixture + render path (`render-wgpu-osr-composite`), asserted against the oracle. Header-only; its GPU-free half is compiled + tested locally. |

## The three things worth knowing before editing

1. **Nothing headless may link this target.** `cmake/ContextPresentIsolation.cmake` walks the
   transitive link closure of the daemon/runtime/CLI/client targets at configure time and fails the
   build if presentation is reachable. Adding a link edge from a headless target is not a style
   violation — it stops the build on every OS leg.
2. **Windows runs the CPU-upload path, by owner ruling (2026-07-19).** Stock wgpu-native exposes no
   external-texture import and the fork was rejected; the accelerated branch is kept dormant behind
   one predicate pending [gfx-rs/wgpu-native#621](https://github.com/gfx-rs/wgpu-native/issues/621).
   Do not delete it, and do not re-pin or fork a wgpu-native artifact.
3. **Degrades are never silent.** The RHI import fails closed; the importer's fallback records
   `degraded()` + a reason; a missing OS blitter reports which task owns it. The whole point is that
   a slow or blank editor is explainable from the artifact rather than from a bisect.
