---
id: e14d-update-daemon-banners
title: editor (14d) — update-notify banner (O3, no-telemetry) + daemon-lost reconnect banner surface
group: B
sequence: 11
repo: "."
base_branch: "main"
depends_on: [e14a-daemon-lifecycle-spine, e14c-welcome-screen]
importance: 6
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [07, 08]
split_from: e14-welcome-lifecycle   # owner ruling 2026-07-21
---

> **Split from [`e14-welcome-lifecycle.md`](e14-welcome-lifecycle.md)** (owner ruling 2026-07-21).
> Last of the e14 chain — **completing it closes e14**. Group B. Rides e14a (reconnect state) +
> e14c (welcome surface).

## Goal

The two notification banners: the **notify-only update banner** (O3 default) and the **daemon-lost
reconnect banner** — surfaced in the welcome screen + Settings.

## Scope & seams

- **Update banner** (O3 default — owner-confirmable at the gate): **notify-only** — an **HTTPS
  version GET against the latest published release** (⚠ NEW native infra — a boundary-clean HTTPS
  client), **NO identifiers, NO telemetry** (08 threat row); click-through to the downloads page;
  surfaced in welcome + Settings (the e06 Settings hook).
- **Daemon-lost banner**: the read-only reconnect banner UI over e14a's reconnect STATE (backoff /
  read-only until reattached).
- ⚠ **O3 is a confirm-or-amend gate** (notify-only vs auto-update) — the design default is
  notify-only; if the owner has not confirmed at dispatch, implement notify-only and flag it.
- Out of scope: lifecycle (e14a), arbitration (e14b), welcome layout (e14c).

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set.
2. Read CI before reviewing, even on a NORMAL entry.
3. A passing sibling only exonerates a suspected flake if that leg registers + runs the affected code.
4. Known flakes CE #319 / #322 + the e05d4 self-hosted-Windows CEF infra flake (env, not code).

## Definition of Done

- [ ] Update banner renders from a **mocked endpoint**; the request carries **NO identifiers** —
      assert on the request shape (08 threat row)
- [ ] Click-through opens the downloads page; banner surfaces in welcome + Settings
- [ ] Daemon-lost banner shows read-only + reconnect status over e14a's reconnect state
- [ ] `context_assert_shell_boundary` passes non-vacuously; FORBIDDEN list untouched
- [ ] Tests same PR (R-QA-013); 3-OS CI green
