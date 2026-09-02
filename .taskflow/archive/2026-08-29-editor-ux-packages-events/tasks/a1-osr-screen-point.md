---
id: "a1-osr-screen-point"
title: "Implement GetScreenPoint and GetRootScreenRect with the placement channel; fix the context-menu offset"
group: "A"
sequence: 1
repo: "."
base_branch: "main"
depends_on: []
importance: 8
complexity: 8
security_critical: false
production_touching: false
model_hint: "top"
taskflow_refs: ["01-current-architecture.md", "03-osr-geometry-and-drag.md"]
---

## Goal

The right-click context menu appears at the cursor's *view* position measured from the **screen**
origin, because `CefRenderHandler::GetScreenPoint` is not overridden — its default returns `false`
and CEF then uses view coordinates as screen coordinates. Implement `GetScreenPoint` and
`GetRootScreenRect` on `ShellCefClient`, converting through the window's live placement, with the
arithmetic in `dpi.h` where all three CI legs compile and test it.

## Scope & seams

- **`ShellCefClient`** (`src/editor/shell/cef/src/cef_shell.cpp:653`): override both members.
- ⚠ **The two members do NOT share a coordinate convention** (pinned `cef_render_handler.h`):
  - `GetScreenPoint` (`:88-91`): screen **device (pixel)** coordinates on Windows/Linux, screen
    **DIP** on macOS — the same per-platform split `osr_screen_extent` already encodes.
  - `GetRootScreenRect` (`:70-72`): screen **DIP on every platform** — no split.
  Applying the split to both multiplies the root rect by the scale factor on Windows/Linux. This is
  the same class of mistake `a2` fixes and is likewise invisible at scale 1.0.
- **The arithmetic goes in `dpi.h`**, not inside the CEF binding — `dpi.h:74-75` already names
  `GetScreenPoint` as sharing `osr_screen_extent`'s split, and that file exists precisely because the
  per-platform branch is the one branch the local gate cannot build and no CI job executes. Portable
  pure functions in `dpi.h`; the CEF callback passes its platform's convention.
- **The placement plumbing is the real size of this task.** `ShellCefClient` holds only
  `logical_size_`, `dpi_`, `sink_`, `browser_` and the router (`cef_shell.cpp:1006-1014`) — no window,
  no placement. Add the channel that carries the live window placement in, on the precedent of
  `resize(logical_size, dpi)` (`browser.h:78`). The conversion must land on the **client** origin, not
  the window rect — a frameless window's client is inset (`win32_frameless_client_insets`).
  `WindowPlacement {monitor, x, y, width, height, maximized}` is at `editor_state.h:74-88`.
- Out of scope: `CefContextMenuHandler` (CEF's own menu, positioned through this callback, is
  acceptable once positioned correctly); drag (`b1`); the popup composite rect (`a2`).

## Definition of Done

- New cases in **`editor-shell-test_dpi`** (`src/editor/shell/CMakeLists.txt:238`), the file where DIP
  arithmetic is pinned, covering at minimum:
  - the view→screen conversion at a **non-integral scale** (e.g. 1.5) — a test at scale 1.0 is
    vacuous here and does not satisfy this;
  - a window at a **non-zero screen origin** (otherwise the missing offset cannot fail);
  - `GetRootScreenRect` stays **DIP on all three platform conventions** (pinning the no-split rule).
- The client-origin (frameless inset) case is exercised, not just the window-rect case.
- The live half rides the existing Windows/Linux CEF smokes; no smoke regresses.
- The placement channel has exactly one source — no second copy of placement state appears beside the
  existing `WindowPlacement` tracking.
- CEF-linking targets stay exempt from `context_warnings`; the new `dpi.h` arithmetic is headless and
  fully warning-gated. No local compile signal exists for the CEF half — CI is the sole authority and
  every check is green before merge.
- PR body cites this set and notes the a0 conformance-table row this closes.
