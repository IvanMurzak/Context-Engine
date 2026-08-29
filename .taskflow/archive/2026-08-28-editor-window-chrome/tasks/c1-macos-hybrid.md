---
id: "c1-macos-hybrid"
title: "macOS hybrid chrome: transparent titlebar, measured controls inset, caption drag handoff"
group: "C"
sequence: 1
repo: "."
base_branch: "main"
depends_on: ["a1-chrome-contract", "a2-strips-scaffold"]
importance: 7
complexity: 8
security_critical: false
production_touching: false
model_hint: "top"
taskflow_refs: ["01-current-architecture.md", "02-target-architecture.md"]
---

## Goal

macOS gets the hybrid chrome: content extends under a transparent titlebar, native traffic lights
stay exactly where macOS puts them, the strip pads by a MEASURED inset, and a caption press hands
the drag to the OS. `chrome.state` flips to `mode:"hybrid"` with a real `controlsInset` in the
same PR.

## Scope & seams

- **Window style** (`cocoa_window.mm:410-415` extension):
  `styleMask |= NSWindowStyleMaskFullSizeContentView`, `titlebarAppearsTransparent = YES`,
  `titleVisibility = NSWindowTitleHidden`. Traffic lights untouched.
- **`controlsInset.left`** measured from `standardWindowButton:` frames and served in
  `chrome.state` (a1's cocoa arm flips from `"system"`/0 to `"hybrid"`/measured here — the
  interim-honesty staging, tasks/README.md); a2's hybrid gating already pads the strip by it.
- **Caption drag**: on a decoded pointer PRESS whose position hits a published `caption` region,
  the pump calls `[window_ performWindowDragWithEvent:event]` and suppresses the browser dispatch
  for THAT press only. The load-bearing `[NSApp sendEvent:]` always-forward rule
  (`cocoa_window.mm:689-692`) holds for every other event — window drag, menu, IME, and key
  equivalents depend on it (01 §1).
- **Double-click on caption** = `zoom:` (platform convention, 02 §4).
- **NOT here**: NSMenu (d3 owns `menu.publish` + the native menu); no window-controls cluster in
  the strip on macOS (a2's hybrid mode already omits it).

## Definition of Done

- macOS CEF smoke: `mode:"hybrid"` reported with `controlsInset.left > 0`; caption-press
  suppression asserted (no stuck hover — ROADMAP risk 3); non-caption events still forwarded
  (existing smoke families that exercise IME/key-equivalents stay green).
- Double-click-zoom covered where CI can carry it; anything CI-unreachable is named in the PR body
  as deferred interactive verification (the `docs/shell.md` precedent).
- `bridge.refused() == 0` holds (no new bridge surface expected in this task; if one becomes
  necessary, the ten-smoke rule applies).
- Tests plant-verified both halves (R-QA-013); full 42-check CI green (the macOS legs are the
  authoritative gate — this task is not locally verifiable on the Windows dev box).
