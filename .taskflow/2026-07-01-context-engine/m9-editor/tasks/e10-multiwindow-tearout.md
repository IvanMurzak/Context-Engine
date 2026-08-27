---
id: e10-multiwindow-tearout
title: Multi-window — PanelHost tear-out/rehome, Shell-mediated cross-window drag, degradation paths (D1/D6)
group: C
sequence: 24
repo: "."
base_branch: "main"
depends_on: [e05-editor-core-foundation, e07-commands-palette-keymap]
importance: 8
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [04, 03, 02]
superseded_by: [e10a-shell-multiwindow-primitive, e10b-tearout-rehome, e10c-crosswindow-drag, e10d-nwindow-persistence-a11y]   # TD decomposition 2026-07-23
---

> # ⛔ SUPERSEDED — DECOMPOSED into [`e10a`](e10a-shell-multiwindow-primitive.md) → [`e10b`](e10b-tearout-rehome.md) → [`e10c`](e10c-crosswindow-drag.md) → [`e10d`](e10d-nwindow-persistence-a11y.md)
>
> **TD decomposition 2026-07-23**, executing the pre-screen verdict recorded on this task's board row
> (2026-07-22). **Never dispatched — zero wasted runs**, like e06c and e08.
>
> Two independent reasons it could not go as one pass: **6 DoD items** spanning native window
> creation, tear-out, cross-window drag, rehome, persistence, degradation and a11y; and it
> **straddles two merge-conflict groups** — net-new native windowing (**B**) vs PanelHost tear-out
> over the D6 state contract (**C**).
>
> Split along the group seam, then by mechanism:
> - **e10a** (B) — the `EditorWindow` primitive: N native windows, one fresh editor-core each,
>   `OnBeforePopup` suppression. Nothing panel-specific.
> - **e10b** (C) — tear-out + rehome by COMMAND over the ONE D6 recreate path, both degradation
>   paths loud.
> - **e10c** (B∩C, hardest) — the Shell-mediated cross-window DRAG session. ⚠ the one slice with no
>   safe parallel partner.
> - **e10d** (C) — N-window persistence, `schemaVersion` guard, keyboard-only path, and the two
>   drills e08/e08c explicitly deferred here.
>
> **e10 is the keystone of the remaining board**: `e09`, `e11` and `e12` are all deep-blocked behind
> it. This file is kept as origin-of-record. **Do not dispatch it.**

## Goal

Deliver the VS Code-grade windowing promise (D1): tear a panel into its own native OS window,
drag panels between windows, rehome on window close — all riding the ONE state-contract
mechanism (D6, serialize → destroy → recreate; no retainContext), with loud degradation paths.

## Scope & seams

- **Tear-out = first-class PanelHost/Shell mechanism** (B-F2 — Dockview popout API stays
  unused): trigger by drag-past-window-bounds OR the tear-out command (keyboard path,
  R-A11Y-001); panel serializes (`getState()`) → Shell creates an `EditorWindow` + fresh
  editor-core instance (03 §1) → Dockview root seeded with the moved panel restores from
  state.
- **Cross-window drag** (04 §2): Shell-mediated drag session — drag leaves window bounds →
  Shell tracks global cursor, renders the drag ghost, targets the window under cursor, asks
  its editor-core for the drop zone over the IPC bridge; drop = rehome. Within one window:
  Dockview native DnD untouched.
- **Rehome** = the universal recreate path — same mechanism serves tear-out, cross-window
  drag, layout restore, crash recovery (D6).
- **Window peers**: all windows equal docking targets; window 0 primary (menu/welcome host);
  layout tree + placements for N windows persist in `.editor/editor-state.json`.
- **Stray popup suppression**: `OnBeforePopup` interception (03 §1) — `window.open` never
  creates an unmanaged window.
- **Degradation (03 §7)**: secondary-window create fails → popout degrades to a floating
  Dockview group in the source window, LOUDLY; window destroyed with panels → panels rehome
  to window 0, never silently lost.
- Viewport panels rehome like any panel (render-target rebinding hooks for e11; a viewport
  panel moved before e11 lands shows its placeholder).
- Commands: "Move panel to new window", "Move panel to window N", dock-zone move commands
  (≤4-key keyboard path — persona A budget).

## Definition of Done

- [ ] Tear-out via drag AND via command produces a second native window with the panel live
      and state preserved (input value, scroll, selection context per state contract)
- [ ] Drag a panel between two open windows; drop zones highlight; state survives
- [ ] Close a window with panels → rehome to window 0; create-fail → floating group + loud
      surface (both T2-asserted)
- [ ] N-window layout + placements persist/restore across restart (T2)
- [ ] schemaVersion mismatch on restore → `null` state + diagnostic, no crash (T1)
- [ ] Keyboard-only tear-out/move path works (a11y check); 3-OS CI green (Windows windowed
      leg per Session-0 honesty; Linux xvfb + macOS T2 legs)
