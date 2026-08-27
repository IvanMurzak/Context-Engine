---
id: e03-present-texture-import
title: ISurface/ISwapchain present path + IDevice external-texture import + composite pass (production) + CPU present fallback
group: B
sequence: 2
repo: "."
base_branch: "main"
depends_on: []   # was [s2-wgpu-shared-texture-spike] — s2 SUPERSEDED by owner ruling 2026-07-19
superseded_by: owner-ruling-2026-07-19-no-wgpu-fork   # see the ⚠ banner below + ROADMAP.md
importance: 8
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [01, 03]
---

> # ⚠️ AMENDED BY OWNER RULING — 2026-07-19. READ BEFORE THE BODY.
>
> **DO NOT FORK, PATCH, OR RE-PIN `wgpu-native`.** The owner **rejected** the patched-fork
> approach (carrying a fork = unbounded long-term maintenance cost). **`s2` is SUPERSEDED** and
> this task's `depends_on` is void. Every sentence below that reads "the s2-ratified patched
> wgpu-native prebuilt" is **dead text kept for history** — implementing it would do the exact
> thing the owner rejected.
>
> **What actually applies, per OS:**
> - **Windows** — accelerated DXGI shared-handle import is **OUT OF SCOPE / deferred**. Ship the
>   **CPU-upload** path (`OnPaint` BGRA → `wgpuQueueWriteTexture`, dirty rects), ~114 µs/frame,
>   accepted **for the Editor on Windows ONLY**. Keep the accelerated↔software flag/seam and leave
>   the Windows accelerated branch disabled with a comment pointing at the upstream ask:
>   <https://github.com/gfx-rs/wgpu-native/issues/621>.
> - **macOS** — **unaffected, stays accelerated** via **STOCK** native accessors
>   (`wgpuTextureGetNativeMetalTexture`, upstream wgpu-native #557) → IOSurface → Metal blit. No
>   fork involved.
> - **Linux** — unchanged: software upload (dmabuf only behind the accel gate, ships OFF).
>
> **DoD line DROPPED:** "patched-prebuilt pin fetch-verified fail-closed" — no patched prebuilt
> exists. The stock wgpu-native pin stands untouched.
>
> Authoritative record: [`../ROADMAP.md`](../ROADMAP.md) — Backlog section + the 2026-07-19
> progress-log entries. **This task LANDED under the amended scope** (CE PR #310 `4972ee0f`).

## Goal

Productionize the present + OSR-interop seams: implement the declared `ISurface`/`ISwapchain`
stubs, add `IDevice::import_external_texture` over the s2-ratified patched wgpu-native
prebuilt, build the premultiplied composite pass, and mechanize the CPU present fallback —
keeping the headless invariant (only the Shell links presentation).

## Scope & seams

- **Present path** (`rhi.h:374-384` stubs → `wgpu_rhi.cpp`, the sole `webgpu.h` TU):
  `IInstance::create_surface(native_window_desc)` with per-OS source structs (HWND /
  CAMetalLayer-backed NSView / X11 window); `ISwapchain` configure (BGRA8Unorm — reserved at
  `rhi.h:40` — Fifo default, capabilities-checked as `spikes/webgpu/main.cpp:546-560`),
  `acquire()`, `present()`, `resize()` reconfigure, `unconfigure()`. Viewport RTs stay
  RGBA8Unorm. `AdapterProbe` gains `can_present(surface)`.
- **External-texture import** (net-new seam on `IDevice`, `rhi.h:322-338`): Windows = the s2
  patched-prebuilt C entry (D3D12, CEF DXGI NT shared handle; reopen-per-paint default,
  cached-pool behind a flag — B-F10); macOS = STOCK native accessors
  (`wgpuTextureGetNativeMetalTexture` et al., raw IOSurface → Metal blit); Linux = software
  upload (dmabuf only behind the accel gate, ships OFF). Driven by
  `SurfaceHandoff.shared_texture` (`surface.h:54-57`).
- **CPU-upload fallback path**: `OnPaint` BGRA → `queue.writeTexture` dirty rects — one flag
  switches accelerated/software (L-41 discipline).
- **Composite pass**: wgpu fullscreen-triangle premultiplied-alpha composite beside
  `render/ui/composite.h` (blend ONE/INV_SRC_ALPHA; UV = `visible_rect/coded_size` —
  `renderer_d3d11.cpp:210-272` reference).
- **CPU present fallback** (C-F2, GPU-less host): OS-level blit seam for the Shell (GDI
  `StretchDIBits` / X11 SHM / `CALayer.contents`) — interface + Windows impl here; e12 fills
  the other OS impls.
- **Headless invariant**: nothing in daemon/runtime targets links the present path — asserted
  structurally (link-graph / target-dependency check).

## Definition of Done

- [ ] Swapchain present proven: Linux under xvfb + macOS runner in CI; Windows windowed
      verified on the interactive box (Session-0 carve-out honored, 09 §3)
- [ ] Import path: accelerated (patched prebuilt) + CPU-upload fallback, flag-switchable;
      composite output pixel-asserted offscreen (golden or checksum test)
- [ ] CPU present fallback interface landed with Windows impl + T1 coverage
- [ ] No daemon/runtime target links presentation (CI assert)
- [ ] 3-OS CI green; patched-prebuilt pin fetch-verified fail-closed
