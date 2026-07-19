# The present path — surface, swapchain, OSR import, composite, CPU fallback

How a rendered frame reaches a window, and how the browser's UI layer gets composited over it. Landed
by M9 e03. This records how the repository implements the design; the normative records live in the
owner's design authority (design 03 §2–§4, §7; `R-HEAD-002`/`R-HEAD-004`, `L-41`, review `C-F2`).

The load-bearing property is stated first because everything else is arranged around it:

> **Only the Shell links presentation.** No daemon, runtime, or CLI target may reach the present
> path. This is enforced, not documented — see § Headless invariant.

## Layout

| Where | What |
|---|---|
| `src/render/include/context/render/rhi.h` | The abstract seam: `ISurface`/`ISwapchain`, `NativeWindowDesc`, `SurfaceCaps`, `AcquiredFrame`, external-texture import, blend state. Dependency-free — includable from headless code. |
| `src/render/present/` (`context_render_present`) | The GPU-backend-free half: import policy, dirty-rect upload driver, composite pass, CPU present fallback. Builds and is unit-tested on the local dev gate and all three CI `build` legs. |
| `src/render/src/wgpu/wgpu_rhi.cpp` | The wgpu-native implementation. Still the ONLY TU that includes `webgpu.h`. CI-gated behind `CONTEXT_BUILD_RENDER_WGPU`. |
| `src/render/src/wgpu/metal_interop.mm` | Objective-C++, Apple-only: the IOSurface → Metal blit behind the macOS accelerated import. |
| `cmake/ContextPresentIsolation.cmake` | The headless-invariant gate (configure-time, transitive). |

## 1. Surface and swapchain

`IRhi::create_surface(NativeWindowDesc)` wraps a native window; the descriptor carries an OPAQUE
`void*` handle plus a `NativeWindowKind` tag (`Win32Hwnd` / `XlibWindow` / `WaylandSurface` /
`MetalLayer`), which is what keeps `<windows.h>`, X11 and Cocoa out of `rhi.h`.

Then: `ISurface::capabilities()` → `ISurface::configure(device, SurfaceConfig)` → `ISwapchain`, whose
per-frame surface is `acquire()` → render → `present()`, with `resize()` and `unconfigure()` for the
window lifecycle.

Three details that are easy to get wrong and are therefore pinned by tests:

- **Capability-check before configuring.** Configuring with an unsupported format or present mode is
  a validation error, not a graceful refusal, so `configure()` checks first and returns `nullptr`.
  It never silently substitutes a mode — that would present the whole editor at the wrong pacing
  with nothing reporting it. Swapchains are **BGRA8Unorm** + **Fifo** by default (Fifo is the one
  present mode WebGPU guarantees on every surface); viewport render targets stay RGBA8Unorm.
- **`Outdated`/`Lost` are normal, not errors.** A resize racing a frame, or a device going away,
  yields no view — the compositor reconfigures and carries on. `Suboptimal` is different: the frame
  IS presentable and must be presented, then reconfigured.
- **A zero-extent resize is ignored.** A minimized window reports one every frame; tearing the chain
  down and rebuilding it each time would thrash.

`AdapterProbe::can_present`, filled by `IRhi::probe_surface(surface)`, is the editor's GPU gate. It
creates no device (R-HEAD-002). The plain `probe()` always reports `can_present == false` — "nobody
asked about a surface" is not "cannot present".

## 2. OSR import — how a browser frame becomes a texture

The policy lives in `present/osr_import.h` as a pure function over an explicit platform argument, so
every platform's branch is compiled and executed by the ctest on every OS.

