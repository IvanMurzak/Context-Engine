# `src/editor/gui/viewport/` — M5-F1 native viewport observer panel

The **read-only observer** viewport panel (issue **#164**; `R-EDIT-001` / `R-UI-007` / **`L-41`** /
`R-REND-002` / `R-HEAD-002` / `R-A11Y-001` / `R-HUX-011`): it renders the live scene **(3D + 2D)** over
`context_render(_wgpu)`, composited into the CEF host through the **L-41 accelerated-OSR shared-texture
seam** (`context_gui_compositor`), and proves **golden-scene equivalence** vs a web WebGPU build
**within the M5 T1 feature set**.

## What this is (and is not)

- **Observer only.** The panel shows the composited scene; it does **not** write the world. In-viewport
  scene-instance / override editing is `R-HUX-006` → **M8.5**, explicitly out of M5. The only affordance
  is *frame scene* (re-frame the observer camera — a view op, not an edit).
- **Headless + CEF-free.** Like the sibling panels, `context_gui_viewport` is pure C++ over the F0b
  headless libs (`context_gui_uitree` + `context_gui_compositor`) and the render RHI abstraction
  (`context_render`). It builds + unit-tests on the **default 3-OS matrix** and the local dev gate; it
  never boots CEF and never links the CI-only GPU backend.

## The two halves

1. **The observer panel (this directory).** `viewport_model.*` summarizes the extracted render snapshot
   (the live scene's drawables + lights, `render::RenderSnapshot`) and computes the **L-41 present
   outcome** — which per-platform CEF compositing surface the frame is handed to CEF through, and the
   failure classes a present can hit. `viewport_panel.*` projects that into a headless `uitree::Panel`:
   a labelled render surface (the a11y analog of an `<img>` `alt`), a status line carrying the L-41
   mode + scene summary + present outcome, and the keyboard-reachable **`viewport.frame-scene`** command
   that drives the **R-HUX-011 gesture→viewport-update loop** (the third R-HUX-011 loop, alongside the
   scene-tree selection loop + the inspector commit loop). The real input→paint latency **timestamp** is
   captured at the CEF host around this seam per `R-EDIT-001`/`R-HUX-011` ("instrumented timestamps in
   the real event path"); the headless panel ships the loop seam.

2. **The render composite + golden gate (`src/render/viewport_scene.h` + `goldens/viewport.ppm`).** The
   pixels: the T1 3D triangle (`triangle3d`) base with the T1 2D sprites (`sprite2d`) painted on top in
   one frame, rendered through **any `rhi.h` device** (the CI wgpu-native backend, or the GPU-free fake
   test backend). The `viewport` golden gates it in the CI `render` job (Linux/lavapipe) via
   `tools/golden_compare.py`. It is **native-only** (like `lit3d`); web T1 equivalence is gated by the
   `triangle3d` + `sprite2d` browser goldens, whose exact primitives the composite reuses — so
   equivalence **within the T1 feature set** follows.

## Reserved `viewport.*` error-domain block (this task mints it)

The wave's single code-minter (`src/editor/contract/src/error_catalog.cpp`, append-only tail). The
strings are owned here as constants (`viewport_model.h`, the promote-a-local-string pattern) and
registered in the catalog:

| code | class | when |
|---|---|---|
| `viewport.adapter_absent` | unimplemented | no GPU adapter to render the observer viewport (R-HEAD-002 — absence is reported, never a fabricated frame) |
| `viewport.surface_unavailable` | internal | the L-41 compositing surface could not be acquired (e.g. macOS IOSurface with no GPU compositor — no software fallback there) |
| `viewport.render_failed` | internal | the scene render / pixel readback failed (R-REND-002) |

## a11y

Registered with the M5-F6 harness (`a11y/registry.cpp` + `a11y/coverage.manifest.jsonl`, id
`builtin.viewport`) so it is scanned on the default 3-OS matrix and the `editor-cef-smoke` Linux
enforcement gate. `tests/test_a11y.cpp` additionally asserts zero violations + a complete keyboard path
across every present state.

## Tests (R-QA-013, same PR)

- `tests/test_viewport_model.cpp` — the snapshot summary + the L-41 present outcome across all three
  reserved codes and the ratified per-platform modes.
- `tests/test_viewport_panel.cpp` — the headless projection, the L-41 seam reaching the panel, the
  R-HUX-011 loop event firing, and observer-only command discipline.
- `tests/test_a11y.cpp` — the per-panel a11y + keyboard-nav gate across present states.
