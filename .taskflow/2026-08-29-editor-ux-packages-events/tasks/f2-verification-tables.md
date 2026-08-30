---
id: "f2-verification-tables"
title: "Reconcile docs/shell.md's manual verification tables with what the set automated"
group: "F"
sequence: 2
repo: "."
base_branch: "main"
depends_on: ["a0-osr-contract-audit", "b1-osr-html5-drag"]
importance: 5
complexity: 5
security_critical: false
production_touching: false
model_hint: "mid"
taskflow_refs: ["03-osr-geometry-and-drag.md", "ROADMAP.md"]
---

## Goal

Close the set by making `docs/shell.md`'s manual verification tables true again: several rows the
tables carry as manual-and-unverified are now automated (`a1`, `a2`, `b1`), and the drag work added
new per-OS claims that CI can only partially reach. Update the tables so every remaining "manual" row
states what CI *does* pin beside it — the section's own rule that "manual" never reads as
"unverified".

## Scope & seams

- **`docs/shell.md` §10/§11 (the manual verification tables)** — docs-only diff:
  - the `PET_POPUP` row: the popup composite is now pinned by `a2`'s scaled
    `editor-shell-test_compositor` cases; the row moves from manual-unverified to automated (or to a
    narrowed manual remainder, if any interactive slice genuinely stays).
  - the context-menu/screen-point rows: `a1`'s `editor-shell-test_dpi` cases pin the conversion; the
    live half rides the Windows/Linux CEF smokes — state both.
  - the drag rows from `b1`, per OS: Linux is automated end to end (the X11 gesture leg); Windows is
    Session-0 (logic-pinned + manual gesture row); macOS is approximate by construction
    (direction/separation assertions) — each row names its CI half.
  - any row `a3` left as a documented manual check (rendered tab-strip hover), if one exists.
- **Consistency with `a0`'s conformance table**: rows that a0 marked gap-with-task and that
  `a1`/`a2`/`b1` closed are flipped to implemented with their `file:line`; gaps registered for
  follow-up (accessibility, IME, `OnTextSelectionChanged`, `NotifyMoveOrResizeStarted`) stay marked
  as gaps with their issue ids.
- Out of scope: any code or test change; the chrome visual-regression harness (deferred to `e16` by
  `docs/shell.md` itself, not taken by this set).

## Definition of Done

- Every manual row that the set automated is updated to name its automating test; every remaining
  manual row states what CI pins beside it, in the section's existing format.
- The conformance table (`a0`) and the verification tables agree — no member is "gap" in one and
  "verified" in the other.
- Docs-only diff; full rollup green; PR body cites the set and lists the rows that changed state.
