---
id: e08-session-state-ui-bus
title: Selection/camera/play → daemon session state (editor verbs + session-topic extensions) + the editor.ui local bus (D7)
group: A
sequence: 4
repo: "."
base_branch: "main"
depends_on: [e02-client-sdk-boundary, e05-editor-core-foundation]
importance: 8
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [05, 01, 02]
status: superseded
superseded_by: [e08a-daemon-session-state, e08b-panel-state-rewiring, e08c-editor-ui-bus]
---

> ⛔ **SUPERSEDED → e08a / e08b / e08c** (decomposed 2026-07-22 under the owner's standing
> "continue + pre-screen" directive — pre-screened milestone-sized 2026-07-21, NOT dispatched, so no
> run was wasted). Six shippable DoD items across three subsystems: a new `editor` verb family + parity
> CI + session topic events + `.editor/session.json` persistence + scene-tree/playbar rewiring + the
> `editor.ui` cross-window bus. Sliced into:
> **[e08a](e08a-daemon-session-state.md)** daemon session state — `editor` verbs + `session` topic
> extensions + `origin` echo-suppression contract + `.editor/session.json` (group A, the spine) →
> **[e08b](e08b-panel-state-rewiring.md)** rewire scene tree + playbar + e07 when-context off private
> local state (group A) · **[e08c](e08c-editor-ui-bus.md)** the `editor.ui` bus (group **C** — it lives
> in `src/editor/webui/`, and it swaps the stub envelope e06b ships).
>
> ⚠ **DoD reassignment (TD ruling 2026-07-22, e09/e12 deferral precedent):** this spec's "propagates to
> a **second window**" clause needed the unbuilt multi-window subsystem — exactly the ambiguity that
> halted e09. The multi-CLIENT proof (CLI + scripted agent client) stays here in e08a; the
> **second-WINDOW** drill and the bus's cross-window mirror drill are reassigned to
> **[e10](e10-multiwindow-tearout.md)**. No child owns a DoD item that needs an unbuilt subsystem.
>
> Body preserved as origin-of-record; children carry the authoritative sliced DoD. Do NOT implement
> THIS file.

## Goal

Promote the semantic human state — selection, cameras, play state — into daemon session state
with contract verbs and topic events (agents can see what the human sees), and stand up the
editor-local `editor.ui` bus for UI chrome — the two-tier event model (D7).

## Scope & seams

- **`editor` verb namespace** (operational, `session_control` scope; additive under
  protocolMajor=1; registered via `MethodBackend` — the existing deterministic `session *`
  file-harness family stays untouched and distinct, C-F4): `editor select {ids[], mode}`
  (L-35 id-path keys, as panels already use — `scene_tree_panel.h:26-30`), `editor camera
  set {viewportId, transform, projection}`, `editor play|pause|stop|step` (REAL RPC play
  control), `editor selection get`, `editor cameras get`. One registry → R-CLI-013 parity CI
  covers CLI ≡ RPC ≡ MCP ≡ describe automatically.
- **`session` topic payload extensions** (additive — `registry.cpp:986-991` carries
  lifecycle only today): `selection-changed {ids, origin}`, `camera-changed {viewportId,
  origin}`, `play-state {…, origin}`; `origin` = client id for echo suppression.
- **Daemon persistence**: session state → `.editor/session.json` on clean shutdown
  (daemon-owned single writer — 03 §1 split); restore on next attach; corrupt file → renamed
  aside + defaults, loudly (07 §6).
- **Panel rewiring**: scene tree local selection seams (`scene_tree_panel.h:62-68`) and
  playbar (`playbar_model.h:83,102-117` — in-process `SessionControl*` today) become
  subscribers/writers of daemon session state; GUI panels lose private ownership.
- **`editor.ui` bus** (editor-core, mirrored across windows via Shell): envelope discipline
  mirrors the daemon stream (seq, topic, snapshot-on-subscribe); topics: `focus`, `layout`,
  `drag`, `viewport`, `theme-changed`, `palette`; facts only, never commands; NEVER forwarded
  to the daemon (D7). Package-panel access rides the `ui_events` capability (enforced fully
  in e13); custom package topics are namespaced + manifest-declared.
- e07's when-context providers switch from local stubs to this state.

## Definition of Done

- [ ] Registry parity CI green with the new verbs/topics (CLI, RPC, MCP, describe)
- [ ] T2: selection made in one window propagates to a second window, the CLI, and a
      scripted agent client; echo suppression verified (no feedback loops)
- [ ] Playbar drives daemon play state over RPC; play-state events observed by a second
      client; L-51 indicator fed
- [ ] session.json persists selection/camera on clean shutdown and restores; corrupt-file
      recovery loud + non-blocking (T1)
- [ ] `editor.ui` bus envelope/protocol T1 tests (seq, snapshot-on-subscribe); daemon
      never receives ui-chrome events (assert)
- [ ] 3-OS CI green
