---
id: e14c-welcome-screen
title: editor (14c) — welcome screen (D13): recent projects + native folder picker + new-from-template
group: B
sequence: 10
repo: "."
base_branch: "main"
depends_on: [e14a-daemon-lifecycle-spine]
importance: 7
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [07, 10]
split_from: e14-welcome-lifecycle   # owner ruling 2026-07-21
---

> **Split from [`e14-welcome-lifecycle.md`](e14-welcome-lifecycle.md)** (owner ruling 2026-07-21).
> Rides the [`e14a`](e14a-daemon-lifecycle-spine.md) launch flow (recent/open/new all resolve into
> attach-or-spawn). Group B. Packaged-shape step-count drill → e15/e16.

## Goal

The app's front door (D13, window 0; no full launcher — R-HUX-003 stays v2): **recent projects**,
**Open project…**, **New from template** — feeding the e14a launch flow.

## Scope & seams

- **Recent projects** from `~/.context/config.json`; **"Open project…"** = a **native folder picker
  via the Shell** (⚠ NEW native infra — a boundary-clean Shell picker, not a kernel dependency);
  **"New from template"** = a thin wrapper over `context new` (R-QA-006 templates).
- 🚫 **NO signature flourish on the welcome CTA** (O1 RESOLVED 2026-07-19). The owner rejected the
  aurora and picked **"Pulse of Work"**, which is *state-linked* and meaningful ONLY on the
  Play/active-action button — the welcome CTA has no such state, so it uses ordinary primary-button
  styling. Do NOT build the aurora or any flourish here. Spec: `../mockups/TOKENS.md` §5.
- Welcome UI is editor-core TS (hydrated like other panels); the folder picker is the Shell seam.
- Out of scope: lifecycle (e14a), arbitration/file-assoc (e14b), banners (e14d).

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set.
2. Read CI before reviewing, even on a NORMAL entry.
3. A passing sibling only exonerates a suspected flake if that leg registers + runs the affected code.
4. Known flakes CE #319 / #322 + the e05d4 self-hosted-Windows CEF infra flake (env, not code).

## Definition of Done

- [ ] Bare launch shows welcome; open-recent = 2 steps; new-from-template flow ≤7 steps total per
      persona A budgets (dev-mode T2 script; packaged-shape count is e15/e16)
- [ ] "Open project…" uses a native folder picker via a boundary-clean Shell seam
- [ ] Welcome CTA carries ordinary primary styling — NO flourish (O1)
- [ ] `context_assert_shell_boundary` passes non-vacuously; FORBIDDEN list untouched
- [ ] Tests same PR (R-QA-013); 3-OS CI green
