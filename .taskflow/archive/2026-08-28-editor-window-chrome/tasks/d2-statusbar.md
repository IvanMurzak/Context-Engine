---
id: "d2-statusbar"
title: "Statusbar content: daemon link state, problems count, theme/project identity"
group: "D"
sequence: 2
repo: "."
base_branch: "main"
depends_on: ["a2-strips-scaffold"]
importance: 6
complexity: 5
security_critical: false
production_touching: false
model_hint: "mid"
taskflow_refs: ["02-target-architecture.md"]
---

## Goal

Fill a2's 24px statusbar shell with the v1 content that already has a truthful source: daemon link
state, problems count, and active theme + project identity. Nothing speculative — anything else
waits for its source (02 §8).

## Scope & seams

- **Daemon link state**: the `daemon.linkState` read the banners already consume
  (`banners.ts:33-36`) — a status dot + label in the strip.
- **Problems count**: the Problems panel model's existing diagnostics feed (editor-core-internal
  source; no new bridge surface).
- **Theme / project identity**: the existing theme state and project name (the same sources the
  titlebar uses).
- Strip content follows the chrome pattern: styled in `app.css` from existing tokens, controls
  are kit components, no new kit family, no new tokens.
- **No new bridge surface expected.** If one turns out to be necessary, the ten-smoke rule
  applies (`window_bridge.h:5-10`) and the task pays it in the same PR.

## Definition of Done

- Each field's rendering AND update path covered by webui tests: link-state transitions, problems
  count changing with the diagnostics feed, theme flip, project name in welcome vs project mode.
- ts-a11y labels/roles per the house pattern (banners worked example).
- Kit/token gates hold; CEF smoke coverage expectations unchanged (the shell already existed from
  a2 — content must not move layout).
- Tests plant-verified both halves (R-QA-013); full 42-check CI green.
