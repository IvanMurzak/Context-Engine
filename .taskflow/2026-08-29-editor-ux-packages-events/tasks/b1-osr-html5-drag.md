---
id: "b1-osr-html5-drag"
title: "Implement HTML5 drag-and-drop in OSR on Win32, X11, and Cocoa (D11)"
group: "B"
sequence: 1
repo: "."
base_branch: "main"
depends_on: ["a0-osr-contract-audit", "a1-osr-screen-point", "a2-osr-popup-dpi"]
importance: 9
complexity: 10
security_critical: false
production_touching: false
model_hint: "top"
taskflow_refs: ["01-current-architecture.md", "02-target-architecture.md", "03-osr-geometry-and-drag.md"]
---

## Goal

**All HTML5 drag-and-drop in the editor is dead** — `CefRenderHandler::StartDragging` is not
overridden and its default returns `false`, which the header defines as *"abort the drag
operation"*: every drag is actively refused. Implement the OSR drag protocol on all three OSes in one
task (D11). Once the events arrive, **Dockview supplies tab drag, drop-to-split into any edge, and
re-docking with no editor-core change** — verified in the pinned `dockview-core@7.0.2` bundle
(12 `draggable` / 1 `dragstart` / 3 `dragover` / 5 `dragend` / 11 `dataTransfer` / 3 `setDragImage`;
floating groups on, `panelhost.ts:1034-1041`).

## Scope & seams

- **Render handler**: `StartDragging` + `UpdateDragCursor` on `ShellCefClient`.
- **Injections**: `DragTargetDragEnter` / `DragTargetDragOver` / `DragTargetDragLeave` /
  `DragTargetDrop` (`cef_browser.h:889-927`) and `DragSourceEndedAt` / `DragSourceSystemDragEnded`
  (`:930-946`), driven from each platform's OS drag manager:
  | OS | Mechanism |
  |---|---|
  | Win32 | OLE — `IDropSource` / `IDropTarget` / `DoDragDrop`, `RegisterDragDrop` on the window |
  | X11 | XDND |
  | Cocoa | `NSDraggingSession` / `NSDraggingDestination` |
- **Prior art**: CEF's `cefclient` OSR drag implementations
  (`tests/cefclient/browser/osr_dragdrop_win.cc`, `osr_dragdrop_x11.cc`) — ⚠ **not vendored** in our
  minimal distribution; read them upstream as public prior art, port rather than research.
- **Coordinate duty (why `a1` is a hard dependency)**: `StartDragging`'s `(x, y)` is documented in
  **screen** coordinates while `DragTargetDragOver`/`DragTargetDrop` take a `CefMouseEvent` in
  **view** coordinates — run `a1`'s `dpi.h` conversion in both directions; `a2` settled where the DPI
  scale enters the window.
- **No `panelhost.ts` change for tab drag.** If drag still fails after this task, the fault is here —
  do not add editor-core workarounds.
- **Must not regress**: the Shell-mediated **cross-window** drag (`cross_window_drag.h`, `drag.ts`,
  ctest `editor-cef-smoke-shell-drag`). The two mechanisms are layered, not alternatives —
  `drag.ts:19-21` states the split and it stays true.
- Out of scope: OS-file drops into the editor beyond what the protocol port naturally provides; IME;
  accessibility (registered by `a0`).

## Definition of Done

- **Linux: a genuine end-to-end proof.** The `editor-cef-smoke-shell` Linux leg (e12a-x11-legs
  infrastructure) injects a real pointer move/press/release through the X server into live CEF; a
  drag-and-drop asserted there end to end (e.g. a Dockview tab drop that re-docks).
- **Windows: Session-0 honesty.** CI cannot drive the gesture — pure-logic tests for the OLE
  glue plus a row in `docs/shell.md`'s manual table.
- **macOS: approximate by construction.** In-process `postEvent:` injection; assert direction and
  separation, not equality (the documented `translate_ns_event` constraint, `docs/shell.md` §11).
- Every OS-specific claim CI cannot reach lands in `docs/shell.md`'s manual table **with what CI does
  pin beside it**, in that section's existing format — "manual" never reads as "unverified".
- `editor-cef-smoke-shell-drag` (cross-window drag) still green on every leg.
- New ctests obey **Not Run = RED**: any test in the `editor-cef-smoke` family needs its executable in
  that job's `--target` list AND the `ctest -R` match in `ci.yml`.
- All three OS legs green in one PR (D11) — a fault on any leg blocks the task; there is **no local
  compile signal**, so budget for CI round-trips.
- PR body cites D11/D12 and the `a0` conformance rows this closes.
