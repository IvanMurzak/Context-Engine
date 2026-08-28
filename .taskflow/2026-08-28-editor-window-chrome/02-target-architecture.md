# 02 — Target architecture: mockup-grade chrome without a windowing framework

Authority: the d1 mockups (`../2026-07-01-context-engine/m9-editor/mockups/editor.html` — strip
order titlebar 38px → play bar 40px → dock → statusbar 24px; CSS `.titlebar`/`.play-bar`/
`.statusbar` in `shared/components.css:160-202,509-538`) under decisions D1–D7 (README). Everything
below composes OS-native mechanisms with the EXISTING web layer, per D5.

## 1. The chrome contract — one new read, three modes

A single new bridge read, `chrome.state` (naming final at implementation), served by the Shell and
fetched by editor-core at boot alongside `welcome.state`:

```
{ mode: "custom" | "hybrid" | "system",   // win32 → custom, cocoa → hybrid, x11 → system
  controlsInset: {left, right},           // physical px the strip must reserve (macOS traffic lights; else 0)
  maximized: bool, focused: bool,
  window: "primary" | "secondary" }
```

- `mode` decides what the titlebar strip renders: `custom` = full strip incl. window controls
  (Windows); `hybrid` = strip minus window controls, left-padded by `controlsInset` (macOS);
  `system` = strip renders as a MENU BAR only — no controls, no drag duty (Linux, D6: the WM owns
  the frame; two stacked bars is the conventional Linux shape).
- `maximized` drives the max/restore glyph; changes are broadcast as a fact on the EXISTING
  `editor.ui` mirror relay (`window_bridge.h:87-102`) from the placement poll that already detects
  them (`shell.cpp:239-253`) — no new push channel, no extra poll.
- ⚠ Being a new boot-time surface, `chrome.state` must be installed in ALL TEN live CEF smokes in
  the same task (`window_bridge.h:5-10` rule) — budgeted, not discovered.

## 2. Strip layout in editor-core

`app/index.html` becomes a flex column: `#editor-titlebar` / `#editor-playbar` / `#editor-root`
(flex:1) / `#editor-statusbar`, with `#editor-banners` staying the fixed overlay it is. This
deliberately spends what `index.html:66-70` protected: Dockview's container shrinks by 102px and
the CEF smokes' per-pixel background-coverage assertions change — the smoke expectation updates are
an explicit task item, not a surprise. Strips are app chrome in the banners pattern
(`app.css:680-688`): strip styled in `app.css` from existing tokens (`colors.panel/line/ink/...`),
every CONTROL inside is a kit component; no new kit family (the twelve stay closed), no new tokens
(mockup values map onto the shipped groups; a `chrome` token subgroup is added ONLY if the theme
schema review during implementation finds a value with no honest home). CSP stays: no inline
styles; new rules land in `app.css` (already staged).

Strips render in BOTH welcome and project modes (the mockup's frame is the app's frame); the play
bar hides on the welcome screen (no session to control).

## 3. Windows — frameless via the standard pattern

Keep `WS_OVERLAPPEDWINDOW` (Snap, animations, minimize-to-taskbar all preserved); take over the
frame in the two NC messages, per the pattern every custom-titlebar app uses:

- **`WM_NCCALCSIZE`** (wParam=TRUE): return the full window rect as client, inset by the system
  resize border on left/right/bottom — and when maximized, inset ALL sides by the frame so content
  does not spill off-monitor. `WM_GETMINMAXINFO` clamps maximized size to the monitor work area.
- **`WM_NCHITTEST`**: decided by a PURE function (the `translate_win32_message` discipline —
  `window.cpp:181` sibling), `hit_test_frame(point, client_size, dpi, regions) → HT*`: resize
  bands first (DPI-scaled border + corner metric), then the published chrome regions —
  `caption-close/max/min` → `HTCLOSE/HTMAXBUTTON/HTMINBUTTON` (HTMAXBUTTON is what lights Snap
  Layouts on Win11), `caption` → `HTCAPTION`, else `HTCLIENT`. The function is executed by
  `editor-shell-test_window` on all three legs; new WM_/HT constants join the local block
  (`window.h:197-223`) + `static_assert`s (`win32_window.cpp:40-78`).
