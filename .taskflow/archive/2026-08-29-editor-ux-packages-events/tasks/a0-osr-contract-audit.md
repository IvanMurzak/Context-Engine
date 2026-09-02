---
id: "a0-osr-contract-audit"
title: "Audit the OSR contract as a list and land the conformance table in docs/shell.md"
group: "A"
sequence: 0
repo: "."
base_branch: "main"
depends_on: []
importance: 7
complexity: 5
security_critical: false
production_touching: false
model_hint: "mid"
taskflow_refs: ["01-current-architecture.md", "02-target-architecture.md", "03-osr-geometry-and-drag.md"]
---

## Goal

Implement D12: walk the CEF windowless (OSR) contract **as a list** and land a conformance table in
`docs/shell.md`, so the remaining gaps stop surfacing one at a time. The evidence that piecemeal
adoption fails is this set itself — three of the owner's seven reported items (dead drag, misplaced
dropdowns, offset context menu) trace to unimplemented members that were invisible because nobody had
ever enumerated the contract.

This task ships a **table, not code**. Baseline truth (verified 2026-08-29): `ShellCefClient`
(`src/editor/shell/cef/src/cef_shell.cpp:653`) implements **5 of `CefRenderHandler`'s 17 members**,
zero of the windowless-only `CefBrowserHost` drag family, and no `CefContextMenuHandler`.

## Scope & seams

- **One new section in `docs/shell.md`**, adjacent to the existing manual-verification tables.
- Enumerate three surfaces from the **pinned SDK headers** (the CEF distribution fetched by the build,
  e.g. `src/build/editor/_cef/<triple>/include/cef_render_handler.h`; cite the pinned CEF version in
  the section header):
  1. every `CefRenderHandler` member (17);
  2. every windowless-only `CefBrowserHost` method — the `DragTarget*` family (`cef_browser.h:889-927`),
     `DragSourceEndedAt` / `DragSourceSystemDragEnded` (`:930-946`), `SendMouse*`, `SendKeyEvent`,
     `SendTouchEvent`, `SetFocus`, `WasResized`, `WasHidden`, `NotifyScreenInfoChanged`,
     `NotifyMoveOrResizeStarted`, `SetWindowlessFrameRate`, `ImeSetComposition` and siblings;
  3. `CefContextMenuHandler`.
- Each row records **exactly one** of:
  - **implemented** — with the `file:line`;
  - **deliberately not needed** — with the reason and, where one exists, the ruling
    (`OnAcceleratedPaint` is the model: owner ruling 2026-07-19, rationale in `cef_shell.h`);
  - **gap** — with the user-visible consequence and a task id from this set (`a1`, `a2`, `b1`) or a
    follow-up issue id.
- Gaps expected to be registered outside this set's scope (file a GitHub issue for each, or point at
  the set's recorded open items): `GetAccessibilityHandler` (an OS screen reader sees nothing in this
  window despite green `gui-a11y-*` gates — sharp for a repo with R-A11Y-001), the IME family
  (`OnImeCompositionRangeChanged`, `OnVirtualKeyboardRequested`), `OnTextSelectionChanged`,
  `NotifyMoveOrResizeStarted` (popups not dismissed on window move).
- Out of scope: any code change; any promise about when a registered gap gets fixed.

## Definition of Done

- Every member of the three surfaces appears exactly once in the table with one of the three verdicts;
  no member of `CefRenderHandler` is missing (count must be 17).
- `implemented` rows carry a `file:line` that resolves in the tree; `gap` rows carry a consequence and
  a task/issue id; `deliberately not needed` rows carry a reason.
- The section names the pinned CEF version it was audited against.
- Docs-only diff; the full CI rollup is still green (docs changes run the same required checks).
- PR body cites D12 and this set.
