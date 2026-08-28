---
id: "a1-chrome-contract"
title: "Chrome contract: chrome.state read, window-control surface, caption region vocabulary"
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
taskflow_refs: ["01-current-architecture.md", "02-target-architecture.md"]
---

## Goal

Land the entire cross-layer chrome CONTRACT in one PR, with zero visual change: the `chrome.state`
bridge read, the `window.minimize` / `window.toggle-maximize` / `window.focus` bridge methods,
`IWindowBackend::minimize()` / `set_maximized(bool)` across all four backends, the `maximized`
fact on the `editor.ui` relay, and the four caption `RegionKind` tokens in all four mirror sites.
Everything later in the set builds on these surfaces.

## Scope & seams

- **`chrome.state`** (02 §1): `{mode: "custom"|"hybrid"|"system", controlsInset: {left, right},
  maximized, focused, window: "primary"|"secondary"}`, served by the Shell, fetched by editor-core
  at boot beside `welcome.state`; TS read function added on the client side. **Interim honesty
  (binding, see tasks/README.md)**: every backend reports `mode:"system"` and inset 0 in this
  task; b1/c1 flip win32/cocoa in the PRs that implement the behavior; x11 stays `"system"` (D6).
  `window` derives from the boot seed that already distinguishes torn-out windows (`boot.ts:363`).
- **Window controls**: `window.minimize` and `window.toggle-maximize` beside the existing
  `window.close` (`window_bridge.h:58-70`); `window.focus` routing `request_activation`
  (`win32_window.cpp:255-262`). TS mirror in `window.ts:34-39`.
- **`IWindowBackend`** (`window.h:95-142`) gains `minimize()` + `set_maximized(bool)` in FOUR
  implementations: promote X11's private EWMH `_NET_WM_STATE` shape (`x11_window.cpp:528-542`) to
  the interface; Win32 via the existing placement machinery (`win32_window.cpp:536-590`); Cocoa
  (`miniaturize:`/`zoom:`); Headless (`window.h:152-191`) honest state-only.
- **`maximized` fact**: the 250 ms placement poll already detects changes (`shell.cpp:239-253`);
  publish a fact on the EXISTING `editor.ui` mirror relay (`window_bridge.h:87-102`). No new push
  channel, no extra poll.
- **`RegionKind` +4**: `caption`, `caption_min`, `caption_max`, `caption_close` (wire tokens
  `"caption"`, `"caption-min"`, `"caption-max"`, `"caption-close"`) in all FOUR mirror sites in
  ONE commit: `input.h:42-48`, the bridge wire tokens (`editor_state_bridge.h:78-82`, parse
  refusal intact `editor_state_bridge.cpp:48-59`), `editorstate.ts:45-47`, and the
  `webui-panel-contract` gate. `target_for` / dispatch arms stay honestly empty — b1/c1 are the
  consumers (`shell.cpp:207-212` precedent comment updated to name them).
- **Ten-smoke rule**: `chrome.state`, `window.minimize`, `window.toggle-maximize`, `window.focus`
  are new boot-time surfaces — installed in ALL TEN live CEF smokes in this PR
  (`window_bridge.h:5-10`).
- **NOT here**: any UI (a2), NC/frameless handling (b1), `session.control` (d1), `menu.publish`
  (d3).

## Definition of Done

- All four backends implement the two new virtuals; pure/unit coverage in the existing
  `editor-shell-test_*` families runs on all three OS legs (plain families — no ci.yml edits).
- `chrome.state` returns the honest interim values per backend; smoke assertions cover the read
  and the three methods; `bridge.refused() == 0` holds in all ten smokes.
- The four-site vocabulary mirror lands atomically; `webui-panel-contract` gate green; unknown-kind
  parse refusal still pinned.
- The `maximized` fact is observed in a test through the relay (placement flip → fact arrives).
- Tests plant-verified both halves (R-QA-013); full 42-check CI green.
