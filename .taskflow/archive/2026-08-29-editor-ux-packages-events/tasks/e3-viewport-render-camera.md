---
id: "e3-viewport-render-camera"
title: "The Scene viewport: producer, DOM hole, RegionMap rect, and camera through editor.camera-set (D7)"
group: "E"
sequence: 3
repo: "."
base_branch: "main"
depends_on: ["a2-osr-popup-dpi", "c3-panel-instance-runtime"]
importance: 8
complexity: 9
security_critical: false
production_touching: false
model_hint: "top"
taskflow_refs: ["01-current-architecture.md", "02-target-architecture.md", "06-viewport-and-files.md"]
---

## Goal

There is no window showing the scene: `builtin.viewport` is rostered but not hostable
(`hostable_panel_ids()` omits it, `builtin_panels.cpp:555-577`), and its model is a text summary.
Build the live viewport (D7, minus picking which is `e4`). **The compositor and input halves already
exist and are tested — do not rebuild them**: `ViewportLayer` (`compositor.h:85`, content_rect in
PHYSICAL px), `publish_viewports` (`compositor.cpp:341`) + the composite loop (`:494`, drawn beneath
the CEF layer through its transparent holes), and `RegionMap` with `RegionKind::viewport`
(`input.h:64-92`). Nothing in production calls `publish_viewports` today — only
`test_compositor.cpp:240,331,371` — and `input.h:88-91` names this as the seam. The missing work is
the **producer, the hole, the rect report, and the camera** — not the composite, not the routing.

## Scope & seams

1. **The producer**: render the scene into an `ITextureView` sized to the panel's rect and call
   `publish_viewports`; damage via `mark_viewport_content()`, which exists for exactly this.
2. **The hole**: the panel's DOM element is transparent so the layer beneath shows through — this
   **pins its Dockview renderer to `"always"`** (`rendererFor` in `panelhost.ts` carries the
   reasoning; an `onlyWhenVisible` panel is detached from the DOM and its rect is meaningless).
3. **The rect**: the panel's live rect reported into `RegionMap` on every layout change (Dockview's
   `onDidLayoutChange`) **in physical pixels** — that is what `ShellRegion` documents and the OS
   reports. ⚠ Dockview reports CSS pixels (DIP), so a conversion sits between them — **take the DPI
   scale from wherever `a2` put it; never open a second source**. This is the same bug class `a2`
   fixed; the vacuity gate applies here identically.
4. **Camera**: through the existing `editor.camera-set` / `editor.cameras-get` — no new contract
   surface; the verbs carry `transform`/`projection` **opaquely**, so the viewport owns their
   meaning; persisted where cameras already persist.
5. **Hosting**: the four anchors + `hostable_panel_ids()` for `builtin.viewport`; under manifest v3
   it is `instances.mode: "unlimited"` (multiple scene views is the point) — **the first shipping
   proof of `c3`'s instance runtime**. The existing summary model is not thrown away: it becomes the
   honest degraded content when no adapter is available (the reserved `viewport.adapter_absent`
   code).

- Out of scope: picking (`e4`); a GPU picking path (later work, out of the set); golden-image
  rendering assertions (the render legs own image correctness); any new render backend.

## Definition of Done

- **Geometry tests at device scale ≠ 1** (the set's named gate, shared with `a2`): the DIP→physical
  conversion for the hole/layer/region rects asserted at ≥1.5 alongside the 1.0 identity cases.
- Headless seam tests on all three legs (the `test_compositor` pattern, stub textures): the producer
  publishes a layer whose `content_rect` matches the reported panel rect; a layout change updates the
  `RegionMap` entry; a pointer inside the viewport rect routes to the viewport region, one outside
  routes to CEF.
- Renderer pinned to `"always"` (test that the viewport panel is never given `onlyWhenVisible`).
- Camera round-trip: `camera-set` → `cameras-get` → persisted and restored where cameras persist.
- All four anchors + `hostable_panel_ids()`; `gui-a11y-coverage`, `gui-help-contextual`,
  `m85-exit-4c`, `editor-shell-test_builtin_panels` green.
- Instance proof: **two viewport instances open simultaneously** with independent rects and cameras
  (`c3`'s unlimited mode exercised on a real panel).
- Degraded content: with no adapter, the summary model renders and `viewport.adapter_absent` is
  reported (sibling proving the degrade path is reachable).
- Tests in the same PR (R-QA-013); PR body cites D7 and the `a2` seam choice it consumed.
