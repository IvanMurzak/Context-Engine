---
id: "d1-playbar-strip"
title: "Play Bar strip: mockup transport on the proven RPC chain, first data-play-state writer"
group: "D"
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

Fill a2's play-bar slot with the mockup strip — transport / status dot + label / `t+` timer /
target chip — wired to the proven daemon RPC chain through ONE new `session.control` bridge
method, with truthful state (a new additive `simTick` field) and the first-ever writer of
`data-play-state`. The docked panel coexists until e1 retires it.

## Scope & seams

- **Anatomy + honesty** (02 §7): kit controls throughout; NO fps (nothing measures it until e11);
  the Target chip renders static "Scene", disabled, as declared future surface.
- **State**: the existing 500 ms `session.state` poll (`session.ts:210-223`). The reply gains
  `simTick` (additive — the daemon already mints it, `kernel_server.cpp:1050-1060`, and
  `SessionFeed` already holds it; reply shape today `session_bridge.cpp:44-54`) so the `t+` timer
  is truthful. CE **#356** post-restart staleness is INHERITED and documented in the strip's code
  + PR body — fixed upstream, not here (ROADMAP risk 6).
- **Flourish**: the Play button writes `data-play-state` (`app.css:204-206` — "all a Play button
  has to set") with the honest 3→5 mapping: `edit→idle`, `playing→running`, `paused→paused`;
  `compiling`/`error` stay unreachable, recorded in code as the extension point (the 01 §4
  vocabulary-mismatch note). Reduced-motion handling is already in the theme layer — add no new
  motion rules.
- **Control**: new bridge method `session.control {verb: play|pause|stop|step}` on the EXISTING
  `SessionBridge` (`session_bridge.h:68`), relaying to the surviving `SessionFeed` writer
  (`session_feed.cpp:184-241`) — the e08b chain with its `origin` echo suppression
  (`session_feed.h:11-16`). The D19 contract-dispatch stub (`boot.ts:1044-1047`) stays untouched.
- **Commands**: real `play.play` / `play.pause` / `play.stop` / `play.step` registered (ids
  match the dock panel's, `playbar_model.h:70-73`), dispatching to `session.control` — ONE
  implementation serving strip buttons, palette, and the d3 menu.
- **Welcome**: strip hidden on the welcome screen (02 §2).
- **Ten-smoke rule**: `session.control` is a new boot-time surface — all ten live CEF smokes
  updated in this PR (`window_bridge.h:5-10`).
- **NOT here**: dock-panel removal (e1), the D19 fan-in, CE #356.

## Definition of Done

- `session.control` → `SessionFeed` → daemon verbs covered end-to-end in the e08b chain's test
  family; `origin` echo suppression pinned.
- `simTick` covered on both sides (C++ reply shape + TS consumer); all existing session suites
  (`test_playbar_model.cpp`, `test_session_feed.cpp`, `editor-session-*`) stay green untouched.
- `data-play-state` writer tested across the full 3→5 mapping; unreachable states asserted
  unreachable (honest-degrade pinned, not skipped).
- Strip DOM/a11y per kit gates; hidden-on-welcome pinned; palette executes the `play.*` commands.
- All ten smokes green (`bridge.refused() == 0`); tests plant-verified both halves (R-QA-013);
  full 42-check CI green.
