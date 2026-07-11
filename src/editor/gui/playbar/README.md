# `src/editor/gui/playbar/` — M5-F5 play-in-editor playbar + runtime session-control

The **play-in-editor** transport (issue **#166**; `R-EDIT-001` / `R-PLAY-001/002/003/004` / **`L-51`** /
**`L-22`** / `R-A11Y-001` / `R-HUX-011`): a **play / pause / stop / step** playbar that drives
**session-control** over `src/runtime/session/`, with the running scene rendered live into the **F1
viewport** (landed, PR #165). This is the **final M5 observer fan-out leg** — completing it reaches the
M5 EXIT ("open → inspect → play → edit → viewport matches web").

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

1. **The session-control seam (`session_control.h`).** `SessionControl` is the abstraction the playbar
   drives — `start` / `step` / `discard` / `apply_hot_reload` — mirroring the inspector's
   `OverrideWriteGateway`. `RuntimeSessionControl` implements it for real over a
   `runtime::session::Session` and extracts the render frame the F1 viewport observes
   (`render::extract_render_world`, the L-39 one-way sim→render extract). The rendered play output the
   seam yields **is** a `render::RenderSnapshot` — the SAME type `viewport::ViewportPanel::set_snapshot`
   consumes, so play output flows through the F1 viewport with **no second render path**.

2. **The state machine (`playbar_model.*`).** The L-51 edit/play provenance state (`edit` /`playing` /
   `paused`), the transport transitions (play / resume / pause / stop / step), the L-22 hot-reload
   classification, and the **R-HUX-011** control-loop listener (the seam the CEF host times input→paint
   latency around, like the viewport's view-update loop). Total; every action reports success or a
   reserved `play.*` code.

3. **The panel (`playbar_panel.*`).** Projects the model into a headless `uitree::Panel`: the **L-51
   loud play-mode indicator** (the running session's runtime state is *discarded on stop, never written
   to files*), a status line (state + simTick + observed-frame summary + any `play.*` error), and the
   four keyboard-reachable transport controls — so the whole play loop has a complete keyboard path.

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

- `tests/test_playbar_model.cpp` — the state machine over a fault-injecting `SessionControl` double: the
  L-51 transitions, the L-22 reload classification, every reserved `play.*` code, and the R-HUX-011
  control-loop listener + generation counter.
- `tests/test_session_control.cpp` — the REAL `RuntimeSessionControl` adapter over a live
  `runtime::session::Session`: stepping advances the fixed-tick simulation, determinism sanity, and the
  L-51/L-22 semantics on the real session.
- `tests/test_playbar_panel.cpp` — the headless projection, the loud play-mode indicator, deterministic
  `render_html`, and the cross-panel proof that the play frame flows through the **F1 viewport** with no
  second render path.
- `tests/test_a11y.cpp` — the per-panel a11y + keyboard-nav gate across play states.
