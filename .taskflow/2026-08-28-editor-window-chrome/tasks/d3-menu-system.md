---
id: "d3-menu-system"
title: "Full menu system: one declarative model, web menubar (Win/Linux) + native NSMenu (macOS)"
group: "D"
sequence: 3
repo: "."
base_branch: "main"
depends_on: ["a1-chrome-contract", "a2-strips-scaffold", "c1-macos-hybrid"]
importance: 9
complexity: 8
security_critical: false
production_touching: false
model_hint: "top"
taskflow_refs: ["03-menu-structure.md", "02-target-architecture.md", "01-current-architecture.md"]
---

## Goal

The D1 full menu: ONE declarative menu model in editor-core, every item backed by a command id in
the e07b registry, rendered two ways — a web menubar inside the titlebar strip on Windows/Linux,
and the native global `NSMenu` bar on macOS fed from the same model over a new `menu.publish`
bridge method. No second dispatch system exists.

## Scope & seams

- **The tree is exactly 03's table** — App (macOS) / File / Edit / View / Selection / Panel /
  Window / Help, with the per-item backing column as written. Items whose backing is future work
  render DISABLED with the reason in their tooltip (the ⏳ rows: Cut/Copy/Paste outside
  `textInputFocus`). Play/pause/stop deliberately have NO menu.
- **Web rendering (Win/Linux)**: menubar in the titlebar strip (a2's `system`-mode strip IS this
  menubar on Linux); dropdowns are app-chrome overlays in the palette's pattern
  (`palette_view.ts:77-117`, z-order stack `app.css:442-466`); ARIA
  `menubar`/`menu`/`menuitem` with full keyboard nav — arrows/Enter/Escape/Home/End;
  Alt-mnemonics deferred and recorded. No new kit family.
- **macOS**: `menu.publish` (renderer → Shell) publishes the model; the Cocoa backend builds the
  global `NSMenu` from it; an activated item comes back as a fact on the EXISTING `editor.ui`
  relay (`window_bridge.h:87-102`) carrying the command id, which editor-core executes through
  the same registry as a web-menubar click. NSMenu key equivalents ride the same return path. No
  web menubar renders on macOS (02 §4).
- **New commands built here**: `project.new` (e14c welcome flow — `WelcomeBridge`, `welcome.cpp`),
  `project.open` (`welcome.pickFolder` + `welcome.open`), Open Recent from `welcome.state`
  (`welcome.cpp:293-310`) / config (`user_config.h:63-69`), `help.docs` (the `ReleaseNotice`
  native URL opener seam, `editor_main.cpp:711-712`), `help.about` (chrome dialog: version +
  `update.state`), `selection.clear` (`editor.selection-set []` relay), `view.panel.open.settings`
  (`PanelHost.open("builtin.settings")`), `view.window.close` (→ `window.close`), `window.quit`
  (primary-window close policy, `window_bridge.cpp:257-268`); Window list from `window.list` →
  `window.focus` (a1's surface). Existing commands back the rest (03's table).
- **Accelerator column** displays `DEFAULT_KEYBINDINGS` strings (`keymap.ts:191-211`) where a
  binding exists; it does NOT imply global keymap wiring (01 §7) — except NSMenu equivalents on
  macOS. The e07c resolver seam stays untouched.
- **Enablement** via the existing when-context (`when.ts:232-267`) — disabled, never
  hidden-vs-shown flicker.
- **Ten-smoke rule**: `menu.publish` is a new boot-time surface — all ten live CEF smokes updated
  in this PR (`window_bridge.h:5-10`).

## Definition of Done

- Model → both renderings covered: web menubar keyboard-nav + ARIA via ts-a11y browser assertions
  (the settings worked example, `test_coverage.cpp:299-305`); NSMenu build + activation
  round-trip asserted on the macOS legs/smokes where CI can carry it, with anything CI-unreachable
  named in the PR body as deferred interactive verification.
- Every new command implemented, registered, and tested; disabled-item honesty pinned (tooltip
  reason present, item truly inert); menu-item activation reaches the same registry path as the
  palette for at least one shared command (single-dispatch pinned).
- All ten smokes green (`bridge.refused() == 0`); tests plant-verified both halves (R-QA-013);
  full 42-check CI green.
