# Editor UX, panel instances, and the package fact bus (2026-08-29)

**Status: REVIEWED — plan locked, ready for `taskflow-tasks`.** Reviewed 2026-08-29 against the tree,
the pinned CEF SDK headers and internal consistency; nine confirmed findings corrected, one product
fork returned to the owner and recorded as `D1` **REVISED**. No finding and no owner question remains
open.

## The problem

The owner walked the live editor on 2026-08-29 and reported seven defects and gaps. Investigation
found that they are not seven independent bugs, and the table below is the map. **Items #3 and #5 share
one root cause** — the editor runs CEF in windowless (OSR) mode, where the host must implement a
substantial part of what a browser window normally does, and only **5 of `CefRenderHandler`'s 17
members** are implemented; #5 alone accounts for two of the reported symptoms. **Item #4 is a second
root cause**: a panel's identity *is* its manifest id, so a panel can neither be reopened by name nor
exist twice. **Item #7 is a design extension** the two-tier event model was built to accommodate but
does not yet reach. **Items #1, #2 and #6 stand alone** — a stylesheet gap, a renderer duplication, and
a panel whose compositor half is built and whose producer is not.

| # | Owner's report | What it actually is |
|---|---|---|
| 1 | No hover/press feedback on interactive elements | The kit's WIDGET layer styles 3 of 12 roles and declares no transition, no pressed state, no disabled state |
| 2 | The panel title is printed twice | Eight of the nine C++ panel models emit their own title as a `heading`; Dockview also renders it in the tab |
| 3 | Panels cannot be dragged by their tab | **All HTML5 drag-and-drop in the editor is dead** — `StartDragging`'s default return of `false` means CEF *aborts* every drag |
| 4 | A closed panel cannot be reopened | There is no "open panel" surface at all, and no panel identity that could name an instance |
| 5 | Dropdowns are misplaced and swallow clicks; the context menu is offset by the window's screen position | Two OSR geometry bugs: the popup rect is used in DIP against a physical-pixel target, and `GetScreenPoint` is not implemented |
| 6 | There is no window showing the scene | `builtin.viewport` is rostered but not hostable, and the existing model is a text summary, not an image |
| 7 | Packages must broadcast events to each other without depending on each other | The two-tier model already does this for selection; it lacks a typed subject and refuses cross-package subscription **by an explicit design decision** |

Full evidence with `file:line` citations: [`01-current-architecture.md`](01-current-architecture.md).

### The framework question, answered once

The owner asked whether a framework that cannot drag a tab is the wrong framework. It is not, and the
answer is recorded here so it is not re-asked every time this set is picked up.

CEF has two modes. **Windowed** — the browser owns an OS window, and the OS provides drag, popups,
context menus, IME and accessibility. **Windowless (OSR)** — *we* own the window; CEF hands us pixels
and we hand it input. In OSR, HTML5 drag-and-drop *cannot* work without the host, because a browser
implements it by talking to the OS drag manager (OLE `DoDragDrop` on Windows, XDND on X11,
`NSDraggingSession` on macOS), and there is no window to do that with. CEF states this plainly: the
`DragTarget*` family is documented "only used when window rendering is disabled", and
`CefRenderHandler::StartDragging`'s default returns `false`, which its own header defines as *"abort
the drag operation"*. Today's drags are not ignored — they are actively refused by the default.

OSR was the right choice and remains so: this editor composites the CEF web layer together with GPU
scene layers into **one** window (`ViewportLayer` drawn beneath the CEF layer through its transparent
holes, with `RegionMap` splitting input between them). A windowed browser cannot do that — its pixels
belong to the OS, not to our swapchain. Every alternative was weighed and each costs more than the
fix or loses something already built: CEF windowed and Electron demote the viewport to a separate
overlay window (throwing away `src/render/present/`, the compositor layer stack, `RegionMap` and the
L-41 handoff, and permanently losing the ability to put web UI *over* the scene); WebView2/WKWebView
have the same windowed-vs-visual-hosting split with three implementations instead of one; Ultralight
and Sciter are OSR-only and therefore impose *more* input duty; a native toolkit discards the entire
web tier including the sandboxed-iframe model that third-party package UI depends on.

**So the defect is an incomplete adoption, not a wrong framework** — and that framing is what task
`a0` acts on.

## Decisions

All ratified by the owner on **2026-08-29** unless marked otherwise. A later change is marked
`REVISED` with its own date. An implementer does not re-open these.

