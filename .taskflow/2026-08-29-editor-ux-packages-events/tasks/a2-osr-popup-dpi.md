---
id: "a2-osr-popup-dpi"
title: "Convert the popup rect DIP → physical on both present paths, with a regression test at scale ≠ 1"
group: "A"
sequence: 2
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

At 150 % scale a `<select>` dropdown renders in the wrong place and cropped, and clicking it
dismisses it without selecting (arrow keys + Enter work). One cause, both symptoms: `OnPopupSize`
delivers the rect in **DIP**, `compositor.cpp:329` stores it raw, and it is then used as a
**physical-pixel destination rect** — `:539` (GPU path) and `:632-641` (CPU present path) — while the
popup **texture** from `OnPaint` is physical (`compositor.cpp:581` states this about itself). The
user clicks the *drawn* popup; CEF hit-tests the *true* DIP rect elsewhere; the click lands outside
and CEF closes the popup. Fix: convert DIP → physical on **both** present paths and derive the
destination size from the physical texture, not the DIP rect.

## Scope & seams

- ⚠ **The compositor is DPI-blind today** — `WindowCompositor` has no `DpiScale` anywhere;
  `on_resize(render::Extent2D physical_size)` (`compositor.h:190`) takes a size only. A scale must be
  brought to the conversion. Two honest seams; **pick exactly one and record why in the PR body and in
  a comment at the seam** (e3 will consume whichever is chosen):
  1. **Convert before the sink** — in `ShellCefClient::deliver_popup_state`
     (`cef_shell.cpp:996-1004`), where `dpi_` is already a member (`:1009`). The compositor stays
     DPI-blind and `IBrowserFrameSink::on_popup_state` (`browser.h:63`) starts carrying physical
     pixels like every other rect the compositor holds — matching `ViewportLayer::content_rect`'s
     existing "PHYSICAL pixels" contract (`compositor.h:88`). Smaller.
  2. **Give the compositor the scale** — extend `on_resize` the way the browser's
     `resize(logical_size, DpiScale)` already is. Larger, but `e3` wants a scale in the compositor
     anyway.
  Whichever is taken, take it **once** — a second DPI source beside the first is the failure mode.
- Both present paths change together: the GPU `draw_layer` call and the CPU clamp-and-blit.
- The pointer path is **not** implicated (`cef_shell.cpp:1256-1266` sends DIP and says so) — do not
  touch input dispatch.
- Out of scope: viewport layer rects (`e3`), screen-point conversion (`a1`).

## Definition of Done

- **A scaled sibling in `editor-shell-test_compositor`** at (at minimum) scale **1.5**, asserting the
  composited destination rect **in physical pixels**, with the existing 1.0 cases kept so the identity
  path stays pinned. This is the set's named vacuity gate: at scale 1.0 the correct and broken code
  are byte-identical, which is exactly why this bug is green in CI today — a test at 1.0 does not
  satisfy this task.
- The scaled test asserts pixels **INSIDE and OUTSIDE** the popup rect — both, which is what
  distinguishes a real second layer from a popup dropped or drawn full-window (the discipline
  `docs/shell.md` already records for this test).
- Both present paths (GPU and CPU) are covered by the scaled assertions.
- Exactly one DPI seam introduced, documented at the seam; `e3` can name it.
- All CI legs green (no local compile signal for the CEF half); `docs/shell.md`'s `PET_POPUP` manual
  row is left for `f2` to reconcile, but the PR body notes that the automated test now covers it.
- PR body cites this set.
