---
id: e14-welcome-lifecycle
title: Welcome screen (D13) + daemon lifecycle (D18) + file association + update-notify banner + second-project arbitration (D15)
group: B
sequence: 7
repo: "."
base_branch: "main"
depends_on: [e05-editor-core-foundation]
importance: 7
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [07, 02, 05]
status: superseded
superseded_by: [e14a-daemon-lifecycle-spine, e14b-arbitration-file-assoc, e14c-welcome-screen, e14d-update-daemon-banners]
---

> ⛔ **SUPERSEDED → e14a / e14b / e14c / e14d** (owner ruling 2026-07-21). This task halted at
> `02-implement` **before any code** with `scope_exceeds_single_pass`: it bundles 5 independently-
> shippable subsystems + net-new native infra (a D10-clean long-running-child spawn primitive, a
> native folder picker, an HTTPS update client). Decomposed into a serial chain (all group B):
> **[e14a](e14a-daemon-lifecycle-spine.md)** daemon lifecycle spine (D18: spawn-or-attach + stdio
> token + exit policy + reconnect) → **[e14b](e14b-arbitration-file-assoc.md)** second-project
> arbitration + presence marker + `context edit .`/file-assoc (D15/C-F23) · **[e14c](e14c-welcome-screen.md)**
> welcome screen (D13: recent + folder picker + new-from-template) · **[e14d](e14d-update-daemon-banners.md)**
> update-notify + daemon-lost banners (O3). **The "T2 packaged-shape" DoD drills are REASSIGNED to
> e15/e16** (owner ruling — they overlap packaging; doc 07 §2); the e14* children prove the functional
> flows in dev-mode. This spec's Goal/Scope/DoD below are preserved verbatim as origin-of-record; the
> children carry the authoritative, sliced DoD. Do NOT implement THIS file.

## Goal

Complete the app's front door and process model: the mini-welcome screen for bare launch, the
spawn-or-attach daemon lifecycle with honest exit policy, project file association, the
notify-only update banner (O3 default), and per-project single-instance arbitration (C-F23).

## Scope & seams

- **Welcome screen** (D13, window 0; no full launcher — R-HUX-003 stays v2): recent projects
  (from `~/.context/config.json`), "Open project…" (native folder picker via Shell), "New
  from template" (thin wrapper over `context new`, R-QA-006 templates). ⚠ **O1 RESOLVED
  2026-07-19 — NO aurora anywhere, and NO signature flourish on the welcome CTA.** The owner
  rejected the aurora outright and picked **"Pulse of Work"**, a *state-linked* glow that is
  meaningful only on the Play/active-action button (its colour + rhythm track idle/compiling/
  running/error/paused). The welcome CTA has no such state, so it carries the ordinary primary-
  button styling — no flourish. Spec: [`../mockups/TOKENS.md`](../mockups/TOKENS.md) §5; ledger:
  [`../ROADMAP.md`](../ROADMAP.md).
- **Launch with project** (07 §4): resolve project (`context edit .` / file association /
  "Open with") → read `.editor/instance.json` → live daemon? attach : spawn daemon as CHILD
  (token via stdio pipe — 05 §2); window 0 restores layout.
- **Exit policy**: daemon survives editor exit only if other clients hold attachments,
  else clean `shutdown` verb; a pre-existing external daemon is attached to, NEVER owned.
- **Reconnect UX** (03 §7): daemon lost → reconnect with backoff; read-only banner until
  reattached; subscription consumer re-snapshots (e02 machinery).
- **Second project (D15/C-F23)**: opening a different project spawns a NEW process;
  per-project single-instance arbitrated via the editor presence marker in
  `.editor/editor-state.json` — the opener FOCUSES the existing process instead of
  duplicating.
- **File association**: project-marker association declared by the app (installer-side
  registration is e15's; the handler + `context edit .` path land here).
- **Update banner** (O3 default, owner-confirmable at the gate): notify-only — HTTPS version
  GET against latest published release, NO identifiers/telemetry (08 threat row);
  click-through to downloads; surfaced in welcome + Settings (e06 hook).

## Definition of Done

- [ ] Bare launch shows welcome; open-recent = 2 steps; new-from-template flow ≤7 steps
      total per persona A budgets (counted in T2 script)
- [ ] Spawn-vs-attach both work; token via stdio (never argv/env); exit policy asserted
      (attached CLI keeps daemon alive; last client exit shuts it down)
- [ ] Duplicate open of the same project focuses the existing process (T2, two-process
      drill); different project spawns a second process
- [ ] Daemon-lost → read-only banner → auto-reattach drill green (T2)
- [ ] Update banner renders from a mocked endpoint; NO request identifiers (assert on the
      request shape)
- [ ] 3-OS CI green
