---
id: e05d2-layout-persistence-region-maps
title: editor-core (d2) — layout persistence + region maps end-to-end
group: C
sequence: 7
repo: "."
base_branch: "main"
depends_on: [e05d1-panelhost-hydration-runtime]
importance: 8
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [04, 03, 02]
split_from: e05d-panelhost-hydration-layout   # owner ruling 2026-07-20
---

> **Split from [`e05d-panelhost-hydration-layout.md`](e05d-panelhost-hydration-layout.md)** (owner
> ruling 2026-07-20). Second of e05d1–e05d4. Depends on **e05d1** for PanelHost + the panel
> lifecycle whose state this persists.

## Goal

Make the editor's arrangement durable and make the Shell aware of it: per-window layout
persistence (dock arrangement + panel state, across restart) and **region-map publication** — the
viewport/native rect feed the Shell's input arbitration consumes.

## Scope & seams

⚠ **The Shell is the SINGLE WRITER of `.editor/editor-state.json`** (C-F3, design 03 §1).
editor-core **publishes state over the e05c bridge**; it does **not** open, write, or lock that
file. This is not a style preference — it is the ownership split that keeps the editor an ordinary
wire client (D18). A direct write from editor-core is a defect even if it works.

- **Layout persistence**: Dockview `toJSON()` per window + panel placements + each panel's D6 state
  blob → published to the Shell → `.editor/editor-state.json`. Three write triggers, all required:
  **debounced** during interaction, **on-exit**, and **crash-restore** (the last-known-good snapshot
  must survive a non-graceful exit).
- **Restore path**: on boot, the Shell supplies the persisted blob; PanelHost rebuilds the
  arrangement and each panel restores from its own versioned blob. A schemaVersion mismatch on ANY
  panel degrades that panel to `null` + diagnostic (e05b's D6 contract) — it must **not** discard
  the whole layout.
- **Region maps** (03 §6): publish viewport/native rects to the Shell on **every** layout change —
  dock, split, tab, float, resize, panel add/remove. This is the input-arbitration feed e04's pump
  already consumes; a stale or missing region map shows up as input landing on the wrong surface.
- Out of scope: the boundary refactor + live scenetree/inspector (**e05d3**), the T2 CEF smoke
  (**e05d4**).

## Standing lessons (carry forward — earned by the siblings)

1. **A spec's ripple list is a starting point, never the whole set** (e05b). Enumerate the real
   consumers of the state file and the region-map feed from the code before trusting this list.
2. **Read CI before reviewing** (e05c) — `03-refine` reading `gh pr checks` on a normal entry is
   what caught a deterministic break that every local signal missed.
3. **A passing sibling test only exonerates a suspected flake if that leg actually runs the
   affected code** — check link-graph disjointness before spending rerun budget.
4. Known flakes: CE [#319](https://github.com/IvanMurzak/Context-Engine/issues/319) and CE
   [#322](https://github.com/IvanMurzak/Context-Engine/issues/322). ⚠ This task touches the
   editor/Shell surface — if your diff is in the failing test's link closure, treat it as **REAL**.

## Definition of Done

- [ ] Dock arrangement + per-panel D6 state persist and **restore across a restart**
- [ ] All three write triggers covered and tested: debounced-during-interaction, on-exit,
      crash-restore (last-known-good survives a non-graceful exit)
- [ ] **Shell is the sole writer of `.editor/editor-state.json`** — asserted structurally, not by
      comment: editor-core contains no path to that file
- [ ] A schemaVersion mismatch on one panel degrades that panel only; the rest of the layout restores
- [ ] Region maps published on **every** layout change and consumed by the Shell's input arbitration
- [ ] T1: layout round-trip, per-trigger persistence, mismatch-degradation, region-map emission
- [ ] `context_assert_shell_boundary` still passes non-vacuously; FORBIDDEN list untouched
- [ ] 3-OS CI green
