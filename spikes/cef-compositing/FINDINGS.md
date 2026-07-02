# FINDINGS — CEF accelerated-OSR / shared-texture compositing (L-41, fifth M0 spike)

**Date:** 2026-07-02 · **Scope:** Windows = measured (the MUST platform); macOS/Linux =
findings-grade research from primary sources. Design frame: `DESIGN-DECISIONS.md` L-41 / L-15,
`REQUIREMENTS.md` R-UI-007.

## Verdict

**The seam holds on Windows — accelerated OSR + shared-texture compositing is PROVEN**, measured
end-to-end (render, composite, input, resize) on real hardware with zero errors. macOS is
credibly viable (IOSurface handed to the embedder directly; production-proven by OBS) with one
sharp caveat (external begin-frame is broken — let CEF self-pace). Linux is **conditionally**
viable only (Mesa drivers, exact flag incantation, NVIDIA-proprietary effectively broken;
no first-party sample exists) — on Linux the L-41 fallback tree is not theoretical, it is the
plan of record.

### L-41 fallback-tree recommendation (the deliverable)

L-41's ordered branches on accelerated-OSR failure: **(1)** windowed CEF + native child-window
viewport "hole" · **(2)** software-OSR for UI chrome only · **(3)** inverse composition (engine
renders into a WebGPU canvas inside CEF).

