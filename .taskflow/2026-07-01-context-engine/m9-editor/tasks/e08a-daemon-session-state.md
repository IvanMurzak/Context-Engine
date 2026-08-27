---
id: e08a-daemon-session-state
title: editor (08a) — `editor` verb namespace + `session` topic payload extensions + `.editor/session.json` persistence + parity CI
group: A
sequence: 5
repo: "."
base_branch: "main"
depends_on: [e02-client-sdk-boundary, e05d4-t2-boot-dock-restore-smoke]
importance: 8
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [05, 01, 02]
split_from: e08-session-state-ui-bus   # TD decomposition 2026-07-22, off the owner's pre-screen directive
---

> **Split from [`e08-session-state-ui-bus.md`](e08-session-state-ui-bus.md)** (decomposition 2026-07-22,
> from the owner's standing "continue + pre-screen" directive; e08 was pre-screened milestone-sized
> 2026-07-21 — 6 shippable DoD items across three subsystems). Serial group-A chain
> **e08a → e08b**, plus **e08c** in group C. **This is the spine**: the daemon-side state + contract
> surface that e08b's panels and e07's when-context providers consume.

## Goal

Promote the semantic human state — **selection, cameras, play state** — into **daemon** session state
with real contract verbs, topic events, and crash-safe persistence, so any client (a second window,
the CLI, a scripted agent) can see and drive what the human sees (D7, tier 1 of the two-tier model).

## Scope & seams

- **`editor` verb namespace** (operational, `session_control` scope; additive under `protocolMajor`;
  registered via `MethodBackend` — the existing deterministic `session *` file-harness family stays
  untouched and distinct, C-F4): `editor select {ids[], mode}` (L-35 id-path keys, as the panels
  already use — `scene_tree_panel.h:26-30`), `editor camera set {viewportId, transform, projection}`,
  `editor play|pause|stop|step` (REAL RPC play control), `editor selection get`, `editor cameras get`.
  ONE registry → R-CLI-013 parity CI covers CLI ≡ RPC ≡ MCP ≡ describe automatically.
- **`session` topic payload extensions** (additive — `registry.cpp:986-991` carries lifecycle only
  today): `selection-changed {ids, origin}`, `camera-changed {viewportId, origin}`,
  `play-state {…, origin}`. **`origin` = client id, and it is the echo-suppression contract** — define
  and test it here, because e08b's panels and e08c's bus both depend on it being trustworthy.
- **Daemon persistence**: session state → `.editor/session.json` on clean shutdown (**daemon-owned
  single writer** — the 03 §1 split; NOT the Shell, which owns `config.json`/layout); restore on next
  attach; a corrupt file is renamed aside + defaults loaded, **loudly** (07 §6).
- **Multi-CLIENT proof is in scope; multi-WINDOW is NOT.** Prove propagation + echo suppression across
  a second *client* (the CLI and a scripted agent client) — that is fully reachable today. The
  "second **window**" half of the original e08 DoD is **reassigned to [`e10`](e10-multiwindow-tearout.md)**
  (TD ruling 2026-07-22, following the e09/e12 deferral precedent: no task may own a DoD item that
  needs an unbuilt subsystem). e10 must assert cross-window selection sync on top of this state.
- Out of scope: panel rewiring (e08b), the `editor.ui` bus (e08c), writes/undo (e09).

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set.
2. Read CI before reviewing, even on a NORMAL entry.
3. A passing sibling only exonerates a suspected flake if that leg registers + runs the affected code.
4. Known flakes CE #319 / #322 / #335 + the self-hosted-Windows CEF env flake family (a JOB_STARTED
   orphan-reaper hook was activated 2026-07-22 — report a surviving `post-build.bat` CEF-locales COPY
   failure explicitly).

## Definition of Done

- [ ] Registry **parity CI green** with the new `editor` verbs + extended `session` topics
      (CLI ≡ RPC ≡ MCP ≡ describe); the deterministic `session *` file-harness family provably untouched
- [ ] T2: a selection/camera/play change propagates to **a second client** (the CLI **and** a scripted
      agent client); **echo suppression verified via `origin`** — no feedback loops
- [ ] `editor play|pause|stop|step` drive daemon play state over RPC; play-state events observed by a
      second client; the L-51 indicator is fed
- [ ] `.editor/session.json` persists selection/camera on clean shutdown and restores on next attach;
      **corrupt-file recovery is loud + non-blocking** (T1), daemon is the single writer
- [ ] Tests same PR (R-QA-013); 3-OS CI green
