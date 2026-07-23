# `src/editor/gui/playbar/` — M5-F5 play-in-editor playbar + runtime session-control

The **play-in-editor** transport (issue **#166**; `R-EDIT-001` / `R-PLAY-001/002/003/004` / **`L-51`** /
**`L-22`** / `R-A11Y-001` / `R-HUX-011`): a **play / pause / stop / step** playbar, with the running
scene rendered live into the **F1 viewport** (landed, PR #165). This was the **final M5 observer
fan-out leg** — completing it reached the M5 EXIT ("open → inspect → play → edit → viewport matches
web").

> ## ⚠ M9 e08b — the play state is the DAEMON's, and this directory is now TWO targets
>
> Until e08b the playbar drove an **in-process `SessionControl*`** and owned the resulting L-51 state
> in a private member. The daemon owns it now (`docs/editor-session-state.md`, design 05 §4 / D7 tier
> 1), so the transport became an **RPC writer** (`editor.play|pause|stop|step`, through the
> `PlayControlGateway` seam) and a **`play-state` subscriber** (`apply_play_state`), and the in-process
> path was **removed** rather than kept as a parallel truth. A CLI or a scripted agent driving play is
> therefore visible on the L-51 indicator with no local write at all.
>
> With the runtime-session dependency gone from `playbar_model.h`, the directory builds two targets:
>
> | target | sources | links | who links it |
> |---|---|---|---|
> | `context_gui_playbar` | `playbar_model` + `playbar_panel` | `context_gui_uitree` ONLY | the a11y harness **and the Shell**, which hosts the playbar since e08b |
> | `context_gui_playbar_session` | `session_control` | `context_render` + `context_session` | the runtime side + the M5 exit gates |
>
> The split is load-bearing, not tidy-minded: keeping one target would have forced the D10-audited
> `context_editor` closure to link the whole runtime session and render tier for a transport bar that
> uses neither.
>
> **What left with the in-process path.** `PlayFrame` observation and L-22 `hot_reload()` were
> SessionControl operations, not play-state transitions, and the daemon has no verb for either yet, so
> both stay on the `SessionControl` seam itself — still built, still tested (`test_session_control.cpp`
> now also carries the "play output reaches the F1 viewport with no second render path" proof), simply
> no longer reachable *through the panel*. Re-homing them onto the wire is later work (e09+).

## What this is (and is not)

- **Observer-grade play only.** The M5 T1 loop: start / pause / stop + rendered output + the L-51
  edit/play split + L-22 hot reload. **No** timeline / debugger / profiler UI, and no
  authoring-during-play beyond L-51 live-edit (all later milestones).
- **Headless + CEF-free.** Like the sibling panels, `context_gui_playbar` is pure C++ over the F0b
  headless libs (`context_gui_uitree`), the runtime session (`context_session`), and the render extract
  (`context_render`). It builds + unit-tests on the **default 3-OS matrix** and the local dev gate; it
  never boots CEF and never links the CI-only GPU backend. The session-control + hot-reload logic is
  covered headless by `context_gui_uitree` + **runtime-session fault-injection** (no GPU required).

## The three halves

1. **The session-control seam (`session_control.h`, `context_gui_playbar_session`).** `SessionControl`
   is the runtime-session abstraction — `start` / `step` / `discard` / `apply_hot_reload` — mirroring
   the inspector's `OverrideWriteGateway`. **Since e08b the playbar does not drive it**; it is the
   runtime side of play. `RuntimeSessionControl` implements it for real over a
   `runtime::session::Session` and extracts the render frame the F1 viewport observes
   (`render::extract_render_world`, the L-39 one-way sim→render extract). The rendered play output the
   seam yields **is** a `render::RenderSnapshot` — the SAME type `viewport::ViewportPanel::set_snapshot`
   consumes, so play output flows through the F1 viewport with **no second render path**.

2. **The transport (`playbar_model.*`, `context_gui_playbar`).** The L-51 edit/play provenance state
   (`edit` / `playing` / `paused`) as the DAEMON reports it, the four transport WRITES over the
   `PlayControlGateway` seam, the `apply_play_state` subscriber another client's transition arrives
   through, and the **R-HUX-011** control-loop listener (the seam the CEF host times input→paint
   latency around, like the viewport's view-update loop). Total; every action reports success or a
   reserved `play.*` code — propagated from the daemon, verbatim. One deliberate refinement at the
   reply boundary: an R-CLI-008 envelope cannot express "ok=false with no code", so a benign daemon
   no-op answers `ok=true, changed=false` and `PlayAction::ok` is fed from `changed` (losslessly —
   `ok` here always meant "something actually happened").

3. **The panel (`playbar_panel.*`).** Projects the model into a headless `uitree::Panel`: the **L-51
   loud play-mode indicator** (the running session's runtime state is *discarded on stop, never written
   to files*), a status line (state + simTick + whether a live daemon session exists + any `play.*`
   error), and the four keyboard-reachable transport controls — so the whole play loop has a complete
   keyboard path. Its `state` vocabulary is byte-identical to the daemon's `play-state` tokens, so the
   indicator is fed straight off the topic with no translation layer to drift.

## L-51 edit/play split · L-22 hot reload

- **L-51.** `start()` runs the session over a **session view** — the authored files are never written;
  runtime mutations are session state `discard()`ed on stop. The panel surfaces this loudly so
  live-edit-during-play can never be mistaken for authoring (made impossible by construction under file
  authority).
- **L-22.** `hot_reload()` reflects a live authored edit (an F3 inspector override write) into the
  **running** session: a **data-value** edit is *live-preserving* (state kept); a component-schema
  **shape / `x-ctx-storage`** change is *restart-class* (state discarded + re-instantiated from the new
  derivation, a loud event; `R-PLAY-003`). In-place migration of live archetype storage is explicitly
  not attempted in v1.

## Reserved `play.*` error-domain block (this task mints it)

The wave's single code-minter (`src/editor/contract/src/error_catalog.cpp`, append-only tail). The
strings are owned here as constants (`playbar_model.h`, the promote-a-local-string pattern) and
registered in the catalog:

| code | class | when |
|---|---|---|
| `play.not_running` | usage | a control (pause / step / hot-reload) issued with no live play session (L-51 edit state) |
| `play.session_failed` | internal | the play session could not be started over the edit state (fail-closed; no file written) |
| `play.step_failed` | internal | advancing the running session by a fixed tick failed (R-SIM-002 fail-closed) |
| `play.hot_reload_failed` | validation | a live authored edit could not be reflected into the running session (L-22) |

## a11y

Registered with the M5-F6 harness (`a11y/registry.cpp` + `a11y/coverage.manifest.jsonl`, id
`builtin.playbar`) so it is scanned on the default 3-OS matrix and the `editor-cef-smoke` Linux
enforcement gate. `tests/test_a11y.cpp` additionally asserts zero violations + a complete keyboard path
across every play state.

## Tests (R-QA-013, same PR)

- `tests/test_playbar_model.cpp` — the transport over a `PlayControlGateway` double that reproduces the
  daemon's reply shapes exactly (a real change / a benign `changed=false` no-op / a `play.not_running`
  refusal): the L-51 transitions over RPC, the `play-state` subscriber half, the ok←changed mapping,
  the no-gateway posture, and the R-HUX-011 control-loop listener + generation counter.
- `tests/test_session_control.cpp` — the REAL `RuntimeSessionControl` adapter over a live
  `runtime::session::Session`: stepping advances the fixed-tick simulation, determinism sanity, the
  L-51/L-22 semantics on the real session, and (since e08b, at its new home) the cross-panel proof that
  the produced play frame flows through the **F1 viewport** with no second render path.
- `tests/test_playbar_panel.cpp` — the headless projection, the loud play-mode indicator fed from
  daemon state, deterministic `render_html`, and the L-51 token vocabulary.
- `tests/test_a11y.cpp` — the per-panel a11y + keyboard-nav gate across play states.
- **Cross-process**: `editor-session-panels-t2` (`src/tests/integration/`) drives this transport
  against a REAL `context daemon` with the real `context` binary as a second client — the only place
  the `origin` echo-suppression contract can actually be checked.
