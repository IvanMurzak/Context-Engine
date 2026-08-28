# Editor window chrome — the mockup-grade titlebar, play bar, statusbar, and menu

**Problem.** The shipped editor window diverges from the owner-approved d1 visual direction
(`../2026-07-01-context-engine/m9-editor/mockups/editor.html`) in every chrome surface: the OS
draws a default light titlebar (no DWM styling exists at all — `win32_window.cpp` makes zero DWM
calls), the Play Bar renders as a generic uitree dock panel (raw text + text buttons) instead of
the mockup's full-width L-51 strip, there is no app menu, and no statusbar. The gap is not a
regression: the d1 mockups carried the chrome design, but only tokens/themes/flourish/fonts were
decomposed into tasks (e06a–e06d); design 03's window section and 04's dock design never absorbed
the custom titlebar or the play-bar-as-fixed-chrome, so no e-task owns them (verified 2026-08-28
against `m9-editor/ROADMAP.md` pending rows e11c–e17).

**Status.** PLANNING (this set). Owner decisions locked 2026-08-28. Awaiting review
(`taskflow-review`) before task decomposition and dispatch.

## Decisions

| # | Date | Decision |
|---|---|---|
| D1 | 2026-08-28 | **Full menu system in this phase** — File / Edit / View / Selection / Panel / Window / Help, with structure, contents, and behavior; backed by the existing e07b command registry (owner chose over the lighter "menu bar from existing commands" and "no menu" options). Per the owner's standing "respect OS conventions" constraint this set designs the menu per-OS: **in-titlebar web menu on Windows/Linux, the native global `NSMenu` menu bar on macOS** fed from the same registry (see `02-target-architecture.md` §menu; flagged as a design consequence, not a separately-asked decision). |
| D2 | 2026-08-28 | **The docked `builtin.playbar` panel is REMOVED** — the full-width strip is the Play Bar's only home, per the mockup. The C++ `PlaybarModel`/`SessionFeed` transport survives as the play-control writer; only the dock PANEL rendering retires. Blast radius (roster, a11y manifest, m5-exit gate amendments) is itemized in `01-current-architecture.md` and owned by explicit tasks. |
| D3 | 2026-08-28 | **The statusbar (mockup bottom strip, 24px) is IN scope** for this phase. |
| D4 | 2026-08-28 | **Secondary (torn-out) OS windows get the same custom chrome minus the menu** — compact titlebar: panel title + window controls. |
| D5 | 2026-08-28 | **No third-party windowing framework, no new heavyweight dependencies.** Electron was explicitly evaluated and rejected in this conversation (architecture stays L-15/L-41: CEF composited by the engine-owned window); the chrome is built from OS-native mechanisms + the existing web layer only. |
| D6 | 2026-08-28 | **Linux keeps server-side decorations in v1** — the user's WM draws the frame (that IS the Linux convention); the web titlebar strip does not render its window-control cluster there. Client-side decorations are a possible later option, out of scope here. |
| D7 | 2026-08-28 | **The interim "phase 1" (DWM-dark native titlebar) is SKIPPED** — this set goes straight to the mockup-grade chrome. |

## Summary of the target (details in 02)

Four fixed strips frame the dock, all rendered by editor-core in the existing web layer, themed by
the existing e06 tokens: **titlebar** (38px: brand, menu [Windows/Linux], project name, palette
button, window controls [Windows only; macOS keeps native traffic lights via
`titlebarAppearsTransparent` + `fullSizeContentView`; Linux keeps the WM's own frame]), **play
bar** (40px: transport with the Pulse-of-Work Play button per TOKENS.md §5, status label, timer,
target chip), the **dock root**, and the **statusbar** (24px). On Windows the window goes
frameless via the standard `WM_NCCALCSIZE`/`WM_NCHITTEST` pattern with Snap Layouts preserved
(`HTMAXBUTTON`); drag/caption/control regions are published by editor-core over the EXISTING
e05d2 `editor.regions.publish` seam (new region kinds), never a new channel. Play control rides
the EXISTING `editor.play|pause|stop|step` RPC chain through the surviving `SessionFeed` writer.

## Document map

| Document | Contents |
|---|---|
| [`README.md`](README.md) | This file — problem, status, decisions, map |
| [`ROADMAP.md`](ROADMAP.md) | Implementation ledger: waves, task board, gates, progress log |
| [`01-current-architecture.md`](01-current-architecture.md) | Verified current behavior + change seams, all claims `file:line` |
| [`02-target-architecture.md`](02-target-architecture.md) | Target design per OS, strip anatomy, decisions and trade-offs |
| [`03-menu-structure.md`](03-menu-structure.md) | The D1 full menu: tree, command backing, per-OS placement and behavior |

**Design authority upstream:** `../2026-07-01-context-engine/` — mockups (`m9-editor/mockups/`:
`editor.html` structure `titlebar` L67 → `play-bar` L82 → `dock-root` L98 → `statusbar` L363;
`shared/components.css` `.titlebar` L160–202, `.play-bar` L509–522, `.statusbar` L525–538;
`TOKENS.md` §5 Pulse-of-Work state→hue+rhythm map, owner pick O1 2026-07-19), decision locks
`core/DESIGN-DECISIONS.md` (L-14 web UI, L-15 CEF-in-host, L-41 compositing tree, L-51 loud play
state). This set implements that authority; it does not amend it.
