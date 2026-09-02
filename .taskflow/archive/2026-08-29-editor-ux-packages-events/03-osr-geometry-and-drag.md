# 03 — OSR geometry, popups, and drag (infrastructure)

Covers tasks `a0`, `a1`, `a2`, `b1`. Read before touching `src/editor/shell/cef/`,
`src/editor/shell/src/compositor.cpp`, `dpi.h`, or any platform input path.

---

## 0. The premise

The editor runs CEF **windowless (OSR)**. In that mode the host owes CEF a contract: report geometry,
accept pixels, deliver input, and drive the drag protocol against the OS. `01` §1 measures how much of
it we owe and have not paid — **5 of 17** `CefRenderHandler` members, zero of the windowless
`CefBrowserHost` drag family, and no `CefContextMenuHandler`.

Nothing here is a CEF defect and nothing here is fixed by changing framework — see `README.md` §
"The framework question, answered once". The work is completing an adoption.

---

## a0 — The OSR conformance audit

**Deliverable: a table, not code.** For every member of `CefRenderHandler`, every windowless-only
`CefBrowserHost` method (`DragTargetDragEnter`, `DragTargetDragOver`, `DragTargetDragLeave`,
`DragTargetDrop`, `DragSourceEndedAt`, `DragSourceSystemDragEnded`, `SendMouse*`, `SendKeyEvent`,
`SendTouchEvent`, `SetFocus`, `WasResized`, `WasHidden`, `NotifyScreenInfoChanged`,
`NotifyMoveOrResizeStarted`, `SetWindowlessFrameRate`, `ImeSetComposition` and siblings), and
`CefContextMenuHandler`, record exactly one of:

- **implemented** — with the `file:line`;
- **deliberately not needed** — with the reason and, where one exists, the ruling that decided it
  (`OnAcceleratedPaint` is the model: owner ruling 2026-07-19, rationale in `cef_shell.h`);
- **gap** — with the user-visible consequence and a task id or a follow-up issue.

Lands in `docs/shell.md` as a new section, adjacent to the existing manual-verification tables.

Gaps expected to be registered (they are *not* in this set's scope — the point is that they stop being
invisible):

| Gap | Consequence |
|---|---|
| `GetAccessibilityHandler` | OS-level accessibility is not automatic in OSR. The `gui-a11y-*` gates assert the C++ models and the DOM and are honestly green; a screen reader still sees nothing in this window. Sharp for a repo with R-A11Y-001 |
| `OnImeCompositionRangeChanged`, `OnVirtualKeyboardRequested` | Composition-based input has no candidate-window placement |
| `OnTextSelectionChanged` | OS services that read the selection get nothing |
| `NotifyMoveOrResizeStarted` | Popups are not dismissed/repositioned when the window moves |

**Why this is a task and not a paragraph in someone's PR:** the evidence that piecemeal adoption fails
is this report. Three of the owner's seven items — the dead drag (#3) and both halves of #5, the
misplaced dropdown and the offset context menu — trace to it.

---

## a1 — `GetScreenPoint` (and `GetRootScreenRect`)

**Symptom:** the right-click context menu appears at the cursor's *view* position measured from the
**screen** origin, not the window's.

**Cause:** the member is not overridden (`01` §1). The default returns `false`, and CEF then uses view
coordinates as screen coordinates. `CefContextMenuHandler` is also unimplemented, so CEF displays its
own menu — positioned through exactly this callback.

**Fix:** convert view DIP → screen using the window's live placement. The Shell already tracks
placement and persists it — `WindowPlacement {monitor, x, y, width, height, maximized}`
(`editor_state.h:74-88`).

⚠ **The two members do NOT share a coordinate convention** — read the pinned header before assuming
they do, because this is the same class of mistake `a2` exists to fix and it is likewise invisible at
scale 1.0:

| Member | Convention (pinned `cef_render_handler.h`) |
|---|---|
| `GetScreenPoint` (`:88-91`) | *"Windows/Linux should provide screen **device (pixel)** coordinates and MacOS should provide screen **DIP** coordinates"* — the per-platform split |
| `GetRootScreenRect` (`:70-72`) | *"the root window rectangle in **screen DIP** coordinates"* — **DIP on every platform**, no split |