- The OS then owns drag, snap, double-click-maximize, and system menu for free — that is the
  point of `HTCAPTION` over hand-rolled dragging. `DWMWA_USE_IMMERSIVE_DARK_MODE` is set from the
  active theme's appearance so the drop shadow/edge tint matches (the ONE Dwm call this design
  adds; dwmapi joins the shell link list).
- Web-drawn caption buttons dispatch over the new control surface (§5); hover visuals are pure
  CSS. The CPU present path needs no change (01 §8) — the enlarged client flows through the
  existing resize protocol.

## 4. macOS — hybrid, native buttons stay native

`styleMask |= NSWindowStyleMaskFullSizeContentView`, `titlebarAppearsTransparent = YES`,
`titleVisibility = NSWindowTitleHidden` (`cocoa_window.mm:410-415` extension). Traffic lights
remain exactly where macOS puts them; `controlsInset.left` is measured from
`standardWindowButton:` frames and published in `chrome.state`, the strip pads accordingly.
Caption drag: on a decoded pointer PRESS whose position hits a `caption` region, the pump calls
`[window_ performWindowDragWithEvent:event]` and suppresses the browser dispatch for that press —
the `[NSApp sendEvent:]` always-forward rule (`cocoa_window.mm:689-692`) is respected for every
other event. Double-click on caption = `zoom:` (the platform convention). The menu does NOT render
in the strip on macOS — it feeds the native `NSMenu` bar (03-menu-structure.md), which is what
"respect the OS" means there.

## 5. Window controls + state

`IWindowBackend` gains `minimize()` and `set_maximized(bool)` (promoting X11's private
`x11_window.cpp:528-542` shape to the interface; four implementations + Headless). The bridge
gains `window.minimize` and `window.toggle-maximize` beside the existing `window.close`
(`window_bridge.h:58-70`); `window.close` already carries the primary-vs-secondary policy. TS
mirror in `window.ts:34-39`; smokes updated per §1. `maximized` reaches the strip via the
`editor.ui` fact (§1), so the glyph flips without polling.

## 6. Regions — the vocabulary grows, the seam does not

`RegionKind` gains `caption`, `caption_min`, `caption_max`, `caption_close` (wire tokens
`"caption"`, `"caption-min"`, …) in the closed vocabulary (`input.h:42-48`,
`editor_state_bridge.h:78-82`, `editorstate.ts:45-47`, `webui-panel-contract` gate — all four
sites in one commit). Editor-core finally supplies the `regionProvider` that has been an empty
default since e05d2 (`editorstate.ts:222`): the titlebar measures its drag surface and control
rects (`getBoundingClientRect` → physical px) and publishes on layout change, resize, and DPI
change — wholesale, per the existing contract. On Windows the NC hit-test consumes these regions
BEFORE client routing (§3), so `route_pointer` never sees a caption click; on macOS the pump
consumes `caption` presses (§4); the browser receives everything else unchanged. `no-drag`
carve-outs need no token: controls publish AFTER the caption rect and back-to-front last-match
wins (`input.cpp:44-56`).

## 7. The Play Bar strip

The mockup strip (transport / status dot + label / timer / target chip), rendered by editor-core
in the titlebar's sibling strip, kit controls throughout, replacing the dock panel (D2).

- **State**: the existing 500 ms `session.state` poll (`session.ts:210-223`). The reply gains
  `simTick` (additive; the daemon already mints it — `kernel_server.cpp:1050-1060` — and
  `SessionFeed` already holds it) so the `t+…` timer is truthful. Poll cadence is enough for a
  status strip; the CE #356 restart-staleness caveat is inherited and documented, not solved here.
