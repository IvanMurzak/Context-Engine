---
id: "a2-strips-scaffold"
title: "Four-strip scaffold, titlebar content, and the first real regionProvider"
group: "A"
sequence: 2
repo: "."
base_branch: "main"
depends_on: ["a1-chrome-contract"]
importance: 8
complexity: 8
security_critical: false
production_touching: false
model_hint: "top"
taskflow_refs: ["01-current-architecture.md", "02-target-architecture.md"]
---

## Goal

Restructure the web layer into the mockup's four-strip frame — titlebar (38px) / play-bar slot
(40px) / dock / statusbar (24px) — with chrome-mode gating off `chrome.state`, real titlebar
content, and the FIRST real `regionProvider` publishing caption/control rects. The CEF-smoke
pixel-coverage cost is paid deliberately in this PR.

## Scope & seams

- **`app/index.html` becomes a flex column**: `#editor-titlebar` / `#editor-playbar` (empty shell
  until d1) / `#editor-root` (flex:1) / `#editor-statusbar` (empty shell until d2);
  `#editor-banners` STAYS the fixed overlay (`index.html:60-71`). CSP intact — no inline styles;
  new rules land in `app.css` (already staged).
- **Chrome-mode gating** (02 §1–2): `custom` = full titlebar incl. window-controls cluster;
  `hybrid` = no controls, left padding = `controlsInset`; `system` = menu-bar-only strip, no drag
  duty. ALL modes implemented and DOM-tested by injecting `chrome.state` values — the live
  backends all report `"system"` until b1/c1 land (interim-honesty staging, tasks/README.md).
- **Titlebar content**: brand · project name · palette button →
  `registry.execute(PALETTE_TOGGLE_COMMAND_ID)` (pattern `boot.ts:1096-1100`) · controls cluster
  dispatching `window.minimize` / `window.toggle-maximize` / `window.close`; the max/restore glyph
  flips on a1's `maximized` fact. Strip styled in `app.css` from existing tokens
  (`colors.panel/panel2/line/ink/muted` + `shape.*`); every control is a kit component; NO new kit
  family (twelve stay closed), NO new tokens unless the theme-schema check finds a value with no
  honest home (02 §2).
- **regionProvider** (02 §6): replace the empty default (`editorstate.ts:222`, `boot.ts:467-471`)
  with a real provider: measure the caption drag surface + control rects
  (`getBoundingClientRect` → physical px) and publish WHOLESALE on layout change, resize, and DPI
  change. Controls publish AFTER the caption rect — back-to-front last-match-wins
  (`input.cpp:44-56`) makes carve-out tokens unnecessary.
- **Welcome mode**: strips render on the welcome screen too (the mockup's frame is the app's
  frame); the play-bar slot hides there (02 §2). The welcome smokes (`editor-shell-welcome-t2`)
  join the update set (ROADMAP risk 5).
- **CEF smoke coverage**: the dock shrinks by 102px — update the per-pixel background-coverage
  expectations deliberately (`index.html:66-70`). A coverage delta beyond the strips' own pixels
  is a real regression, not an expectation to widen (ROADMAP risk 1).

## Definition of Done

- Strip DOM + mode gating + glyph flip covered by webui tests across all three chrome modes and
  both welcome/project modes.
- Region publish observed end-to-end in a live smoke: provider fires on resize/layout/DPI change,
  generation bumps (`input.cpp:38-42`), caption + control rects arrive in physical px.
- ts-a11y: titlebar controls carry labels/roles per the banners worked example
  (`app.css:680-688`, `banners.ts`).
- All ten CEF smokes green with the updated coverage expectations; welcome smokes updated; kit
  gates hold (no raw colors/lengths, no `.ctx-widget-*` outside kit.css).
- Tests plant-verified both halves (R-QA-013); full 42-check CI green.