| Platform | Path | Why |
|---|---|---|
| **Windows** | CPU upload | Stock wgpu-native's C API exposes **no** external-texture import, and the owner **rejected** carrying a patched fork on 2026-07-19 (unbounded long-term maintenance cost). Upstream ask: [gfx-rs/wgpu-native#621](https://github.com/gfx-rs/wgpu-native/issues/621). Measured cost ~114 µs/frame vs ~27 µs zero-copy — accepted for the Editor on Windows only. |
| **macOS** | Accelerated (IOSurface → Metal blit) | wgpu-native ships the STOCK native Metal accessors (upstream PR #557): `wgpuDeviceGetNativeMetalDevice`, `wgpuQueueGetNativeMetalCommandQueue`, `wgpuTextureGetNativeMetalTexture`. No fork needed. |
| **Linux** | CPU upload | dmabuf import only behind the accel gate, which **ships OFF**. |

The Windows accelerated branch is **dormant, not deleted**: flipping
`OsrImportOptions::windows_shared_handle_import_available` restores it (a ctest asserts exactly
that), so when upstream lands the C API the path returns without re-architecting anything.

`OsrImportOptions::force_software` is the single L-41 switch that forces the software path on any
platform. It is checked first and unconditionally.

**Never a silent degrade.** `IDevice::import_external_texture` FAILS CLOSED — a source the backend
cannot honour returns a null texture plus a diagnostic. `OsrTextureImporter` is the layer that may
then fall back to CPU upload, and when it does it records `degraded() == true` plus the reason. A
silent degrade here is a ~4× per-paint regression nobody would notice until a user reports a sluggish
editor.

**Accelerated is not always one-shot.** macOS delivers a NEW IOSurface per paint, so the importer
calls `IDevice::refresh_external_texture` every frame (a GPU-side blit, no CPU roundtrip). An import
that ran only once would freeze the UI on its first frame while still reporting success.

**Dirty rects.** On the CPU path only the damaged sub-rects are uploaded, via
`IQueue::write_texture_region`. Two things this depends on: the destination `Origin2D` (without it
the damage lands at the texture's top-left corner and still "succeeds"), and using the SOURCE
frame's stride for a rect carved out of a larger frame (CEF frames are commonly padded).

## 3. Composite pass

`present/osr_composite.h` — a fullscreen-triangle draw of the UI layer over whatever the window
compositor already rendered. Three things must agree, and all three live in that one header so they
cannot drift apart:

1. **Blend** — `ONE / INV_SRC_ALPHA` on colour AND alpha. CEF hands over PREMULTIPLIED pixels, so the
   source is added, not lerped; `SRC_ALPHA/INV_SRC_ALPHA` (the un-premultiplied classic)
   double-darkens every antialiased edge in the UI.
2. **UV** — the sampled sub-rect is `visible_rect / coded_size`, not `[0,1]`. The producer allocates
   in chunks, so sampling the whole texture stretches unused margin across the window.
3. **Orientation** — browser rows are top-down, clip space is y-up; the vertical UV is flipped in the
   vertex stage.

`composite_reference_cpu()` is the GPU-free oracle for that arithmetic. It is what pins the composite
locally on a GPU-less host, and `render-wgpu-osr-composite` asserts the REAL backend against it
per-pixel. The fixture (`present/osr_scene.h`) is deliberately adversarial: the region outside
`visible_rect` is magenta (absent from the expected output, so a wrong UV is loud) and the visible
region is half opaque / half 50%-premultiplied (so an un-premultiplied blend diverges on one half
while the other still looks fine).

## 4. CPU present fallback (C-F2)

The promise is "the editor UI never REQUIRES a GPU". When `can_present` is false there is no
swapchain and no composite pass; the Shell blits the software-OSR buffer through an OS 2D primitive
and viewport panels draw their diagnostic placeholder.

`present/present_blit.h` lands the seam plus the **Windows GDI** implementation (`StretchDIBits` into
the window DC, top-down DIB, tight repack for a padded stride, bars filled before a letterboxed
present). X11 SHM and `CALayer.contents` are **e12's** — and that gap is REPORTED, not silent:
`make_present_blitter` returns a `BlitterSelection` carrying an explicit diagnostic naming e12, so a
caller degrades loudly instead of quietly presenting nothing.

`compute_blit_plan` (aspect-preserving, centred, pure integer math) is kept apart from every OS call,
which is what lets the geometry be pixel-asserted on all three OSes via `MemoryBlitter` — a portable
blitter running the identical plan into a buffer.

## 5. Headless invariant

`cmake/ContextPresentIsolation.cmake` walks the **transitive** link closure of
`context_runtime_server`, `context_runtime_desktop`, `context` and `context_client`, and
`message(FATAL_ERROR)`s if `context_render_present` or `context_render_wgpu` is reachable. It runs at
configure time, so a violation fails the build on every OS leg before anything compiles.
Transitivity is what makes it strong: presentation cannot sneak in through an intermediate library.

`context_render` (the GPU-free RHI abstraction + extract) is deliberately NOT forbidden —
`context_runtime_desktop` links it on purpose. What headless targets must never reach is windows,
swapchains, OS blits, or a GPU backend.

The companion ctest `render-present-headless-isolation` re-reads the audit report. Its job is the
failure mode a `FATAL_ERROR` structurally cannot catch: the gate silently covering NOTHING after a
target rename. A gate that checks nothing reads exactly like a gate that passed.

## 6. Test map

| Test | Covers |
|---|---|
| `render-present-test_osr_import` | All three platform policies, `force_software`, clipping, dirty-rect uploads (incl. the origin and padded-stride traps), reimport on resize, malformed frames, the loud degrade, the per-frame accelerated refresh |
| `render-present-test_osr_composite` | UV math, pipeline blend state, the premultiplied oracle (opaque / transparent / half-alpha), sub-image selection, top-down orientation, the GPU fixture's adversarial properties |
| `render-present-test_present_blit` | Letterbox/pillarbox geometry incl. degenerate sizes, the memory blitter's pixels + swizzle + bars, the never-silent platform selection |
| `render-present-test_swapchain` | Surface creation, the `can_present` gate, capability-gated configure, the frame loop, Outdated/Lost/Timeout/Suboptimal, resize + idempotent teardown |
| `render-present-headless-isolation` | The audit actually ran and covered every expected target |
| `render-wgpu-osr-composite` | The REAL backend's composite output, per-pixel against the CPU oracle (`render` CI job, lavapipe) |

All `render-present-*` tests are a plain (non-gate) family: the `build` job's general ctest step runs
them on all three OS legs and `--preset dev` builds them, so **no `ci.yml` `--target` bookkeeping** —
the "Not Run = RED" tripwire does not apply. `render-wgpu-osr-composite` adds no new executable
either; the `render` job's `^render-wgpu-` regex picks it up.

## 7. What this does NOT yet do

Named so the gaps are visible rather than assumed:

- **No window manager.** Creating and pumping an OS window is e04's; e03 lands the path a window
  will drive.
- **No `PET_POPUP` second OSR layer.** Required for production dropdown/select widgets (03 §4).
- **No X11 / macOS CPU present blitter** — e12.
- **The macOS accelerated path has no runtime CI proof.** It compiles on the `render (macos-latest)`
  leg, but exercising it needs a live IOSurface from a real CEF host, which arrives with the Shell.
- **Windows accelerated import is deferred**, pending gfx-rs/wgpu-native#621.