- **Flourish**: the strip's Play button finally writes `data-play-state`
  (`app.css:204-206` — "all a Play button has to set"), with the honest 3→5 mapping:
  `edit→idle`, `playing→running`, `paused→paused`; `compiling`/`error` stay unreachable until the
  build pipeline publishes those facts (recorded in the strip's code as the extension point).
- **Control**: new bridge method `session.control {verb: play|pause|stop|step}` on the EXISTING
  `SessionBridge` (`session_bridge.h:68`), relaying to the surviving `SessionFeed` writer
  (`session_feed.cpp:184-241`) — the strip rides the proven RPC chain with its `origin` echo
  suppression; the D19 contract-dispatch stub (`boot.ts:1044-1047`) stays untouched. Editor-core
  registers real `play.play/pause/stop/step` commands (ids match the retired panel's,
  `playbar_model.h:70-73`) dispatching to `session.control` — one implementation serving strip
  buttons, palette, and the D1 menu.
- **Honesty rule for mockup items with no source**: FPS is not rendered (nothing measures it until
  e11); the Target chip renders static "Scene", disabled, as declared future surface.

## 8. Statusbar

24px bottom strip, same chrome pattern. v1 content = what already has a truthful source: daemon
link state (the `daemon.linkState` read the banners already use — `banners.ts:33-36`), problems
count (the Problems panel's model already receives the diagnostics feed), active theme/project
name. Anything else waits for its source.

## 9. Secondary windows (D4)

Same chrome mode, compact strip: panel title + window controls (Windows) / inset padding (macOS);
no menu, no play bar, no statusbar. `chrome.state.window = "secondary"` (the boot seed already
distinguishes torn-out windows — `boot.ts:363`); the strips gate on it. Frameless applies to every
window the factory creates (`window_registry.h` spec picks up the same `WindowDesc`).

## 10. Retiring the dock panel (D2)

One task removes: roster entry (`builtin_roster.cpp:98-100`), a11y factory + manifest row
(`registry.cpp:68-70`, `coverage.manifest.jsonl:16`), help topic (`help_model.cpp:179-181`), and
amends the enumerated gates — `test_builtin_panels` counts, `test_roster.cpp:102-106`,
`test_m5exit2_a11y_coverage.cpp:133-140`, the m5-exit-1 walkthrough's playbar leg re-pointed at
the surviving model+RPC path, m5-exit-3 seam rows, help both-ways gates (full list: 01 §6). Gate
amendments are owner-visible in the PR body (frozen-gate courtesy), with the e06d five-gate
partition as precedent. `PlaybarModel`/`SessionFeed` and their suites survive untouched.

## 11. Decisions and trade-offs

| Choice | Over | Why |
|---|---|---|
| `HTCAPTION`/NC pattern on Windows | hand-rolled drag loops, CSS `app-region` via CEF's drag-handler | The OS keeps snap/drag/dbl-click semantics; regions already have a proven in-house channel (e05d2) — a second CEF-specific channel would duplicate it and `OnDraggableRegionsChanged` adds an unverified dependency |
| Strips as layout siblings | the banners' fixed-overlay trick | Strips are permanent chrome, not transient notices; overlaying the dock would steal its pixels. The smoke-coverage cost is paid consciously (§2) |
| `session.control` relay | wiring the D19 contract fan-in now | The fan-in is its own design (D19); the strip needs four verbs that already have a tested writer |
| Reuse retired command ids `play.*` | new ids | Palette/keymap/docs continuity; the ids are already the daemon verbs' names |
| macOS native NSMenu | in-window web menu everywhere | The owner's own constraint — macOS convention IS the global menu bar |
| Linux = menu-bar strip over SSD | client-side decorations | D6; forcing CSD is the un-Linux move, and it would be the only platform needing hand-rolled window buttons AND resize handling |

## 12. Non-goals

No CEF `OnDraggableRegionsChanged` dependency; no `-webkit-app-region` reliance (the CSS may still
carry it as documentation, the shell never reads it). No auto-update/installer work (e15). No
visual-regression CI (e16 — this set hands e16 stable chrome to pin). No CSD on Linux. No fix for
CE #356. No live FPS/Target sources. No keymap-wiring beyond what the menu needs (the e07c
resolver seam stays as is).
