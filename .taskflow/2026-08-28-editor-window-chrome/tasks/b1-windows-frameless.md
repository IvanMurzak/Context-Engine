---
id: "b1-windows-frameless"
title: "Windows frameless: WM_NCCALCSIZE/WM_NCHITTEST with a pure hit-test over published regions"
group: "B"
sequence: 1
repo: "."
base_branch: "main"
depends_on: ["a1-chrome-contract", "a2-strips-scaffold"]
importance: 8
complexity: 8
security_critical: false
production_touching: false
model_hint: "top"
taskflow_refs: ["01-current-architecture.md", "02-target-architecture.md"]
---

## Goal

Windows goes mockup-frameless via the standard pattern: keep `WS_OVERLAPPEDWINDOW` (Snap,
animations, minimize-to-taskbar preserved), take over the frame in the two NC messages with a PURE
hit-test function fed by the a2-published caption regions, and flip `chrome.state.mode` to
`"custom"` in the same PR.

## Scope & seams

- **`WM_NCCALCSIZE`** (wParam=TRUE): return the full window rect as client, inset by the
  DPI-scaled system resize border on left/right/bottom; when maximized, inset ALL sides by the
  frame so content never spills off-monitor. **`WM_GETMINMAXINFO`** clamps maximized size to the
  monitor work area. Style stays `WS_OVERLAPPEDWINDOW` (`win32_window.cpp:454`); the OS-side
  switch to extend is `win32_window.cpp:366-413`.
- **`WM_NCHITTEST`** decided by a pure function in `window.cpp` (the `translate_win32_message`
  discipline, `window.cpp:181-290`): `hit_test_frame(point, client_size, dpi, regions) → HT*` —
  resize bands first (DPI-scaled border + corner metric), then the published chrome regions:
  `caption-close/max/min` → `HTCLOSE`/`HTMAXBUTTON`/`HTMINBUTTON` (HTMAXBUTTON lights Snap
  Layouts on Win11), `caption` → `HTCAPTION`, else `HTCLIENT`. New WM_/HT constants join the local
  block (`window.h:197-223`) + the `static_assert`s (`win32_window.cpp:40-78`).
  `test_window.cpp:232` (WM_NCHITTEST pinned as deliberately un-decoded) flips to the new truth.
- **The OS owns drag/snap/double-click/system-menu** through `HTCAPTION` — no hand-rolled drag
  loop, no CEF `OnDraggableRegionsChanged`, no `-webkit-app-region` (02 §11–12).
- **`DWMWA_USE_IMMERSIVE_DARK_MODE`** set from the active theme's appearance — the ONE Dwm call
  this design adds; `dwmapi` joins the shell link list (`src/editor/shell/CMakeLists.txt:154`).
- **Caption press suppression**: a caption press must never half-reach the browser (a stuck hover
  in the strip) — asserted in the live smokes (ROADMAP risk 3).
- **Present path unchanged by design** (01 §8): the enlarged client flows through the existing
  resize protocol (`shell.cpp:167-174`); only the letterbox `FillRect` paints new pixels
  (`present_blit.cpp:240-244`). Do not add frame-inset arithmetic to the blit.
- **`chrome.state`**: the win32 backend now reports `mode:"custom"` (interim-honesty staging —
  the flip lands with the behavior).

## Definition of Done

- `hit_test_frame` exhaustively unit-tested on ALL THREE OS legs via the existing
  `editor-shell-test_window` family (pure function): resize bands + corners, region precedence
  (control-over-caption, last-match-wins), HTMAXBUTTON, DPI scaling. No ci.yml edits for plain
  families.
- Maximized-inset correctness pinned at BOTH DPI 96 and 150% — no 8px overhang (ROADMAP risk 2).
- Windows CEF smoke: caption regions consumed before client routing (`route_pointer` never sees a
  caption click), suppression asserted, `bridge.refused() == 0` holds, coverage expectations
  still exact.
- Dark-mode DWM attribute follows theme appearance, asserted where CI can carry it; anything
  CI-unreachable is named in the PR body as deferred interactive verification (the
  `docs/shell.md` precedent).
- Tests plant-verified both halves (R-QA-013); full 42-check CI green.
