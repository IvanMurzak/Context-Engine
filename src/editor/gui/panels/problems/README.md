# `src/editor/gui/panels/problems/` — M5-F4: the Problems observer panel

The observer editor's **Problems panel** (`context_gui_panel_problems`, **R-HUX-005** /
**R-FILE-003** / **R-BRIDGE-008** / **R-A11Y-001** / **R-HUX-011**, issue #158): it renders the
**R-FILE-003 structured diagnostics** (code + message + a JSON-pointer / line-column navigation
target) as a **list grouped by file** plus the **inline markers** editors draw, drives
**click-to-navigate** to the offending source, and respects the **R-BRIDGE-008 `stability` field**
(provisional diagnostics are visually distinguished from stable ones). **Read-only** — an observer
projection of the diagnostics/event stream: no writes, **no new error-catalog codes**.

Built ON the F0b headless substrate (`../../uitree/`) + the R-BRIDGE-008 stability enum
(`../../../bridge/`) — like every M5 panel it is CI-assertable **without booting CEF**
(R-EDIT-001 testable-by-construction editor). The primary CI assertion is the headless unit path below
on the default 3-OS `build` matrix.

## Two pieces

- **`problems_model.h` — the diagnostics view model.** The panel-facing projection of an R-FILE-003
  diagnostic (`ProblemDiagnostic`: `code`, `message`, `Severity`, a `NavTarget` = file + JSON-pointer
  + line/column, the R-BRIDGE-008 `stability`, and the derived-world `generation` stamp). Free
  functions weave a flat diagnostic list into the rendered view:
  - `build_problems_model()` groups diagnostics by file (first-seen file order), stable-sorts each
    group **worst-severity-first**, collapses duplicate identities (**last wins** — the promotion
    path), and fills the total / per-severity / provisional **counts** the status line surfaces;
  - `build_inline_markers()` derives one **inline marker** per navigable diagnostic from the SAME set,
    so the list and the editor squiggles never diverge;
  - `is_provisional()` projects the three-state R-BRIDGE-008 `Stability` (`stable` / `settling` /
    `unstable`) onto the R-FILE-003 **provisional vs stable** distinction R-HUX-005 draws (provisional
    = anything not `stable`) — reusing the canonical bridge enum rather than minting a new field.
- **`problems_panel.h` — the panel.** `ProblemsPanel` holds the current diagnostic set + the current
  navigation + the tracked generation/stability, and:
  - `build_panel()` projects the model into a headless `uitree::Panel` (a `list` of file `group`s of
    focusable, command-bound `listitem`s) — **a11y-conformant by construction** (`uitree::audit_a11y`
    returns no violations) and **deterministic** (identical state → byte-identical `render_html`);
  - `navigate()` / `clear_navigation()` drive **click-to-navigate** and **emit a `ProblemNavigation`
    event** every registered listener (the relevant editors) consumes — the R-HUX-005 / R-HUX-011
    navigation loop; a non-navigable diagnostic (no file) cannot be navigated to, and every navigable
    row is a focusable, command-bound `listitem`, so navigation has a complete keyboard path
    (R-A11Y-001 / R-CLI-001);
  - `on_derivation_settled(generation, stability)` consumes the **R-BRIDGE-008** quiescence event —
    it **discards stale** provisional diagnostics stamped with an OLDER generation (a settling pass
    superseded them) and **promotes** provisional diagnostics stamped with the settled generation to
    `stable` (R-FILE-003 provisional→stable), recording the generation/stability the status line
    reflects; a navigation whose diagnostic was discarded is cleared (notifying);
  - `set_diagnostics()` refreshes the set from a fresh snapshot, **preserving the navigation** when
    its diagnostic identity survives and clearing (notifying) it otherwise; `ingest()` merges one
    `diagnostics`-topic delta, replacing an existing diagnostic in place (the promotion path) or
    appending a new one.

The panel consumes the bridge diagnostics surface + `derivation.settled` events at the seam
(`set_diagnostics`/`ingest` from the stream, `on_derivation_settled` on the event); wiring it to a
live `bridge::EventStream` subscription + the CEF editors that draw the inline markers and act on the
navigation event is the CEF-host integration path (a trailing M5 surface), out of this headless
panel's scope.

## Building + testing (default `dev` gate — no CEF)

```bash
cmake -S src --preset dev && cd src && cmake --build --preset dev
ctest --preset dev -R "^gui-panel-problems-"   # model / panel / a11y
```

- `gui-panel-problems-test_problems_model` — grouping, worst-severity-first ordering, counts,
  identity dedup/promotion, navigation-target display, and inline-marker derivation.
- `gui-panel-problems-test_problems_panel` — click-to-navigate events, R-BRIDGE-008 provisional→stable
  promotion + stale-marker discard on `derivation.settled`, stable re-render, navigation
  preservation/clearing across a snapshot refresh, provisional marker visibility.
- `gui-panel-problems-test_a11y` — the **per-panel a11y scan + keyboard-only navigation assertion**
  (R-A11Y-001) across empty / navigable / provisional / grouped / non-navigable diagnostic sets.
  Registered for the M5-F6 a11y harness in `../../a11y/coverage/problems.json` (a defensive per-panel
  fragment the harness reconciles via append-only union-merge).

All three run inside the default `build` job's `ctest --preset dev` on ubuntu/macOS/windows.
