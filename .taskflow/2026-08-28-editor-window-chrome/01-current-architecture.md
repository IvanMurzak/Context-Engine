# 01 — Current architecture: what exists, verified (2026-08-28)

Every claim carries `file:line` against CE `main` @ `e68450d`. Negative findings name what was
searched.

## 1. The window backends draw stock OS chrome — nothing else

- **Win32**: one window class, one style for every window — `win32_window.cpp:454`
  `const DWORD style = WS_OVERLAPPEDWINDOW;`, `CreateWindowExW(0, …)` with literal `dwExStyle = 0`
  (`:477-479`), class registered once and shared (`:445-452`). **Zero DWM calls anywhere** (grep
  `dwmapi|Dwm[A-Z]|uxtheme` over `src/`: only CEF's generated build files); the shell links
  `user32 ole32 uuid winhttp shell32` only (`src/editor/shell/CMakeLists.txt:154` — no `dwmapi`).
- **The WndProc handles no non-client message.** The message constants the pure decoder knows
  (`window.h:197-223`) contain no `WM_NCCALCSIZE`/`WM_NCHITTEST`/`WM_NCLBUTTONDOWN`/
  `WM_GETMINMAXINFO`/`WM_SYSCOMMAND`; `test_window.cpp:232` pins `WM_NCHITTEST` as deliberately
  un-decoded. The two switch statements a frameless path would extend: the OS-side switch
  `win32_window.cpp:366-413` (the only place returning non-`DefWindowProcW`) and the pure
  `translate_win32_message` switch `window.cpp:181-290` with its `static_assert` block
  (`win32_window.cpp:40-78`).
- **Maximize state exists only as `WindowPlacement.maximized`** (`editor_state.h:74-88`), read via
  `GetWindowPlacement` (`win32_window.cpp:536-568`), applied via `SetWindowPlacement` (`:570-590`),
  and observed by a 250 ms POLL (`shell.cpp:239-253`, `shell.h:82-85`) — no event, no bridge fact.
- **Cocoa**: plain titled window — `cocoa_window.mm:410-415` (`Titled|Closable|Miniaturizable|
  Resizable`), plain `NSView` + `CAMetalLayer` content view with no subclass (`:427-436`).
  `titlebarAppearsTransparent`, `NSWindowStyleMaskFullSizeContentView`, `titleVisibility`,
  `standardWindowButton:`, `performWindowDragWithEvent:` appear **nowhere**. Load-bearing pump
  rule: `[NSApp sendEvent:event]` is ALWAYS forwarded, decoded or not (`:689-692`) — window drag,
  the menu, IME and key equivalents depend on it.
- **X11**: `XCreateWindow` with no decoration handling (`x11_window.cpp:370-378`);
  `_MOTIF_WM_HINTS` appears nowhere in the repo; maximize is EWMH `_NET_WM_STATE` only
  (`:528-542`, private method). Server-side decorations are the current — and per D6 the target —
  state.
- **`IWindowBackend` (window.h:95-142) has 12 operations and no chrome verbs**: no `minimize()`,
  `maximize()`, `restore()`, `begin_move_drag()`, `set_position()`. `request_activation()`
  incidentally un-minimizes on Win32 (`win32_window.cpp:255-262`). Any new virtual lands in FOUR
  implementations (Headless `window.h:152-191`, Win32, X11, Cocoa).

## 2. The region seam exists, is wired, and publishes an empty set forever

- Closed two-token vocabulary: `RegionKind { viewport, native }` (`input.h:42-48`), wire tokens
  `"viewport"`/`"native"` (`editor_state_bridge.h:81-82`), TS mirror `editorstate.ts:45-47`,
  cross-checked by the `webui-panel-contract` gate. An unknown kind is REFUSED at parse
  (`editor_state_bridge.cpp:48-59`, `:78-88`).
- Publish path is live end-to-end: `publishRegions` (`editorstate.ts:117-130`) → bridge handler
  (`editor_state_bridge.cpp:271-289`) → per-window `input().regions().publish()`
  (`editor_main.cpp:284-291` created windows, `:607-618` window 0). Wholesale replace, generation
  bump (`input.cpp:38-42`).
- **But no `regionProvider` is ever supplied** (`boot.ts:467-471`; default returns `[]`,
  `editorstate.ts:222`), publish fires only on Dockview layout change / `pagehide`
  (`editorstate.ts:300,304`), and **hits are dropped on the floor**: `target_for` maps the two
  kinds to `InputTarget::viewport|native` (`input.cpp:29-32`) whose dispatch arm is an empty
  `break` (`shell.cpp:207-212` — "no native consumer exists yet"). Arbitration itself is real:
  back-to-front last-match-wins (`input.cpp:44-56`), implicit press capture (`:142-162`).

## 3. The bridge has no window-control or chrome surface

- `window.*` vocabulary is complete at 6 methods + 5 drag/ui-mirror methods
  (`window_bridge.h:58-108`, `window.ts:34-39`): list, tear-out, move-to, seed, rehomed, close.
  **No minimize / maximize / restore / begin-drag / window-state**; `editor.state.get` returns
  only `{layout, panels}` (`editor_state_bridge.cpp:118-138`). No platform/capability flag reaches
  editor-core from any surface (handshake `ipc_bridge.h:254-281`, `welcome.state`
  `welcome.cpp:293-310`, config keys `user_config.h:63-69`, `WindowDesc` `window.h:83-92` — all
  checked).
- ⚠ Standing constraint (`window_bridge.h:5-10`): **a new boot-time bridge surface must be
  installed in every live CEF smoke** (ten of them) or the deny-by-default router turns the calls
  into `unknown_method` and trips each smoke's `bridge.refused() == 0` invariant — the e06d
  regression, twice measured.

## 4. The web layer has no chrome strips — and two ready precedents

- `app/index.html` body = `#editor-root` (`:60`) + the `#editor-banners` fixed OVERLAY (`:71`,
  `pointer-events:none`, z-index 40 — `app.css:740-751`). The overlay choice is documented as
  protecting Dockview's measured box AND the CEF smokes' per-pixel background-coverage floor
  (`index.html:66-70`) — permanent layout strips WILL change that box, so the smokes' expectations
  are part of this change's blast radius. No inline scripts/styles (CSP, `index.html:10-27`); a
  new stylesheet needs CMake staging + an `index.html` link (`webui/CMakeLists.txt:138,199,229`).
- Kit: **twelve authored families, a closed list** (`kit/src/index.ts:107-135`); no toolbar, no
  menu, no status-dot family. The sanctioned pattern for chrome: "the CONTROLS inside a banner are
  the kit's while the strip itself is styled here" (`app.css:680-688`); `banners.ts` is the worked
  example (kit `createButton`/`createBadge` at `:28,217-234`, ARIA split `:211-213,255-257`).
  Kit gates forbid raw colors/lengths and `.ctx-widget-*` rules outside `kit.css`
  (`kit/README.md:106-135`, `app.css:344-349`).
- **The Pulse-of-Work flourish is fully implemented in CSS with ZERO writers**: `.ctx-flourish` +
  five `data-play-state` selectors + keyframes (`app.css:211-290`), tokens derived per theme
  (`theme.ts:275-295`, values in `tokens/themes/*.theme.json`), reduced-motion honored twice
  (`theme.ts:315-337`, `app.css:104-113`). `app.css:204-206` says it aloud: setting
  `data-play-state` "is all a Play button (**a later task**) has to set." ⚠ Vocabulary mismatch:
  flourish has 5 states (idle/running/compiling/error/paused), the daemon `PlayState` has 3
  (`edit|playing|paused`, `when.ts:37`); no mapping exists anywhere.
- Theme reaches the DOM as `--ctx-*` custom properties on `<html>` (`theme.ts:70,585-588,841-846`);
  **no titlebar/caption/chrome token exists** in any theme group (searched `tokens/**`,
  `core/src`); a strip composes from `colors.panel/panel2/line/ink/muted` + `shape.*` like every
  other chrome surface.

## 5. Play state and play control today

- **The full RPC chain is live** (e08b): dock-panel button → `panel.command` → provider `invoke`
  (`session_feed.cpp:247-274`) → `PlaybarModel` → `PlayControlGateway` = `SessionFeed`
  (`session_feed.h:78,115-118`) → `drive_play` issues `editor.play|pause|stop|step`
  (`session_feed.cpp:184-241`) → daemon handler publishes the `play-state` session fact
  (`kernel_server.cpp:1004-1061`) → subscribers apply with `origin` echo suppression
  (`session_feed.h:11-16`). Verbs registered (`registry.cpp:1012-1042`), scope-gated
  (`scope.cpp:169`), redteam-pinned (`test_redteam_boundaries.cpp:189`).
- editor-core reads play state by POLLING `session.state` at 500 ms (`session.ts:40,51,210-223`;
  reply shape `session_bridge.cpp:44-54`: `{state, attached, generation}` — **no `simTick`**), and
  feeds the when-context (`when.ts:159-211`, `boot.ts:553-580`). There is no push channel to the
  renderer by construction (`session.ts:10-18`).
- The palette lists `contract.editor.play|pause|stop|step` (auto-projected from the client schema,
  `commands.ts:301-314`, `generated/client-schema.ts:508-547`) **but their dispatch is a stub**:
  `boot.ts:1044-1047` returns "daemon RPC fan-in not wired yet (D19)". No `play.*` editor command
  exists in the registry (`commands.ts:357-499` checked).
- ⚠ CE **#356** stands: no `play-state` GET verb on the daemon (`session_bridge.h:42-46`,
  `test_kernel_server.cpp:983` lists the complete verb set), so post-daemon-restart staleness has
  no honest repair. The strip inherits this; fixing it is out of scope here (tracked upstream).

