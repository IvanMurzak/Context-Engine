---
id: e11-viewports-picking-gizmos
title: Viewports — Camera/View render abstraction, N viewports (Scene|Game), picking, gizmos, camera controls (D5)
group: B
sequence: 5
repo: "."
base_branch: "main"
depends_on: [e03-present-texture-import, e04-window-shell-windows, e08-session-state-ui-bus]
importance: 9
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [03, 01, 05]
---

## Goal

Make the editor a scene editor: N simultaneous interactive viewports (Scene = editor camera,
Game = runtime camera) in any window, with click-selection, translate/rotate/scale gizmos
driving the existing tested gesture verbs, and editor camera controls — camera and selection
truth living daemon-side so agents share it.

## Scope & seams

- **Render additions** (`render_world.h:24-137` — no Camera exists today): `Camera`/`View`
  abstraction — per-viewport `{camera transform, projection, mode 2D/3D, type Scene|Game,
  viewport id}`; extract fills per-view visible sets via the shared spatial index; per-view
  persistent RTs via the `DynamicTextureRegistry` pattern (`dynamic_texture.h:30-63`);
  replaces the hardcoded `view_proj` single-view path for editor use.
- **Compositor binding**: viewport panel instances bind RTs into e04's compositor layer pass
  (letterboxed per config; transparent-hole contract with editor-core); N viewports bounded
  by GPU memory (D5).
- **Scene viewport**: editor camera (daemon session state via `editor camera set` — e08);
  edit-time overlays: grid, selection outline, gizmos (viewport palette tokens from e06 when
  present). **Game viewport**: runtime camera of the play session; NO edit overlays;
  play-mode indicator respected (L-51).
- **Camera controls** (Shell input layer): orbit/pan/zoom/fly writing camera state through
  the bridge — agents can observe AND set the camera (persona C).
- **Picking**: pointer → ray through viewport camera → spatial-index query (generalize the
  `PanelMeshRaycaster` broad-phase-pruned pattern); 2D = point/AABB; result → `editor select`
  (e08) → daemon selection truth → all panels/windows/agents update. Pixel-perfect ID buffer
  = optimization slot, NOT v1-required.
- **Gizmos**: native-rendered overlay pass in Scene view driving the EXISTING gesture verbs
  (`viewport_edit_model.h:117-144` begin/translate/commit) — logic layer already built and
  tested; commits route through the e09 wire write path (CAS + rebase-or-drop).
- **GPU-less host**: viewport panels render the diagnostic placeholder (02 §6) — the app
  never requires a GPU.
- R-HUX-011 timestamps stamped through pick/gesture dispatch (03 §6).

## Definition of Done

- [ ] Two simultaneous viewports (Scene + Game) live in one window + one more in a second
      window, rendering a real project scene; engine rate decoupled from CEF rate
- [ ] Click-select in a viewport updates daemon selection; scene tree/inspector/second
      window/CLI observer all converge (T2)
- [ ] Gizmo translate/rotate/scale commits through wire CAS; concurrent-edit drop is loud
- [ ] Camera controls work; `editor cameras get` reflects them; an agent can set the camera
      and the viewport follows (T2 contract-parity assert)
- [ ] Game viewport shows play session with L-51 indicator; no edit overlays
- [ ] GPU-less placeholder path asserted; 3-OS CI green (windowed legs per 09 §3 honesty)