So `GetScreenPoint` gets `osr_screen_extent`'s treatment and `GetRootScreenRect` does **not**. Applying
the split to both multiplies the root rect by the scale factor on Windows and Linux.

**Where the arithmetic goes:** `dpi.h`, not inside the CEF binding. That file already establishes the
rule and the reason — `osr_screen_extent` lives there because the per-platform DIP-vs-device branch is
the one branch the local gate cannot build *and* no CI job executes, so a wrong choice would surface as
a whole-UI mis-scale found by a human on a Mac. `dpi.h:74-75` already names `GetScreenPoint` as sharing
that split, so it gets the identical treatment: portable arithmetic in `dpi.h`, compiled and tested on
all three legs, with the caller passing its platform's convention.

⚠ **The placement is not reachable from the callback yet.** `ShellCefClient` holds only
`logical_size_`, `dpi_`, `sink_`, `browser_` and the router (`cef_shell.cpp:1006-1014`) — no window and
no placement. `a1` therefore also adds the channel that carries it in, on the precedent of
`resize(logical_size, dpi)` (`browser.h:78`), and must land on the **client** origin rather than the
window rect: a frameless window's client is inset from it (`win32_frameless_client_insets`). That
plumbing is why `a1` is not as small as the one-line symptom suggests.

**Tests:** the conversion is pure and testable on all three `build` legs. Its home is
**`editor-shell-test_dpi`** (`src/editor/shell/CMakeLists.txt:238` — *"scale derivation, rounding, the
never-collapse rule, DIP<->physical points"*), which is where the existing DIP arithmetic is pinned; a
non-integral scale and a window at a non-zero screen origin are the two cases that discriminate, plus
one asserting `GetRootScreenRect` stays DIP on all three platforms. The live half rides the
Windows/Linux CEF smokes.

---

## a2 — The popup rect coordinate space

**Symptom, both halves from one cause:** at 150 % scale a `<select>` dropdown renders in the wrong
place and cropped, and clicking it dismisses it without selecting, while arrow keys + Enter work.

**Cause** (`01` §2): `OnPopupSize` gives DIP; `compositor.cpp:329` stores it raw; `:539` (GPU) and
`:632-641` (CPU) use it as a **physical-pixel destination rect**, while the texture from `OnPaint` is
physical — as `:581` already says about itself. The user clicks the *drawn* popup; CEF hit-tests
against the *true* DIP rect elsewhere; the click falls outside and CEF closes the popup.

**Fix:** convert DIP → physical, on **both** present paths, and derive the destination size from the
physical texture rather than from the DIP rect.

⚠ **The compositor is DPI-blind today** — check this before planning the seam. `WindowCompositor` has
**no `DpiScale` at all**: `on_resize(render::Extent2D physical_size)` (`compositor.h:190`) takes a size
and nothing else, and neither `compositor.h` nor `compositor.cpp` mentions `DpiScale` anywhere. The
`resize(logical_size, DpiScale)` that does carry the scale is the **browser's** (`browser.h:78`), one
layer up. So a scale has to be brought to the conversion; there are two honest places and the task
picks one, with its reason:

1. **Convert before the sink** — in `ShellCefClient::deliver_popup_state` (`cef_shell.cpp:996-1004`),
   where `dpi_` is already a member (`:1009`). The compositor stays DPI-blind and
   `IBrowserFrameSink::on_popup_state` (`browser.h:63`) starts carrying physical pixels like every
   other rect the compositor holds. Smaller, and it matches `ViewportLayer::content_rect`'s existing
   "PHYSICAL pixels" contract (`compositor.h:88`).
2. **Give the compositor the scale** — extend `on_resize` the way the browser's `resize` already is.
   Larger, but `e3` wants a scale in the compositor anyway.

Whichever is taken, take it **once**: do not let a second DPI source appear beside the first.

### The gate that makes this real

> **A regression test for this MUST run at a device scale ≠ 1.**

At scale 1.0 the correct and the broken implementation produce byte-identical output, so a test at 1.0
is vacuous — it cannot fail on the bug it exists to pin. This is why
`editor-shell-test_compositor` is green today and why `docs/shell.md` §10 still carries `PET_POPUP` as
manually unverified.

Add a scaled sibling to the existing popup cases in `editor-shell-test_compositor` asserting the
composited destination rect **in physical pixels** at (at minimum) 1.5, and keep the 1.0 cases so the
identity path stays pinned. Assert INSIDE and OUTSIDE the popup rect — asserting both is what
distinguishes a real second layer from a popup dropped or drawn full-window, the discipline
`docs/shell.md` already records for this test.

---

## b1 — HTML5 drag-and-drop in OSR (all three OSes, D11)

**Scope:** `StartDragging` + `UpdateDragCursor` on the render handler, and the injection calls
`DragTargetDragEnter` / `DragTargetDragOver` / `DragTargetDragLeave` / `DragTargetDrop` /
`DragSourceEndedAt` / `DragSourceSystemDragEnded` driven from each platform's OS drag manager:

| OS | Mechanism |
|---|---|
| Win32 | OLE — `IDropSource` / `IDropTarget` / `DoDragDrop`, `RegisterDragDrop` on the window |
| X11 | XDND |
| Cocoa | `NSDraggingSession` / `NSDraggingDestination` |

**Prior art:** CEF's own `cefclient` sample carries reference OSR drag implementations upstream
(`tests/cefclient/browser/osr_dragdrop_win.cc`, `osr_dragdrop_x11.cc`). ⚠ They are **not vendored in
our minimal distribution** (`include/`, `libcef_dll/`, `Release/`, `Resources/` only), so treat them as
public prior art to read, not as code in the tree.

**What comes free once this lands:** Dockview already implements tab drag, drop-to-split into any
edge, and re-docking — verified in the pinned bundle (`01` §3). **No `panelhost.ts` change is required
for them.** Do not add one; if drag still does not work after `b1`, the fault is in `b1`.

**What must not regress:** the Shell-mediated **cross-window** drag
(`cross_window_drag.h`, `drag.ts`, ctest `editor-cef-smoke-shell-drag`). The two are layered: Dockview
owns in-window DnD, the Shell owns the cross-window session. `drag.ts:19-21` states that split and it
stays true.

### Verification, honestly

- **Linux is the only OS where CI can drive a real gesture.** Since `e12a-x11-legs` the
  `editor-cef-smoke-shell` Linux leg injects a real pointer move/press/release through the X server
  into a live CEF browser. A drop asserted there is a genuine end-to-end proof.
- **Windows CI is Session-0** — no interactive desktop, so the gesture cannot be driven. The Windows
  half is pure-logic tests plus a row in the manual table.
- **macOS** injects in-process via `postEvent:`, and AppKit's delivered pointer location is only
  approximate, so assert direction and separation, not equality — the constraint `docs/shell.md` §11
  already records for `translate_ns_event`.

Every OS-specific claim that CI cannot reach goes into `docs/shell.md`'s manual table with what CI
*does* pin beside it, in the format that section already uses — so "manual" never reads as
"unverified".

### Why `a1` is a hard dependency, not a preference

`StartDragging`'s `(x, y)` is documented in **screen** coordinates, while `DragTargetDragOver` and
`DragTargetDrop` take a `CefMouseEvent` in **view** coordinates. The host therefore has to run that
conversion in both directions, which is exactly `a1`'s arithmetic — `b1` cannot be built correctly on
top of a `GetScreenPoint` that still returns `false`. So `a1` is in `b1`'s `needs`, alongside `a2`,
which settles the other coordinate-space question in the same window.

### Risk

This is the largest single change in the set and it has **no local compile signal**: the Windows dev
gate is Ninja + GCC and CEF is an MSVC/Clang-ABI prebuilt, so nothing about this task builds locally.
CI is the sole authority, on three legs, and D11 puts all three in one task — a fault on any of them
blocks the whole thing. Budget for CI round-trips accordingly.