## 6. Removing `builtin.playbar` from the dock — the measured blast radius

- Roster entry (`builtin_roster.cpp:98-100`, the only `DockZone::top` and only
  `kCapabilitySessionControl` panel) · a11y factory (`a11y/registry.cpp:68-70`) + manifest row
  (`coverage.manifest.jsonl:16`) · help topic (`help_model.cpp:179-181`) · `hostable_panel_ids()`
  6→5 (`builtin_panels.cpp:494-516`).
- Gates/tests that red on removal and must be amended IN THE SAME TASK:
  `test_builtin_panels.cpp:153-220,463` (hardcoded 6, hosts playbar), `test_roster.cpp:102-106`
  (M5 exit panel list), `test_m5exit2_a11y_coverage.cpp:133-140` (hardcoded id list),
  `m5-exit-1-walkthrough` (`test_m5exit1_walkthrough.cpp:391-506` drives the playbar panel),
  `m5-exit-3` seam checklist (`test_m5exit3_seam_checklist.cpp:353-365,501-504`),
  `gui-help-contextual` + `m85-exit-4c` (both-ways roster↔topics), `gui-a11y-coverage`
  (roster==factories==manifest, `test_coverage.cpp:221-339`). Ten CEF/live smokes are
  count-coupled via `hostable_panel_ids()` and follow automatically.
