---
id: "f1-secondary-window-chrome"
title: "Secondary-window chrome: compact titlebar, frameless factory windows"
group: "F"
sequence: 1
repo: "."
base_branch: "main"
depends_on: ["a2-strips-scaffold", "b1-windows-frameless", "c1-macos-hybrid"]
importance: 7
complexity: 6
security_critical: false
production_touching: false
model_hint: "mid"
taskflow_refs: ["02-target-architecture.md", "README.md"]
---

## Goal

Torn-out OS windows get the same custom chrome minus the menu (D4): a compact titlebar — panel
title + window controls (Windows) / inset padding (macOS) — with no play bar and no statusbar,
gated off `chrome.state.window = "secondary"`; framelessness applies to every window the factory
creates.

## Scope & seams

- **Strip gating on `chrome.state.window`** (02 §9): `secondary` renders ONLY the compact
  titlebar — panel title + controls cluster (custom mode) or inset padding (hybrid mode); no
  menu, no play bar, no statusbar. `"secondary"` derives from the boot seed that already
  distinguishes torn-out windows (`boot.ts:363`) — a1 defined the field; this task makes the
  strips consume it.
- **Factory windows go frameless/hybrid**: b1's NC arms and c1's style mask hold for every window
  the factory creates (`window_registry.h` spec, same `WindowDesc`) — not just window 0.
- **Per-window regions**: each secondary window's editor-core publishes its own caption/control
  rects over its own channel (`editor_main.cpp:284-291` — created windows already wire
  `input().regions().publish()`).
- Existing tear-out / move-to / rehome behavior (`window_bridge.h:58-108` D6 mechanism) must be
  preserved bit-for-bit — this task changes chrome, not the panel-relay mechanism.

## Definition of Done

- The tear-out smoke asserts: `chrome.state.window == "secondary"` in the torn-out window, the
  compact strip renders (title + mode-correct controls), no menu/play-bar/statusbar DOM exists
  there, controls dispatch (`window.minimize`/`toggle-maximize`/`close`) work, and the secondary
  window publishes its own regions (generation bump observed).
- Windows and macOS factory-window chrome covered on their respective legs; existing
  tear-out/rehome suites stay green untouched.
- Tests plant-verified both halves (R-QA-013); full 42-check CI green.
