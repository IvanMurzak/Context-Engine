---
id: e05d-panelhost-hydration-layout
title: editor-core (d) — PanelHost over Dockview, hydration runtime v1, layout persistence, region maps
group: C
sequence: 5
repo: "."
base_branch: "main"
depends_on: [e05a-webui-workspace-toolchain, e05b-manifest-roster-state-contract, e05c-app-scheme-ipc-bridge]
importance: 9
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [04, 02, 03, 05]
split_from: e05-editor-core-foundation   # owner-approved decomposition 2026-07-20
superseded_by: owner-ruling-2026-07-20-e05d-split   # → e05d1–e05d4; see the ⛔ banner below + ROADMAP.md
---

> # ⛔ SUPERSEDED — DECOMPOSED 2026-07-20 (owner ruling). DO NOT IMPLEMENT THIS AS ONE TASK.
>
> Run `1eeb21321ae4` halted at `02-implement` **before writing any production code**, on a real
> **D10 design collision** plus a scope finding. Both are resolved by the owner ruling below.
>
> **① The collision.** This spec's DoD requires **Scene tree + Inspector + Problems** to hydrate
> from the live daemon — but the **D10 shell-boundary gate landed by e04**
> (`context_assert_shell_boundary`) forbids exactly that for two of the three:
> `context_gui_panel_scenetree` → PUBLIC `context_compose`; `context_gui_panel_inspector` → PUBLIC
> `context_compose` + `context_schema`. **Only Problems is Shell-hostable today.**
> **OWNER RULING: split BOTH kernel-typed builders out — do NOT widen the gate's FORBIDDEN list**,
> because that gate is what makes **D18** ("the editor is physically an ordinary client") true
> rather than aspirational. That work is **e05d3**.
>
> **② The scope.** Each sibling PR landed 1.8k–4.3k insertions for a SINGLE seam (e05a
> `552cbd3`=4293, e05b `2e8d2ba`=1821, e05c `7d448c9`=4199). The e05a–e05d split moved the
> *substrate* out but left the **entire headline payload** in this last child — four sibling-sized
> seams plus two new test tiers.
>
> **Implement these instead.** All four are **group C and share `src/editor/webui/`** ⇒ run them
> **SEQUENTIALLY, never in parallel**:
>
> | | Spec | Needs | Why separate |
> |---|---|---|---|
> | **e05d1** | [`e05d1-panelhost-hydration-runtime.md`](e05d1-panelhost-hydration-runtime.md) | e05a/b/c ✅ | PanelHost + hydration v1 (TS); **unblocked today** |
> | **e05d2** | [`e05d2-layout-persistence-region-maps.md`](e05d2-layout-persistence-region-maps.md) | e05d1 | layout persistence + region maps; Shell stays single writer (C-F3) |
> | **e05d3** | [`e05d3-shell-boundary-refactor.md`](e05d3-shell-boundary-refactor.md) | e05d1 | the ① boundary refactor + live scenetree/inspector |
> | **e05d4** | [`e05d4-t2-boot-dock-restore-smoke.md`](e05d4-t2-boot-dock-restore-smoke.md) | e05d1 + e05d2 | T2 boot→dock→restore CEF smoke + `ci.yml --target` wiring |
>
> ⚠ **Only `problems` is Shell-hostable until e05d3 lands.** e05d1 must therefore build the
> hydration runtime **panel-agnostic** and prove it on Problems — *without* special-casing it in a
> way e05d3 must undo.
>
> This file is kept as the **origin of record** for the scope and design references; the four specs
> above are authoritative. Live state: [`../ROADMAP.md`](../ROADMAP.md).
>
> ♻ **Preserved asset:** worktree `.claude/worktrees/1eeb21321ae4` (`outcome=halted`) holds a
> completed `cmake -S src --preset dev` and `src/build/dev/shell-boundary-report.txt` — the ①
> evidence. **Do not destroy it before e05d3 is done with it.**

## Goal

Make the editor usable: PanelHost over Dockview, the hydration runtime binding uitree panels to
live DOM, per-window layout persistence, and region-map publication to the Shell.

## Scope & seams

⚠ **Toolchain-seam trap (generalized from the e05a run — expect to hit it).** Tool paths published
by `src/runtime/ts` are **NOT visible from `src/editor/`**, because `src/editor/` is configured
BEFORE `src/runtime/ts`. This cost e05a real time on `CONTEXT_ESBUILD_BIN`, and **`tsgo` is
strictly worse** — not even `PARENT_SCOPE`-exported. Any such tool path must be re-staged locally
or promoted to `CACHE INTERNAL`; never assume a variable set in that subtree is readable here.

- **PanelHost over Dockview**: **Dockview is geometry ONLY**; PanelHost owns panel lifecycle.
  ⚠ Dockview's popout API is deliberately **UNUSED** (B-F2 — v7 rejects non-http(s) popout URLs,
  proven in s1); OS-window tear-out is a PanelHost/Shell mechanism arriving in **e10**, so build
  the seam without depending on popout.
- **Hydration runtime v1** (04 §4): uitree HTML requested over the e05c bridge → DOM mount;
  focusables follow `focus_order`; activation dispatches bound command ids; gesture verbs
  (begin/extend/commit/cancel) mapped; incremental DOM patches keyed by **stable node ids**; widget
  classes keyed by node role/type (presentation-only).
- **Panel purity (D6)**: every panel is f(bridge state, state blob) — consume e05b's contract; a
  schemaVersion mismatch degrades to `null` + diagnostic, never a broken panel.
- **Layout persistence**: Dockview `toJSON()` per window + placements → `.editor/editor-state.json`
  (debounced + on-exit + crash-restore). ⚠ The **Shell is that file's single writer** (03 §1) —
  editor-core publishes state to the Shell, it does NOT write the file.
- **Region maps**: publish viewport/native rects to the Shell on every layout change — this is the
  input-arbitration feed e04's pump consumes (03 §6).

## Definition of Done

- [ ] App boots inside the e04 shell window from the app scheme under strict CSP
- [ ] Scene tree + Inspector + Problems hydrate from the **LIVE daemon** via the bridge (read path);
      interactions dispatch commands to the C++ models
- [ ] Panels dock / split / tab / float; layout + panel state persist and restore across restart
- [ ] Region maps published on layout change and consumed by the Shell's input arbitration
- [ ] T1: state-contract round-trips + schema-mismatch handling. T2: boot → dock → restore smoke
- [ ] Boundary discipline: editor-core deps = the s1-approved set ONLY (`dockview-core@7.0.2`)
- [ ] 3-OS CI green
