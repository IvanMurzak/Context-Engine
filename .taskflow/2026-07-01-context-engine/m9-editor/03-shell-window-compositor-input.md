# 03 — Shell: windows, compositor, present, input

The Shell is the native C++ layer. Everything here has a measured reference implementation in the
two spikes (`spikes/webgpu` windowed present; `spikes/cef-compositing` OSR composite + input) —
M9 productionizes those patterns behind real seams (01 §6).

## 1. Window manager

- **Model**: `WindowManager` owns N `EditorWindow`s. Each `EditorWindow` = native handle
  (HWND / NSWindow / X11 window), an `ISwapchain`, one CEF browser (windowless-OSR mode with the
  window as the device-context owner), a `WindowCompositor`, an input binding, and one
  editor-core instance. Window 0 hosts the app menu + welcome screen (D13); windows are docking
  peers otherwise.
- **Per-OS backends** (one interface, three impls — mirroring `compositor/surface.cpp`'s single
  platform `#if` discipline):
  - Windows: `RegisterClassW/CreateWindowExW` + WndProc pump (ref `spikes/webgpu/main.cpp:506-522`,
    `cef-compositing/main.cpp:582-599`); per-monitor-v2 DPI awareness (spike pinned DPI 1.0 —
    M9 implements real DPI: CEF `device_scale_factor`, swapchain resize, layout scale).
  - macOS: NSWindow + NSView; CEF via `CefScopedLibraryLoader` app-bundle model (existing
    `host/CMakeLists.txt:61-113` packaging); IOSurface OSR delivery; CEF-internal pacing only
    (never `SendExternalBeginFrame` — `surface.cpp:39`, cef#4033).
  - Linux: X11 (+XWayland) per D21; software-OSR default, accelerated behind the Mesa/X11-ozone
    gate (L-41 tree as encoded in `select_mode`, `surface.cpp:8-30`).
- **Popup interception**: `CefLifeSpanHandler::OnBeforePopup` suppresses stray `window.open`
  (tear-out does NOT ride `window.open` — it is a PanelHost/Shell mechanism; 04 §2). DevTools
  from an OSR browser needs explicit handling — an `OnBeforeDevToolsPopup`-hosted window or the
  remote-debugging port (dev-loop only); naive pass-through does not display (review B-F11).
- **Message loop**: production uses `multi_threaded_message_loop=false` + integrated pump on the
  shell's main thread (predictable frame pacing with present; the spike caveat "prod =
  multi-threaded + mutex" is REJECTED in favor of a single-threaded owner loop with
  `CefDoMessageLoopWork` scheduled by `OnScheduleMessagePumpWork` — simpler invariants, and the
  compositor already decouples engine FPS from CEF's 60 Hz).
- **Lifecycle/persistence**: window placement (monitor, rect, maximized) persisted with the
  layout in `.editor/editor-state.json` (see the ownership split below); crash-safe debounced
  writes.
- **Session-file ownership split** (an explicit, named refinement of L-20's file mapping — the
  authored-vs-session LAW is untouched): the **daemon** is the single writer of
  `.editor/session.json` (daemon session state, incl. the 05 §4 selection/camera/play
  persistence); the **editor app (Shell)** is the single writer of `.editor/editor-state.json`
  (dock layout, window placement, panel state blobs, undo journal, editor presence marker).
  One writer per file — no cross-process torn writes; both are gitignored session state
  (review C-F3).

## 2. Present path (`ISurface`/`ISwapchain`)

Implement the declared stubs (`rhi.h:374-384`) in `wgpu_rhi.cpp` (the only `webgpu.h` TU):

- `IInstance::create_surface(native_window_desc)` → `wgpuInstanceCreateSurface` with
  per-OS source structs (HWND / CAMetalLayer-backed NSView / X11 window).
- `ISwapchain`: configure (BGRA8Unorm, present mode Fifo default; capabilities-checked as in
  `spikes/webgpu/main.cpp:546-560`), `acquire()` → current texture view, `present()`,
  `resize(w,h)` (reconfigure), `unconfigure()` on teardown.
- Formats: swapchain BGRA8Unorm (already reserved `rhi.h:41`); viewport RTs stay RGBA8Unorm.
- Headless invariant preserved: nothing in the daemon/runtime links the present path; only the
  Shell does. `AdapterProbe` gains a `can_present(surface)` check for the editor's GPU gate.
- **CPU present fallback** (GPU-less host / ultimate degrade): the editor UI never REQUIRES a
  GPU — when no usable adapter exists (or after unrecoverable device loss), the Shell presents
  the software-OSR CEF buffer via an OS-level blit (GDI `StretchDIBits` / X11 SHM /
  `CALayer.contents`). Viewport panels then render the diagnostic placeholder (02 §6). This is
  what makes "panels work on a GPU-less host" an honest, mechanized promise (review C-F2).

## 3. OSR interop (CEF texture → engine GPU context)

> ### ⚠️ SUPERSEDED 2026-07-19 — OWNER RULING: NO `wgpu-native` FORK. READ BEFORE THE BODY.
>
> **DO NOT FORK, PATCH, OR RE-PIN `wgpu-native`.** The owner **rejected** the patched-fork
> approach — carrying a fork is an unbounded long-term maintenance cost (every upstream rebase,
> forever) for a per-frame optimization. **`s2` is SUPERSEDED**; its premise (prove the fork
> import) is void. The original per-OS reasoning is preserved below as history, but the
> **"Windows (accelerated primary) = our patched wgpu-native prebuilt"** bullet is **dead text** —
> implementing it would do the exact thing the owner rejected.
>
> **What actually applies, per OS:**
>
> | OS | Path | Fork? |
> |---|---|---|
> | **Windows** | Accelerated DXGI shared-handle import is **OUT OF SCOPE / deferred**. Ships the **CPU-upload** path (`OnPaint` BGRA → `wgpuQueueWriteTexture`, dirty rects) — measured **~114 µs/frame** vs ~27 µs zero-copy, accepted **for the Editor on Windows ONLY**. ⚠ That budget is **NOT** sanctioned for any frame-rate-sensitive surface (runtime/game presentation, high-refresh or high-resolution targets) — those need the upstream feature or a separate owner decision. | none |
> | **macOS** | **UNAFFECTED — stays accelerated.** Raw IOSurface → Metal blit via **STOCK** native accessors (`wgpuTextureGetNativeMetalTexture`, upstream wgpu-native PR #557). Landed in e03 as `metal_interop.mm`. | none |
> | **Linux** | Unchanged: software upload; dmabuf import only behind the accel gate, ships **OFF**. | none |
>
> The accelerated↔software **flag/seam is RETAINED** (L-41 discipline) so the Windows path can
> slot back in unchanged when upstream lands the C API; the unimplemented Windows accel branch
> carries a comment pointing at the upstream ask:
> <https://github.com/gfx-rs/wgpu-native/issues/621>. Watch that issue — it is the revisit
> trigger recorded in the ROADMAP Backlog.
>
> Authoritative record: [`ROADMAP.md`](ROADMAP.md) — Backlog section + the 2026-07-19
> progress-log entries. **e03 LANDED under this amended scope** (CE PR #310 `4972ee0f`).

Net-new `IDevice::import_external_texture(handle_desc)` (seam `rhi.h:322-338`). ⚠ Review
ground truth (B-F1): **stock wgpu-native v29 exposes NO external-memory / shared-handle import
in its C API** (interop is a long-open upstream request; only Metal-native accessors exist).
The capability DOES exist one layer down, in wgpu's Rust core (`create_texture_from_hal`; D3D11
shared handles were wired for Vulkan in wgpu PR #6161). Therefore:

- ~~**Windows (accelerated primary) = our patched wgpu-native prebuilt**: a small C entry over
  `create_texture_from_hal`, importing CEF's DXGI NT shared handle on the D3D12 backend. We
  already own SHA-pinned prebuilt supply chains (CEF, V8, wgpu-native itself) — the fork is a
  pinned, patch-carrying build of the same v29 line, and **spike s2's job is to prove exactly
  this**, together with sandbox ON + `shared_texture_enabled` + windowless (regression
  precedent cef#4057 → per-CEF-bump CI tripwire with automatic software-OSR degrade — B-F5).
  Handle discipline: the CEF header contract says reopen the shared handle on EACH
  `OnAcceleratedPaint`; the measured per-handle-value cache (~11-deep pool,
  `renderer_d3d11.cpp:294-331`; no keyed mutex — header-documented) ships as an optimization
  behind a flag, with the contract-compliant reopen path as the default-safe fallback (B-F10).~~
  ⛔ **DEAD — owner REJECTED the fork 2026-07-19 (see the banner above).** Kept verbatim as the
  historical rationale and as the design of record for the day upstream #621 lands. The
  sandbox-ON + `shared_texture_enabled` + windowless assertion and the per-CEF-bump tripwire
  (B-F5) do NOT die with it — they re-home to T2/e15 (07 §2), where they gate macOS
  acceleration and the software-degrade behavior on every OS.
- **Windows = CPU upload (THE SHIPPING PATH, not a fallback)**: `OnPaint` BGRA buffer →
  `queue.writeTexture` with dirty-rects (`updateUiFromBuffer` pattern). This — NOT a "GPU interop
  layer", which is equally unexpressible in the stock C API — is the always-works path; one flag
  switches paths (L-41). ⚠ **Amended 2026-07-19:** this was written as the *stock-API fallback*;
  it is now the **primary and only** Windows path for M9.
- **macOS**: raw IOSurface → Metal blit via the STOCK native accessors
  (`wgpuTextureGetNativeMetalTexture` et al.) — no fork needed; fallback = software upload.
  ✅ Unchanged by the ruling — this is why macOS keeps hardware acceleration.
- **Linux**: software-OSR default (upload path); dmabuf import only behind the accel gate,
  research-grade — ships OFF unless the gate proves it. ✅ Unchanged by the ruling.

## 4. Per-window compositor

Each `WindowCompositor` frame:

1. Acquire swapchain texture.
2. Draw viewport layers: for each viewport panel instance visible in THIS window, draw its
   render-target texture into the panel's content rect (letterboxed per viewport config).
3. Draw the CEF layer: full-window premultiplied-alpha composite of the window's OSR texture
   (blend ONE/INV_SRC_ALPHA; UV = `visible_rect/coded_size` — `renderer_d3d11.cpp:210-272`).
   Editor-core keeps viewport content rects transparent (alpha 0) so native content shows
   through — the "transparent hole" contract; CEF UI (menus, overlays, drag ghosts, gizmo HUD
   if HTML) naturally draws OVER viewports.
4. Present. Engine render rate is decoupled from CEF paint rate (measured in the spike); redraw
   is damage-driven: CEF paint events, viewport RT updates, input, resize.

Resize protocol: native resize → swapchain reconfigure + `WasResized()` + shared-handle cache
clear; convergence in a few CEF paints is expected and handled (spike resize findings).
⚠ **Amended 2026-07-19:** "shared-handle cache clear" applies only where an accelerated import
is live — i.e. **macOS** (§3). On **Windows** (CPU-upload) and **Linux** (software) there is no
shared-handle cache; the equivalent step is reallocating the upload destination texture to the
new `coded_size`. Everything else about the protocol is unchanged.

`PET_POPUP` (dropdown/select widgets): composite the popup rect as a second OSR layer —
REQUIRED for production (spike explicitly skipped it; select/context menus depend on it).

## 5. Viewports (N, Scene|Game)

- **Render-side additions**: a `Camera`/`View` abstraction in the render world (none exists —
  01 §4): per-viewport `{camera transform, projection, mode 2D/3D, type Scene|Game, viewport id}`.
  Extract fills per-view visible sets via the shared spatial index; each view renders into a
  persistent per-viewport RT (allocated via the `DynamicTextureRegistry` pattern).
- **Scene viewport**: editor camera (session state, daemon-side per 05 §4), edit-time overlays
  (grid, selection outline, gizmos). **Game viewport**: renders the runtime camera of the play
  session; no edit overlays; respects play-mode indicator (L-51).
- **Editor camera controls** live in the Shell input layer (orbit/pan/zoom/fly), writing camera
  session state through the bridge (so agents can observe/set the camera too).
- **Picking**: pointer → ray through the viewport camera → spatial-index query (the
  `PanelMeshRaycaster` broad-phase-pruned pattern generalizes); 2D mode = point/AABB query.
  Pixel-perfect pick (ID buffer) is an optimization slot, not v1-required.
- **Gizmos**: native-rendered overlay pass in the Scene view (translate/rotate/scale), driving
  the EXISTING gesture verbs (`viewport_edit_model.h:117-144` begin/translate/commit) — the
  logic layer is already built and tested; M9 wires real input to it.
- GPU-less host: viewport panels render a diagnostic placeholder (02 §6).

## 6. Input pump and routing

Per-window input **binding**, dispatched from the single shell-owned pump of §1 (ref
`cef-compositing/main.cpp:262-352`):

1. OS event → normalize (device, code, position, modifiers, DPI-scaled).
2. **Region arbitration**: hit against the window's region map — editor-core publishes viewport
   content rects + "native-interaction" regions each layout change (a tiny per-window shared
   state). Pointer in a viewport rect → the native viewport path; everywhere else → CEF
   (`SendMouse*/SendKey*Event`).
3. Viewport path: camera controls / picking / gizmo gestures → panel-model verbs via bridge;
   emits pick/gesture facts onto `editor.ui`.
4. Keyboard: keys go to CEF when a DOM editable has focus (editor-core reports focus class over
   the bridge); otherwise resolved against the command keymap (when-contexts, 05 §3); unresolved
   keys fall through to CEF.
5. The `InputRouter`/`UiInputRouter` capture-stack shape (modal swallow vs overlay fall-through,
   single sink — `input_routing.h:62-112`) is reused as the arbitration model between editor
   chrome, panels, and viewport.
6. Every dispatch stamps the R-HUX-011 instrumented timestamps (input→commit→derive→paint).

## 7. Failure modes

| Failure | Behavior |
|---|---|
| GPU device lost / adapter gone | Rebuild device + swapchains + RTs; CEF falls to software-OSR; session intact (daemon unaffected) |
| No usable GPU adapter at all | CPU present path (§2) for the whole UI; viewport panels show the diagnostic placeholder |
| CEF renderer process crash | CEF respawns browser; editor-core reloads; layout+panel state restore from session store (D6) — a designed recovery drill in T2 |
| Daemon connection lost | Reconnect with backoff; subscription consumer re-snapshots (gap protocol); read-only banner until reattached |
| Window destroyed with panels | Panels rehome to window 0 (serialize→recreate) — never silently lost |
| Secondary-window create fails | Popout degrades to a floating Dockview group inside the source window, loudly |
