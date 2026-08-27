---
id: e14a-daemon-lifecycle-spine
title: shell (14a) — daemon lifecycle spine (D18): spawn-or-attach + stdio token + exit policy + reconnect
group: B
sequence: 8
repo: "."
base_branch: "main"
depends_on: [e05d4-t2-boot-dock-restore-smoke, e02-client-sdk-boundary]
importance: 8
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [07, 02]
split_from: e14-welcome-lifecycle   # owner ruling 2026-07-21
---

> **Split from [`e14-welcome-lifecycle.md`](e14-welcome-lifecycle.md)** (owner ruling 2026-07-21).
> e14 halted `scope_exceeds_single_pass` (5 subsystems + net-new native infra). Serial chain
> e14a→{e14b,e14c,e14d}, all group B. **This is the spine** — the process model the other three ride
> on. The **packaged-shape** versions of the T2 drills are reassigned to **e15/e16** (owner ruling);
> this task proves them in dev-mode.

## Goal

Stand up the app's process model (D18): **spawn-or-attach** the daemon with an honest **exit policy**
and a **reconnect** path — the spine `e14b`/`e14c`/`e14d` build on.

## Scope & seams

- **Launch → resolve → attach-or-spawn** (07 §4): resolve project → read `.editor/instance.json` →
  live daemon? **attach** : **spawn the daemon as a CHILD**. ⚠ **The D10-clean long-running-child
  spawn primitive is NEW native infra** — put it in a boundary-clean module (`context_common` / the
  Shell), **NOT** the FORBIDDEN `context_import` or any kernel-internal target. The attach **token
  goes over a stdio pipe — NEVER argv/env** (05 §2; 08 threat model).
- **Exit policy**: the daemon survives editor exit ONLY if other clients hold attachments, else a
  clean `shutdown` verb; a pre-existing EXTERNAL daemon is **attached-to, NEVER owned**.
- **Reconnect** (03 §7): daemon lost → reconnect with backoff; **read-only mode** until reattached;
  the e02 subscription consumer re-snapshots on reattach. (The banner UI surface is e14d; the
  lifecycle/backoff/read-only STATE is here.)
- Out of scope (deliberately): welcome screen (e14c), second-project arbitration + file-assoc
  (e14b), update banner (e14d). Packaged-shape drills: e15/e16.

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set — enumerate consumers from code.
2. Read CI before reviewing, even on a NORMAL entry.
3. A passing sibling only exonerates a suspected flake if that leg registers + runs the affected code.
4. Known flakes CE #319 / #322 + the e05d4 self-hosted-Windows CEF infra flake (env, not code —
   `post-build.bat` COPY / `DCompositionCreateDevice3` access-denied = rerun/gate, not a 02 loop).

## Definition of Done

- [ ] Spawn-vs-attach both work; the attach token travels via **stdio (never argv/env)** — asserted
- [ ] Exit policy asserted (an attached CLI keeps the daemon alive; last-client exit shuts it down;
      an external daemon is attached, never shut down)
- [ ] Daemon-lost → **read-only** state → auto-reattach drill green (dev-mode T2; the packaged-shape
      version is e15/e16)
- [ ] The child-spawn primitive lives in a boundary-clean module; `context_assert_shell_boundary`
      passes **non-vacuously** with its FORBIDDEN list untouched
- [ ] Every behavior change ships WITH its tests same PR (R-QA-013); 3-OS CI green