| id | Decision | Why it was a fork |
|---|---|---|
| **D1** | **Selection is per subject kind.** `entity` and `file` selections coexist and do not clear each other (Unreal-style, not Unity-style). `subject` is an OPTIONAL wire parameter defaulting to `entity`, so the contract change is **additive** and `protocolMajor` does not move. **REVISED 2026-08-29:** additive covers the REPLY too — `selection-get` **keeps** its existing `ids` member and **adds** `selections: [{subject, ids}]`, rather than being replaced by a bare array. Review found the replacement would silently break every current reader (`attach_command.cpp:157` among them) while `08 §4` asserted the change was additive; the owner took the additive reply. Cost: `ids` is redundant with `selections[entity]` until a major moves | The alternative — one shared selection — is a smaller change but makes "click a file" silently deselect the object |
| **D2** | **The subject-kind vocabulary is extensible and namespaced per package.** `entity`/`file`/`asset` are contract-owned; a package declares `<pkg>.<kind>` in its manifest and the registry validates the namespacing | A closed enum means a third-party package can never introduce its own notion of selection |
| **D3** | **The Inspector tiebreak is a tier-1 fact.** The daemon publishes `selection-focus {subject, origin}` and persists it | Deciding by tier-2 panel focus is cheaper but leaves the CLI and an AI agent unable to know what the human is looking at — which fails the two-tier model's own test for what belongs in the daemon |
| **D4** | **Cross-package broadcast rides daemon topics, not the `editor.ui` bus.** Manifest `events.publishes[]` / `events.subscribes[]`; subscribing to another package's topic is an install-time consented grant, deny-by-default | Opening the `editor.ui` bus is cheaper but window-local — invisible to agents, to the CLI, and to a second window |
| **D5** | **A package fact is a STATE, not an edge.** The daemon retains the last value per topic and refuses to publish a repeat; publishing from inside an event handler is refused | **This is the cycle breaker** — see the hazard note below. Without it the architecture ships an unbounded loop |
| **D6** | **Full instance-id refactor now.** `dock.singleton: bool` → `instances: {mode, max}`, plus a `path` field for menu grouping. Breaking: `kContractMajor` 2 → 3 | Deferring is far cheaper today and far more expensive after v1 ships, because the compatibility window is exactly one major |
| **D7** | **Full interactive viewport**: live render + camera + mouse picking that propagates to Hierarchy and Inspector | |
| **D8** | **Picking is a CPU raycast** against the extracted `render::RenderSnapshot`, not a GPU id-buffer readback | A GPU buffer is pixel-exact but assertable only on the one Linux render leg; a CPU raycast is assertable on all three build legs with no GPU, which is this repo's headless-first rule |
| **D9** | **The `Window` menu opens things; the `Panel` menu acts on the focused one.** Window gets the searchable panel tree; the OS-window list moves to a subsection of the same menu | The owner looked under `Window` and found OS windows. `Panel` already exists and holds tear-out/move — a different verb class |
| **D10** | **The Files panel is a full file manager** — rename / move / delete, not a read-only tree | Entering the L-30 write path costs undo, the `file_write` grant, refusal UX and loud notices; the owner accepted that cost |
| **D11** | **HTML5 drag-and-drop lands on all three OSes in one task** | The repo's three-leg parity culture over a smaller Windows-first PR. Accepted risk: no local compile signal (the dev gate's GCC cannot link CEF), so CI is the sole authority and a fault on any OS blocks the whole task |
| **D12** | **The OSR contract is audited as a list before anything else is built on it** (task `a0`) | Raised by the owner's framework question. Without it the remaining gaps surface one at a time over months — drag today, IME next, the screen reader after that |

### Two rulings taken by the planner

Mechanical consequences with no product trade-off, recorded so an implementer does not re-litigate:

- **a11y coverage is audited per panel KIND, not per instance.** Auditing N copies of one model
  proves nothing the first copy did not.
- **The duplicated panel title is hidden in the RENDERER, not deleted from the C++ model.** Headless
  and CLI consumers read that heading, `role_requires_name(Role::heading)` is true, and
  `gui-a11y-coverage` gates it. A renderer-side visual hide costs zero C++ change and zero gate risk.

### Rulings added by the review (2026-08-29)

- **`b1` depends on `a1` and `a2`, not on `a0` alone.** `StartDragging` delivers screen coordinates
  while `DragTarget*` consumes view coordinates, so the drag work needs `a1`'s conversion to exist.
  The `needs` column now says what `03`'s prose always did.
- **`selection-get`'s reply keeps `ids`** — the `D1` REVISED entry above.

### The hazard D5 exists to close

The cycle breaker in the shipping event model is **"a no-op publishes nothing"** — daemon-side state
dedup — **not** `origin`. Two packages hold **separate** baseline daemon sessions and therefore
**different** origins, so origin echo suppression does not stop an A → B → A mirroring loop. Arbitrary
package topics have no daemon-side state to dedup against, so opening cross-package broadcast without
D5 would ship an unbounded loop. This is the single most important sentence in the set.

## What the work is

Seventeen tasks in six waves. The ledger, dependency graph, gates and board are in
[`ROADMAP.md`](ROADMAP.md).

- **Wave A** — the OSR contract audit, the two OSR geometry hotfixes, and two renderer-only fixes.
  Small, independent, and what the owner sees change first.
- **Wave B** — HTML5 drag-and-drop in OSR. One task, three OSes, the largest single C++ change in the
  set; it alone restores tab drag, split-on-drop and re-docking, because Dockview implements those
  itself once the events arrive.
- **Wave C** — the contract: typed selection (additive), manifest v3 (breaking), the instance runtime.
- **Wave D** — the surfaces built on wave C: the Window menu and the package fact bus.
- **Wave E** — the two new panels: Files and the Scene viewport.
- **Wave F** — closeout: the boundary deny-list the docs already record as owed, and the manual
  verification tables.

## Document map

| Document | Read it when |
|---|---|
| [`01-current-architecture.md`](01-current-architecture.md) | You need the verified behaviour and the exact change seams, with citations |
| [`02-target-architecture.md`](02-target-architecture.md) | You need the target design and the trade-offs each decision bought |
| [`03-osr-geometry-and-drag.md`](03-osr-geometry-and-drag.md) | You are touching the CEF host, the compositor, DPI, or input |
| [`04-panel-instances-and-menu.md`](04-panel-instances-and-menu.md) | You are touching the panel manifest, `PanelHost`, layout restore, or the menu |
| [`05-selection-and-package-events.md`](05-selection-and-package-events.md) | You are touching selection, the daemon session state, or package events |
| [`06-viewport-and-files.md`](06-viewport-and-files.md) | You are building the Scene viewport or the Files panel |
| [`07-ui-states.md`](07-ui-states.md) | You are touching the kit stylesheets, the chrome CSS, or the Dockview theme bridge |
| [`08-compatibility-and-migration.md`](08-compatibility-and-migration.md) | **Before** changing any version number — three separate versioned surfaces move in this set |
