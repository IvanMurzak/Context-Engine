---
id: e08b-panel-state-rewiring
title: editor (08b) — rewire scene tree + playbar + when-context off private local state onto daemon session state
group: A
sequence: 6
repo: "."
base_branch: "main"
depends_on: [e08a-daemon-session-state]
importance: 8
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [05, 01]
split_from: e08-session-state-ui-bus   # TD decomposition 2026-07-22
---

> **Split from [`e08-session-state-ui-bus.md`](e08-session-state-ui-bus.md)** (decomposition 2026-07-22).
> Second of the group-A chain **e08a → e08b**. e08a made daemon session state authoritative; this task
> makes the existing GUI actually *use* it, deleting the private local copies.
>
> ⚠ **Co-scheduling constraint** (merge-conflict domains): this task is group **A** (C++ panels under
> `src/editor/gui/`) but it also edits **one group-C file**, `src/editor/webui/core/src/when.ts`
> (e07's when-context providers). It may run concurrently with group-C work *only* while that work
> does not touch `when.ts` — true for e06b/e06c/e06d (theme/components/settings), **not** assumed for
> e10/e13. The orchestrator serializes if a group-C task lands in `when.ts`.

## Goal

Make the daemon the single source of truth for selection / camera / play in the live UI: the scene
tree, the playbar, and e07's when-context providers become **subscribers and writers** of e08a's
session state instead of owning private in-process copies.

## Scope & seams

- **Scene tree**: the local selection seams (`scene_tree_panel.h:62-68`) become a subscriber of
  `session` `selection-changed` + a writer via `editor select`. The panel no longer *owns* selection;
  it renders it. Round-trip through the daemon must not flicker or double-apply — use e08a's `origin`
  echo suppression.
- **Playbar**: `playbar_model.h:83,102-117` today drives an **in-process `SessionControl*`**. It
  becomes an RPC writer (`editor play|pause|stop|step`) + a `play-state` subscriber. The in-process
  path is REMOVED, not left as a parallel truth.
- **e07 when-context providers**: `src/editor/webui/core/src/when.ts` currently feeds the command
  palette / keymap `when` clauses from **local stubs** (`editorFocus`, selection-ish predicates).
  Switch them to the real session state over the bridge. e07's existing when-eval tests must keep
  passing — the evaluation semantics do not change, only the source.
- **No private ownership left behind**: assert structurally (a test, not a comment) that the GUI
  panels no longer hold an authoritative selection/play copy — a second client's change is visible in
  the panel without any panel-local write.
- Out of scope: the `editor.ui` bus (e08c), viewports/cameras UI (e11), writes/undo (e09).

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set — grep for every reader of the
   private seams before declaring the rewire complete.
2. Read CI before reviewing, even on a NORMAL entry.
3. A passing sibling only exonerates a suspected flake if that leg registers + runs the affected code.
4. Known flakes CE #319 / #322 / #335 + the self-hosted-Windows CEF env flake family.

## Definition of Done

- [ ] Scene-tree selection is daemon-backed: a selection made by a **second client** (CLI/agent) is
      reflected in the panel, and a panel selection is observed by that client — no echo loop
- [ ] Playbar drives daemon play state over RPC; the in-process `SessionControl*` path is **removed**
      (no parallel truth); L-51 indicator fed from the daemon state
- [ ] e07 when-context providers read the real session state (local stubs gone); the existing e07
      when-eval + palette tests still pass unchanged
- [ ] A structural test asserts no GUI panel holds authoritative selection/play state
- [ ] `context_assert_shell_boundary` passes non-vacuously; FORBIDDEN list untouched
- [ ] Tests same PR (R-QA-013); 3-OS CI green
