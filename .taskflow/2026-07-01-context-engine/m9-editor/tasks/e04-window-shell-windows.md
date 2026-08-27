---
id: e04-window-shell-windows
title: Window shell v1 (Windows) — native window, integrated pump, windowed-OSR CEF, compositor, input arbitration, DPI, PET_POPUP
group: B
sequence: 3
repo: "."
base_branch: "main"
depends_on: [e03-present-texture-import]
importance: 9
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [01, 03]
---

> # ⚠️ AMENDED BY OWNER RULING — 2026-07-19. READ BEFORE THE BODY.
>
> **DO NOT FORK, PATCH, OR RE-PIN `wgpu-native`, and do NOT implement a Windows accelerated
> import.** The owner rejected the patched-fork approach; `s2` is superseded (see
> [`e03-present-texture-import.md`](e03-present-texture-import.md)'s banner). Per OS:
> - **Windows** — `OnPaint` → **CPU-upload** is the SHIPPING path (~114 µs/frame, accepted for the
>   Editor on Windows only). The `OnAcceleratedPaint` → e03-import branch stays a **disabled seam**
>   behind the existing flag, commented to <https://github.com/gfx-rs/wgpu-native/issues/621>.
> - **macOS** — accelerated via **STOCK** native accessors (landed in e03, `metal_interop.mm`).
> - **Linux** — software upload.
>
> So the body's "`OnAcceleratedPaint` → e03 import" reads: *wire the seam, leave the Windows
> accelerated branch unimplemented.* The DoD line "Accelerated and software OSR paths both work
> behind the flag" is **amended below** — do not gate completion on a Windows accelerated path.
> Authoritative ledger: [`../ROADMAP.md`](../ROADMAP.md) (2026-07-19 entries).
>
> **Status note:** this task ran as `15e18035b085` and its code landed on PR
> [#312](https://github.com/IvanMurzak/Context-Engine/pull/312); the run halted only on an external
> GitHub Actions outage that never dispatched the `windows-latest` legs.

## Goal

Stand up `context_editor` — the native Shell — on Windows: a real OS window with the
single-threaded owner loop, a windowed-OSR CEF browser composited through e03's present path,
the per-window compositor, the OS input pump with region arbitration, real DPI, and the
PET_POPUP layer. This is the first time the engine draws an interactive window in production.

## Scope & seams

- **New `src/editor/shell/`** → `context_editor` exe: `WindowManager` / `EditorWindow`
  (native handle + `ISwapchain` + one CEF browser + `WindowCompositor` + input binding);
  links `context_client`, `context_render` present API, CEF — nothing kernel-internal
  (boundary job asserts).
- **Windows backend**: `RegisterClassW/CreateWindowExW` + WndProc (refs
  `spikes/webgpu/main.cpp:506-522`, `cef-compositing/main.cpp:582-599`); per-monitor-v2 DPI
  (CEF `device_scale_factor`, swapchain resize, layout scale — the spike's DPI-1.0 pin is
  replaced with real handling).
- **Message loop**: `multi_threaded_message_loop=false`, integrated pump on the shell main
  thread — `CefDoMessageLoopWork` scheduled by `OnScheduleMessagePumpWork` (design REJECTS
  the spike's multi-threaded+mutex caveat; single-threaded owner loop).
- **CEF host**: windowed-OSR (window as device-context owner); `OnPaint` → CPU-upload path,
  `OnAcceleratedPaint` → e03 import; `OnBeforePopup` suppresses stray popups; DevTools via
  hosted window or remote-debugging port, dev-loop only (B-F11); never
  `SendExternalBeginFrame` (`surface.cpp:39`, cef#4033).
- **Compositor** (03 §4): acquire → viewport layers (rect slots; live content arrives in e11)
  → full-window CEF premultiplied layer (transparent-hole contract) → present; damage-driven
  redraw; resize protocol (reconfigure + `WasResized()` + shared-handle cache clear);
  **`PET_POPUP` composited as a second OSR layer** (required — dropdowns/selects).
- **Input pump** (03 §6): normalize → region arbitration against the window's region map
  (editor-core publishes viewport/native rects) → CEF `SendMouse*/SendKey*Event` or the
  native path; keyboard focus-class rule (DOM editable → CEF, else keymap-first — hook lands
  with e07); reuse `InputRouter`/`UiInputRouter` capture-stack shape
  (`input_routing.h:62-112`); stamp R-HUX-011 timestamps at dispatch.
- **CPU present fallback wired** (e03 seam): no-adapter boot presents software-OSR via GDI.
- Window placement persisted (debounced, crash-safe) to `.editor/editor-state.json` —
  Shell is that file's single writer (03 §1 split).

## Definition of Done

- [ ] Boots a real Windows window loading a placeholder page over the app scheme; input
      round-trip (mouse/keyboard/wheel) works; resize + DPI change handled live
- [ ] **Software (CPU-upload) OSR path works on Windows**; the accelerated branch is present as a
      disabled, flag-switchable seam (see the ⚠️ banner — do NOT gate on a Windows accelerated
      path); PET_POPUP renders
- [ ] CPU present fallback boots the UI with no usable adapter
- [ ] CI: Session-0-safe smoke green (software-OSR, composited-present asserts); full
      windowed pass verified on the interactive Windows box and recorded
- [ ] Boundary job builds `context_editor` against installed artifacts (D10)
- [ ] 3-OS CI green (Windows exec; mac/Linux compile-guarded until e12)
