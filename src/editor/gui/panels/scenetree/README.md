# `src/editor/gui/panels/scenetree/` — M5-F2: the scene-tree observer panel

The observer editor's **scene-tree panel** (`context_gui_panel_scenetree`, **R-EDIT-001** /
**R-BRIDGE-008** / **L-35** / **R-A11Y-001** / **R-HUX-011**, issue #155): it renders the **composed
derived-world hierarchy** live, drives **selection**, and re-renders on the R-BRIDGE-008
`derivation.settled` event. **Read-only** — an observer projection of the derived world: no writes,
**no new error-catalog codes**.

Built ON the F0b headless substrate (`../../uitree/`, `../../contract/`) — like every M5 panel it is
CI-assertable **without booting CEF** (R-EDIT-001 testable-by-construction editor). The primary CI
assertion is the headless unit path below on the default 3-OS `build` matrix.

## Two pieces

- **`scene_tree_model.h` — the derived-world view model.** BOUNDARY-CLEAN since M9 e05d3 (owner
  ruling 2026-07-20): plain data only, no `compose::` type — which is what makes this library
  Shell-hostable under the D10 shell-boundary gate. The kernel-typed builder `build_scene_tree(const
  compose::ComposedScene&)` now lives in `../builders/` (`context_gui_panel_builders`, the daemon
  side of the wire); it weaves the FLAT composed entities (each keyed by its **L-35 id-path**
  `[instanceId, …, entityId]`) into a nested `SceneTreeNode` hierarchy:
  - an id-path **prefix with no composed entity of its own** becomes a synthetic **instance boundary**
    (`NodeKind::instance`) — the sub-scene root the instance composes under, so **instances are
    visible** in the tree;
  - an entity whose `field_provenance` carries an **override contributor** is marked `overridden`, so
    **overrides are visible** (L-35), not hidden;
  - `identity` (the id-path joined with `/`) is the stable selection key; `identity_hash` is the L-37
    composed identity hash. Deterministic — node order is the composed entities' expansion order.
- **`scene_tree_panel.h` — the panel.** `SceneTreePanel` holds the model + the current selection +
  the tracked generation/stability, and:
  - `build_panel()` projects the model into a headless `uitree::Panel` (a `tree` of focusable
    `treeitem`s) — **a11y-conformant by construction** (`uitree::audit_a11y` returns no violations)
    and **deterministic** (identical state → byte-identical `render_html`, so re-render is stable);
  - `select()` / `clear_selection()` drive selection and **emit a `SceneSelection` event** every
    registered listener (other panels) consumes — the R-HUX-011 selection loop; every row is a
    focusable, command-bound `treeitem`, so selection has a complete keyboard path (R-A11Y-001 /
    R-CLI-001);
  - `on_derivation_settled(generation, stability)` consumes the **R-BRIDGE-008** quiescence event —
    advancing the generation and recording the `stability` (`stable` / `settling` / `unstable`) the
    status line reflects, so a settling world is visibly distinguished from a stable one;
  - `set_model()` refreshes the tree from a fresh snapshot, **preserving the selection** when its
    identity survives and clearing (notifying) it otherwise.

The panel consumes the bridge query surface + `derivation.settled` events at the seam
(`set_model` from a snapshot, `on_derivation_settled` on the event); wiring it to a live
`bridge::EventStream` subscription + the R-HUX-011 instrumented input→paint latency measurement is
the CEF-host integration path (a trailing M5 surface), out of this headless panel's scope.

## Building + testing (default `dev` gate — no CEF)

```bash
cmake -S src --preset dev && cd src && cmake --build --preset dev
ctest --preset dev -R "^gui-panel-scenetree-"   # model / panel / a11y
```

- `gui-panel-scenetree-test_scene_tree_model` — the tree builder: hand-built + **real `flatten()`**
  fixtures; instance boundaries synthesized, overrides marked, deterministic order, name fallback.
- `gui-panel-scenetree-test_scene_tree_panel` — selection events, R-BRIDGE-008 settle/stability,
  stable re-render, selection preservation/clearing across a snapshot refresh.
- `gui-panel-scenetree-test_a11y` — the **per-panel a11y scan + keyboard-only navigation
  assertion** (R-A11Y-001) across empty / populated / deep / overridden worlds. Registered for the
  M5-F6 a11y harness in `../../a11y/coverage/scenetree.json` (a defensive per-panel fragment the
  harness reconciles via append-only union-merge).

All three run inside the default `build` job's `ctest --preset dev` on ubuntu/macOS/windows.