- **What survives**: the `PlaybarModel` transport + `SessionFeed` writer/subscriber and their
  suites (`test_playbar_model.cpp`, `test_session_feed.cpp`, `editor-session-panels-t2`,
  `editor-session-multiclient-t2` token parity `test_e08a…:243-245`) — the strip drives play
  through them; only the uitree PANEL rendering (`playbar_panel.cpp`) and its roster/a11y/help
  anchors retire.

## 7. Menu and keymap

- **No menu system exists** in any layer (searched shell, webui, kit). The command registry (e07b)
  is the backing store: contract verbs auto-projected + 9 editor commands + 2 session commands
  (`commands.ts:301-499`, implementations `boot.ts:1283-1414`).
- The declared `Ctrl+Shift+P` palette binding is NOT wired: `KeymapController` is constructed only
  in tests (`keymap.ts:473`, `test/keymap.test.ts:284`); `boot.ts:592-609` builds a throwaway
  `Keymap` to validate overrides and discards it; the native resolver seam resolves nothing
  (`docs/shell.md:314-320`). A titlebar palette BUTTON calling
  `registry.execute(PALETTE_TOGGLE_COMMAND_ID)` (pattern `boot.ts:1096-1100`) is the only
  reliable opener today — and the first non-programmatic one.

## 8. Present path vs framelessness (Windows)

The CPU blit is expressed entirely in CLIENT coordinates (`present_blit.cpp:192-264`: `GetDC`
client DC at `:223`, destination extent = compositor `size_` seeded from `client_size()`,
`shell.cpp:151`). A `WM_NCCALCSIZE` that grows the client area flows through the EXISTING resize
protocol (`shell.cpp:167-174`); no frame-inset arithmetic exists to break. `WM_ERASEBKGND` is
claimed (`win32_window.cpp:391-393`); newly-client pixels are only ever filled by the blit's
letterbox `FillRect` (`present_blit.cpp:240-244`).
