---
id: "e1-files-panel-read"
title: "The `editor files` daemon read verb, the Files panel, and subject:\"file\" selection"
group: "E"
sequence: 1
repo: "."
base_branch: "main"
depends_on: ["c1-selection-subjects", "c3-panel-instance-runtime"]
importance: 7
complexity: 6
security_critical: false
production_touching: false
model_hint: "mid"
taskflow_refs: ["01-current-architecture.md", "06-viewport-and-files.md"]
---

## Goal

There is no file-browser panel. Build the read half of D10: a new daemon read verb `editor files`
and a Files panel that publishes `subject:"file"` selections and consumes `selection-focus`. This
alone closes the owner's four-panel scenario (Files · Scene · Hierarchy · Inspector reacting to one
another) without entering the write path.

## Scope & seams

- **The D10 boundary dictates the shape**: `context_assert_shell_boundary` (the `editor-boundary`
  job) FATAL_ERRORs at configure time on any EditorKernel internal in the Shell's link closure — the
  Shell **cannot link `assetdb`**. So the verb follows `registry.cpp:939` `editor scene-tree` and
  `:949` `editor inspect` exactly: the kernel-typed model builder runs **daemon-side** (over
  `src/editor/assetdb/`'s bounded path/guid/kind index and `src/editor/filesync/`), the model arrives
  at the Shell as data, and the boundary gate's forbidden list never moves.
- **`editor files`** (operational, `read_query` scope): the project's file tree as a boundary-clean
  panel model — path, guid, kind, and the row identity the panel keys by. Registered in the one
  contract registry → `describe` parity for free (R-CLI-013).
- **The panel** (C++-modelled, `uitree`): rows publish selection through `editor.select` with
  `subject: "file"`; the panel highlights only the `file` selection and consumes `selection-focus`.
- **The four-anchor rule + the fifth edit** — adding a built-in panel is guarded by two *different*
  ctests:
  1. roster entry (`builtin_roster.cpp`) — with v3 `instances` + `path` (from `c2`);
  2. headless a11y factory (`gui/a11y/registry.cpp`) + linking its library into `context_gui_a11y`;
  3. the `coverage.manifest.jsonl` line — anchors 1–3 guarded by **`gui-a11y-coverage`**;
  4. `help::panel_topics()` (`gui/help/src/help_model.cpp`) — guarded by **`gui-help-contextual`**
     and the `m85-exit-4c` gate;
  5. `hostable_panel_ids()` (`builtin_panels.cpp:555`), whose lockstep with the bindings is asserted
     by `editor-shell-test_builtin_panels`.
- **Read-only**: no write operation of any kind — rename/move/delete are `e2`. No `file_write` grant
  appears in this task's manifest surface.
- If a new library joins the exported install set, it joins the `editor-boundary` job's `--target`
  list in the same PR (a target never built fails `cmake --install`).
- Out of scope: any file mutation (`e2`); thumbnails/previews; watching beyond what `filesync`
  already provides.

## Definition of Done

- `editor files` returns the tree for a fixture project (paths, guids, kinds asserted); `describe`
  lists it; the CLI projection works (one registry, no bespoke wiring).
- The panel is hosted: all four anchors + `hostable_panel_ids()`; **both** guarding ctests green
  (`gui-a11y-coverage`, `gui-help-contextual`), plus `editor-shell-test_builtin_panels` and
  `m85-exit-4c`.
- Selection integration: clicking a row calls `editor.select subject:"file"`; the resulting fact does
  **not** move the scene tree (c1's filter — asserted here as an integration case) and the Files
  panel highlights the row; entity and file selections coexist (D1).
- `selection-focus` consumption: focus lands on `file` when the user selects there; the panel renders
  focus state.
- The `editor-boundary` job green — the Shell links no EditorKernel internal.
- Tests in the same PR (R-QA-013); PR body cites D10 (read half) and D1/D3.