| Platform | Accelerated OSR viable? | Recommended branch | Evidence |
|---|---|---|---|
| **Windows** | **YES — measured here** | **Accelerated OSR, primary path.** No fallback needed; keep (2) compiled-in (it shares ~95 % of the integration and costs one flag). | This spike: 4/4 autotest runs green on RTX 4090, CEF 149 stable; §Windows results below. |
| **macOS** | YES (researched, not locally measured — no Mac hardware on this box) | **Accelerated OSR (IOSurface → `MTLTexture` via `makeTexture(descriptor:iosurface:plane:)`), CEF-internal frame pacing.** Fallback (2) software-OSR compiled-in. Do NOT design M5 around `SendExternalBeginFrame` — broken on macOS (cef#4033, cef#4166, both open). | `cef_types_mac.h` delivers a raw IOSurface; cefclient demos it (GL path); OBS ships shared-texture browser sources on macOS in production. |
| **Linux** | CONDITIONAL (researched): works on Mesa (Intel/AMD) under X11-ozone with `--use-angle=gl-egl --ozone-platform=x11`; NVIDIA proprietary broken/blacklisted; Wayland needs consumer-side workarounds; no first-party sample (cef#3687 open since 2024) | **Accelerated OSR behind a capability gate on Mesa; automatic fallback to (2) software-OSR for UI chrome (and on NVIDIA proprietary, default to (2))** — exactly the OBS production posture. Branch (1) windowed+hole is NOT recommended: it forfeits UI-over-viewport overlays (the editor wants gizmos/panels over the 3D view) and child-window "holes" are hostile to Wayland compositing. Keep (3) as last-resort research, not a plan. | cef#3953 (flags), cefclient commit `cf5fddc` (forces those flags), OBS obs-browser NVIDIA blacklist + dmabuf import (PR #453, OBS 31.1), cef#3687 (no sample). |
| **M5 editor shell** | — | **Build the UI-Provider seam so accelerated-OSR and software-OSR are the same code path except for the texture-update call** (this spike's `updateUiFromSharedHandle` vs `updateUiFromBuffer` — ~40 lines apart). Software-OSR is the universal floor: measured only ~4× the per-frame CPU of accelerated at 1280×800 (≈ 114 µs vs ≈ 27 µs — both < 1 % of a core at 60 Hz); it stays acceptable up to ~1440p chrome and degrades honestly (linear in pixels) beyond. | §Fallback cost below. |

**Bottom line for L-41:** the lock's assumption is CONFIRMED on Windows, plausible-confirmed on
macOS, and on Linux amended from "prove accelerated OSR" to "gate accelerated OSR by
driver/windowing capability, with software-OSR as the shipped default fallback." No redesign of
L-41 needed — branch (2) is simply promoted from fallback to co-equal default on Linux. Nothing
in the evidence requires branch (1) or (3); both remain unexercised last resorts.

## CEF pin (record)

| | |
|---|---|
| Version | **149.0.6+g0d0eeb6+chromium-149.0.7827.201** (Chromium 149.0.7827.201) |
| Channel | **stable** (current stable at spike time; beta was 150.0.3) |
| Distribution | `windows64_minimal` from the Spotify CDN (official CEF binary host), sha1 `fe8f461b743f03dc640e998ae08264407d8bc2c9` |
| Download size | **162.0 MB** (tar.bz2; the `standard` distro is 342.5 MB; other platforms: macosx64 min 128.7 MB, macosarm64 min 122.9 MB, linux64 min 311.0 MB) |
| Extracted | **416 MB** total; **runtime payload that must ship next to the exe ≈ 389 MB** (`Release/` 312 MB — `libcef.dll` alone is 258 MB — + `Resources/` 77 MB, of which 50 MB is all-languages `locales/`, prunable) |
| API notes | `cef_accelerated_paint_info_t` (Win) = NT shared handle **without keyed mutex** + `cef_color_type_t` + common info (`coded_size`, `visible_rect`, `capture_update_rect`, `capture_counter`). Frames come from Viz's capture pool and are recycled after the callback returns — copy out inside the callback. |
| Packaging note for M5 | Since ~M138 the Windows **sandbox requires the `bootstrap.exe` launch model** (app builds as a DLL); this spike runs `no_sandbox` with the classic single-exe + `CefExecuteProcess` subprocess re-entry model. The editor's packaging decision (sandbox vs not) is an M5 work item — it does not affect the compositing seam. |

## Windows results (measured)

**Rig:** Windows 11 Pro, NVIDIA GeForce RTX 4090, 160 Hz display. MSVC 17.14 (VS 2022),
Release x64. Demo: 1280×800 client, D3D11 flip-model swapchain, spinning triangle + CEF page
(opaque toolbar with animated spinner forcing continuous UI paints, transparent body);
`windowless_frame_rate=60`; single-threaded pump (`CefDoMessageLoopWork` per frame).
All four runs exited 0 with **zero errors and all pixel checks green**.

| Metric | Accel + vsync | Accel, uncapped | Software + vsync | Software, uncapped |
|---|---|---|---|---|
| Engine fps | **160.0** (display-limited) | **9,749** | 159.8 | 10,432 |
| Present-to-present avg / p95 / p99 (ms) | 6.251 / 6.332 / 6.42 | 0.103 / 0.193 / 0.70 | 6.258 / 6.338 / 6.44 | 0.096 / 0.187 / 0.62 |
| CEF UI paint rate (Hz) | 59.9 | 60.0 | 60.0 | 60.2 |
| Per-UI-frame CPU cost p50 / avg / p95 / max (µs) | **2.5 / 30.9 / 128 / 384** | 2.3 / 32.4 / 125 / 303 | **113.7 / 119.5 / 173 / 348** | 106.9 / 112.9 / 148 / 361 |
| Shared-handle (re)opens | 163 of 583 paints | 205 / 562 | — | — |

Readings:

- **The engine framerate is fully decoupled from the UI**: CEF paints at its 60 Hz
  `windowless_frame_rate` while the engine composites at 160 fps (vsync) or ~10,000 fps
  (uncapped) — UI-over-viewport compositing costs the engine one textured fullscreen-triangle
  draw + (accel) a GPU-GPU `CopyResource`.
- **Accelerated per-paint CPU cost is effectively zero** when the pool handle is cached
  (p50 2.4 µs = `CopyResource` enqueue). The p95 ≈ 125 µs is `OpenSharedResource1` on a
  not-yet-seen pool handle. Viz's capture pool recycles more aggressively than its nominal
  ~11 entries (≈ 30 % of paints delivered a new handle, more during resizes); caching per
  handle value keeps that bounded.
- **`shared_texture_enabled=1` produced zero `OnPaint` software callbacks** (and vice versa);
  the two paths are cleanly mutually exclusive.

### Input round-trip (proven, two independent ways)

Synthetic `WM_MOUSEMOVE`/`WM_LBUTTONDOWN·UP`/`WM_KEYDOWN·CHAR·UP` sent through the real
`WndProc` → `SendMouseMoveEvent`/`SendMouseClickEvent`/`SendKeyEvent` forwarding path:

- **Hover**: CSS `:hover` flipped the button pixel #2266CC → #FF8800 in the composited UI
  texture — read back and verified. Input → JS (`mouseenter`, title round-trip): **19.2 ms**
  vsync-quantized; **1.8–2.4 ms** in the uncapped runs (true IPC cost). Input → visible pixels:
  **19–31 ms** (bounded by the 60 Hz UI frame rate, as expected).
- **Click**: JS `click` handler fired (counter incremented, state strip #444444 → #22CC44
  verified by pixel readback). Title round-trip 19.3 ms (vsync) / **1.3–1.4 ms** (uncapped);
  pixels 25–44 ms. A **second click after the window was resized** also landed correctly at
  unchanged client coordinates (layout re-anchored properly).
- **Keyboard**: `keydown` reached the page. Bonus fidelity proof: the box runs a Cyrillic
  keyboard layout, and the page reported `e.key == "л"` for the `'K'` VK — i.e. real
  Chromium keyboard-layout translation ran, not a fake char injection.

Verdict: input latency is **not** a seam risk; it is bounded by the UI frame rate (≤ 1–2 UI
frames), with the IPC round-trip itself ~1–2 ms.

### Resize behavior

Two live resizes per run (1280×800 → ~1600×900 → ~1000×700), driving
`ResizeBuffers` + `WasResized()` + shared-handle cache flush:

- Converged in **3–5 CEF paints** (~50–80 ms) in both modes, both directions, every run; no
  crash, no device removal, no stuck frames, and the UI remained interactive after (click #2).
- **Transient:** at the instant the first new-size frame arrives, Chromium delivers the *old*
  layout scaled/letterboxed into the new surface (the same stretch you see resizing Chrome
  itself); full re-layout follows within a few frames and the settled frame is pixel-perfect
  (verified by dumped composites). An M5 shell that wants zero visible transient should hold
  the last stable frame or fade during live-drag — standard practice, not seam risk.
- `coded_size` can exceed `visible_rect` during resize; the composite must map UVs to
  `visible_rect/coded_size` (implemented here) or edge garbage shows.

### Fallback cost (software-OSR, measured — not estimated)

Same page, same autotest, `shared_texture_enabled=0` → `OnPaint` BGRA + dirty-rect
`UpdateSubresource`:

- **Per-UI-frame CPU: ≈ 114 µs vs ≈ 27 µs accelerated → ~4× the cost, but both are noise at
  editor-chrome scale**: at 60 Hz that is 0.7 % vs 0.16 % of one core at 1280×800. Full-surface
  worst case (first paint / resize, ~1600×940) measured ≈ 350–384 µs.
- Scaling is linear in dirty pixels: extrapolated full-frame software upload at 4K ≈ 1.5–2 ms
  CPU per UI frame — still workable for *chrome-only* UI at 30–60 Hz, painful for full-screen
  web content at high refresh. Hence branch (2)'s "UI chrome only" qualifier is right.
- Latency parity: hover/click/key round-trips within measurement noise of the accelerated path
  (both bounded by the 60 Hz UI rate).
- Engine throughput unaffected (10,432 fps uncapped vs 9,749 accel — same order; the delta is
  run-to-run noise, plus the accel path's per-frame `CopyResource`).

## Per-platform accelerated-OSR maturity matrix

| | Windows | macOS | Linux |
|---|---|---|---|
| Status | **Measured, green** | Researched | Researched |
| Delivery | D3D11 NT shared handle, **no keyed mutex**; open via `OpenSharedResource1`, copy in-callback (pool recycles after return) | Raw **IOSurface** (`cef_types_mac.h`: `shared_texture_io_surface`) | **dmabuf native pixmap**: up to 4 planes `{stride, offset, size, fd}` + DRM `modifier` (`cef_types_linux.h`) |
| Engine import | `ID3D11Device1::OpenSharedResource1` (this spike) | `MTLDevice.makeTexture(descriptor:iosurface:plane:)` (supported Apple API; no CEF Metal sample exists — cefclient uses deprecated CGL/OpenGL) | GL: `EGL_EXT_image_dma_buf_import(_modifiers)`; Vulkan: `VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier` (no known production Vulkan consumer — unverified) |
| First-party sample | `cefclient` D3D11 renderer | `cefclient` GL renderer | **None** — cef#3687 open since 2024-04 |
| Production proof | This spike; OBS; CefSharp | OBS browser sources (IOSurface) | OBS 31.1+ (obs-browser PR #453/#491): dmabuf zero-copy on Mesa, **NVIDIA-GL blacklisted at runtime** |
| Required flags | none | none | `--use-angle=gl-egl` + `--ozone-platform=x11` (cef#3953; cefclient now forces these for itself — commit `cf5fddc`, ~CEF 139) |
| Known issues | cef#4057 (NULL handle in 142/143 release builds — fixed before 149); cef#3968 (AMD VRAM churn at high UI fps) | cef#4033 / cef#4166: **external begin-frame broken** (blank window / incomplete frames) — self-paced mode unaffected | NVIDIA proprietary GBM flags broken (fix PR closed unmerged — status unverified); Wayland needs consumer workarounds (OBS carries an unexplained X11 modifier workaround) |
| History | The Viz-based reimplementation landed 2024-03 (commit `260dd0ca`, fixes cef#1006/#2575), first stable ~CEF 124/125. Landing commit verbatim: *"Verified to work on macOS and Windows. Linux support is present but not implemented for cefclient, so it is not verified to work."* | same | same |

(Research sources: CEF headers `cef_types_{win,mac,linux}.h` + `cef_types_osr.h`;
cef issues #2575/#1006/#3687/#3953/#3968/#4033/#4057/#4166; cefclient
`browser_window_osr_{win,mac,gtk}`; obs-browser `obs-browser-plugin.cpp` platform gates and
PRs #453/#491. Windows numbers: this spike, 2026-07-02.)

## Design-collision check (owner directive)

No loud collision with L-41's assumptions. Two honest amendments recorded:

1. **Linux "proven on" is unreachable as stated for v1**: accelerated OSR there is a
   driver/windowing-conditional capability, not a platform property. The lock's own fallback
   tree absorbs this (branch (2) becomes the Linux shipped default; accelerated is a gated
   fast path) — recommendation above, risk row stays open until an M5-era Linux measurement.
2. **Frame pacing constraint**: any M5 design that wanted the engine to drive CEF's cadence
   via external begin-frames would collide with open macOS bugs. Use CEF-internal pacing +
   `windowless_frame_rate` (this spike's model, measured decoupled and clean).

## Spike-scope caveats (stated, not hidden)

- macOS/Linux rows are research-grade; no local hardware measurement (task scope).
- Single-threaded pump: production will use `multi_threaded_message_loop=true` + a mutex on the
  texture handoff; changes locking, not the seam.
- `PET_POPUP` (select dropdowns etc.) not composited in the spike; a known, mechanical addition.
- DPI pinned to 1.0 (`GetScreenInfo`); per-monitor DPI is an M5 work item.
- Sandbox disabled (see packaging note). Vsync'd latency numbers are quantized by the 160 Hz
  present loop; uncapped runs give the true IPC cost.
