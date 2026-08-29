# 03 — The application menu (D1: full menu in this phase)

One declarative menu MODEL in editor-core; two renderings. Windows/Linux: a web menubar inside the
titlebar strip (chrome-styled, kit-controlled where a control exists, ARIA
`menubar`/`menu`/`menuitem` with full keyboard nav — arrows/Enter/Escape/Home/End; Alt-mnemonics
deferred, recorded). macOS: the model is PUBLISHED to the Shell over a new bridge method
(`menu.publish`, renderer → Shell) and the Cocoa backend builds the native global `NSMenu` from
it; an activated item comes back as a fact on the EXISTING `editor.ui` mirror relay
(`window_bridge.h:87-102`) carrying the command id, which editor-core executes through the same
registry as a click in the web menubar. NSMenu key equivalents ride the same return path. No
second dispatch system exists: every item names a command id in the e07b registry.

**Every v1 item is backed by something that exists (or is built in this set).** Items whose
backing is future work render disabled with the reason in their tooltip — the honest-degrade
house rule — and are marked ⏳ below.

## The tree

| Menu | Item | Backing (command id → mechanism) |
|---|---|---|
| **App** (macOS only) | About Context Editor | `help.about` (new) → chrome dialog: version (`CONTEXT_EDITOR_VERSION`), update state from the existing `update.state` read (`banners.ts:33-36`) |
| | Settings… | `view.panel.open.settings` (new) → `PanelHost.open("builtin.settings")` |
| | Quit Context Editor | `window.quit` (new; primary-window close path — `window.close` policy, `window_bridge.cpp:257-268`) |
| **File** | New Project… | `project.new` (new) → the e14c welcome flow (`WelcomeBridge`, spawns `context new` via the located CLI — `welcome.cpp`) |
| | Open Project… | `project.open` (new) → `welcome.pickFolder` + `welcome.open` (existing surfaces) |
| | Open Recent ▸ | recents from `welcome.state` (`welcome.cpp:293-310`) / config (`user_config.h:63-69`); each entry → `welcome.open` |
| | Close Window | `view.window.close` (new) → `window.close` |
| | Exit (Windows/Linux) | `window.quit` |
| **Edit** | Undo / Redo | `session.undo` / `session.redo` (existing — `commands.ts:471-499`) |
| | Cut / Copy / Paste | enabled only under `textInputFocus` (when-context, `when.ts:67`), delegating to the browser's native editing; disabled elsewhere ⏳ (app-level clipboard is future) |
| **View** | Command Palette | `workbench.palette.toggle` (existing — `palette.ts:29`) |
| | Toggle Theme | `view.theme.toggle` (existing) |
| | Focus Next / Previous Panel | `view.panel.focusNext/Previous` (existing; honest-refusal implementations today — `boot.ts:1319-1330` — the menu inherits that honesty) |
| | Close Panel | `view.panel.close` (existing) |
| **Selection** | Clear Selection | `selection.clear` (new) → `editor.selection-set []` over the strip's `session.control`-style relay; the menu grows with e11 picking (recorded) |
| **Panel** | Tear Out Panel | `view.window.tearOut` (existing) |
| | Move Panel to Primary | `view.window.moveToPrimary` (existing) |
| | Move Panel ◂▸▴▾ | `view.panel.move.*` (existing) |
| **Window** | Minimize | `window.minimize` (new surface, 02 §5) |
| | Maximize / Restore | `window.toggle-maximize` (new surface, 02 §5; label flips on the `maximized` fact) |
| | window list | from `window.list` (existing); entries → `window.focus` (new: routes `request_activation`, `win32_window.cpp:255-262`) |
| **Help** | Documentation | `help.docs` (new) → the Shell's native URL opener (the `ReleaseNotice` opener seam, `editor_main.cpp:711-712`) |
| | About Context Editor (Win/Linux) | `help.about` |

Play/pause/stop deliberately have NO menu — the mockup gives them no menu either; they live in the
play bar strip and the palette (`play.*`, 02 §7).

## Constraints carried from the code

- New bridge surfaces (`menu.publish`, plus the 02 §5 window-control set) obey the ten-smoke
  installation rule (`window_bridge.h:5-10`).
- The menubar's accelerator COLUMN displays `DEFAULT_KEYBINDINGS` strings (`keymap.ts:191-211`)
  where a binding exists; it does not imply the keymap is globally wired (it is not —
  01 §7) except through NSMenu equivalents on macOS. Wiring the web-side keymap resolver stays
  e07c's seam.
- Dropdowns are app-chrome overlays in the palette's pattern (`palette_view.ts:77-117`,
  z-order stack `app.css:442-466` region) — no new kit family; ts-a11y browser assertions per the
  settings worked example (`test_coverage.cpp:299-305`).
- Menu content that depends on state uses the existing when-context (`when.ts:232-267`) for
  enablement, never hidden-vs-shown flicker.
