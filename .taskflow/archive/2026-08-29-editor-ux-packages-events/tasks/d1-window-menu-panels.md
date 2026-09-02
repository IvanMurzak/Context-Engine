---
id: "d1-window-menu-panels"
title: "The Window menu: search field, the path panel tree, and the OS-window subsection (D9)"
group: "D"
sequence: 1
repo: "."
base_branch: "main"
depends_on: ["c2-manifest-v3", "c3-panel-instance-runtime"]
importance: 6
complexity: 6
security_critical: false
production_touching: false
model_hint: "mid"
taskflow_refs: ["01-current-architecture.md", "04-panel-instances-and-menu.md"]
---

## Goal

A closed panel cannot be reopened: the only open command is the hardcoded
`view.panel.open.settings` (`menu.ts:76`), and the **Window** menu — where the owner looked — holds
OS windows (`menu.ts:305`). Implement D9: **`Window` opens things; `Panel` acts on the focused one.**
`Window` becomes, in order: a search field · the panel tree built from manifest `path` (slashes →
nesting) · a separator · the existing OS-window list (`windowListEntries`, `menu.ts:205`). `Panel`
(`menu.ts:290`) keeps tear-out and the move actions unchanged.

## Scope & seams

- **Search** matches **every path segment and the panel name simultaneously** — `dbg tile` finds
  `Scene/Debug → Tilemap Painter`. **Reuse `fuzzyMatch` from `palette.ts:102`; do not write a second
  matcher.** Its scoring already rewards consecutive runs, word starts and an early first match, and
  it returns `{score, positions}` for highlighting the way the palette highlights.
- **Instance rules are visible, not just enforced** (from `c3`): a singleton already open reads as
  "focus"; a `limited` panel at its maximum renders **disabled with the reason in its tooltip** — the
  honest-degrade rule `menu.ts` already applies to its ⏳ rows (`MENU_ITEM_DISABLED_REASON`,
  `CLIPBOARD_DISABLED_REASON`). An item is disabled, never hidden, so the menu's shape cannot flicker.
- **Inherited `menu.ts` constraints that must not break**:
  1. **No second dispatch system.** Every entry names a command id in the one e07b registry — opening
     a panel is a command (`view.panel.open.<panelId>`), registered like any other, so the palette and
     keymap get it for free and the native macOS `NSMenu` path keeps working through `editor.ui.menu`.
  2. **DOM only, no `innerHTML`** — every node via `createElement` + `textContent`, since a
     package-authored panel title reaches this menu.
  3. The dropdown is an app-chrome overlay in the palette's pattern, with ARIA
     `menubar`/`menu`/`menuitem` and full keyboard navigation. **The search field inside a menu is
     new for this codebase**: arrow keys move through results while the field keeps text focus, and
     Escape closes the menu rather than only clearing the field.
- Out of scope: alt-mnemonics (already deferred, recorded in `menu.ts`); any change to the `Panel`
  menu's actions; new panels.

## Definition of Done

- Search tests: a query matching a path segment, one matching the panel name, one matching across
  both (`dbg tile`), and a non-match; result highlighting driven by `{positions}`.
- Keyboard tests for the new search-in-menu pattern: arrows traverse results with text focus
  retained; Enter opens the selected panel; Escape closes the whole menu.
- Instance-mode surfacing tests: open singleton → row reads as focus and focuses; limited at max →
  row disabled with the reason in its tooltip (never hidden); unlimited → opens a new instance.
- Every new row dispatches through a registered command id (assert registry entries; palette can
  invoke the same command).
- No `innerHTML` in the new code; ARIA roles asserted; the OS-window subsection still lists and
  focuses windows as before.
- All `webui-*` menu/chrome tests green; tests in the same PR (R-QA-013). PR body cites D9.
