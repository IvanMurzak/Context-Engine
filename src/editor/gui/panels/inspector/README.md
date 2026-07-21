# `src/editor/gui/panels/inspector/` — M5-F3: the inspector panel

The observer editor's **inspector panel** (`context_gui_panel_inspector`, **R-EDIT-001** /
**R-DATA-002** / **L-35** / **L-20** / **L-30** / **R-CLI-005** / **R-A11Y-001** / **R-HUX-011**,
issue #160): it renders the **selected entity's component schemas** as editable widgets and commits
edits as **L-35 override writes** through the EXISTING `context set` write path — the M5 authoring
surface (inspector-override edits only; in-viewport override editing is deferred to M8.5).

Built ON the F0b headless substrate (`../../uitree/`, `../../contract/`) + the M2 compose write path
(`context set` / `compose::plan_write`) + the schema introspection surface — like every M5 panel it is
CI-assertable **without booting CEF**. It reuses the existing error-catalog codes (`cas.mismatch`,
`compose.write_target_not_found`, `compose.immutable_pointer`) — **no new error-catalog codes**.

## Two pieces

- **`inspector_model.h` — the schema-driven editable projection.** BOUNDARY-CLEAN since M9 e05d3
  (owner ruling 2026-07-20): the model carries plain data + `serializer::JsonValue` only — no
  `compose::`/`schema::` type — which is what makes this library Shell-hostable under the D10
  shell-boundary gate. The kernel-typed builders below now live in `../builders/`
  (`context_gui_panel_builders`, the daemon side of the wire). `build_inspector_model(entity,
  kindSchema, rootScene)` derives the entity's editable fields from its kind schema's **R-CLI-005
  introspection** (`schema::introspection_json`) intersected with the composed value:
  - each **present component leaf** becomes an `InspectorField` carrying its declared `WidgetKind`
    (text / number / toggle / json / readonly), `units` (x-ctx-units), and current composed `value`;
  - a field an **L-35 override contributor** touched is marked `overridden`, so overrides are visible
    in the inspector, not hidden;
  - the **immutable identity fields** (`/id`, `/$schema`, `/version` — L-37) and the L-32 `notes`
    annotation fields are excluded from the editable set.
  - `builders::override_write_request(model, pointer, value)` builds the **override-write
    envelope**: a `compose::WriteRequest` targeting the **outermost** instancing scene (L-35
    default), addressed by
    the entity's id-path — the exact request the `context set` write path consumes (single source of
    truth; no parallel writer).
- **`inspector_panel.h` — the panel.** `InspectorPanel` holds the model + the in-flight gesture + the
  commit gateway, and:
  - `build_panel()` projects the model into a headless `uitree::Panel` — one focusable, command-bound
    widget per editable field (`textbox` / `checkbox`), **a11y-conformant by construction**
    (`uitree::audit_a11y` returns no violations) and **deterministic** (identical state →
    byte-identical `render_html`);
  - `stage_edit()` records an in-flight gesture; `commit()` performs the **L-20 gesture-end commit**:
    an L-35 outermost override write through the `OverrideWriteGateway` seam (the `context set` path),
    **CAS-guarded** (`--if-match`), applying **L-30 rebase-or-drop** under a concurrent writer —
    **REBASE** onto the new state when the external change left this field path untouched, **DROP
    loudly** (a `cas.mismatch` diagnostic + a commit event, never a silent overwrite) when the same
    field path collided;
  - `add_commit_listener()` registers the R-HUX-011 loop other panels (Problems / status) consume —
    fired on every applied / rebased / dropped outcome.

The panel commits through the `OverrideWriteGateway` seam: the CEF host implements it over
`compose::plan_write` + filesync's atomic CAS write (the live `context set` path), converting the
seam's boundary-clean `OverrideWriteRequest` via `builders::to_write_request`; the headless tests
inject an in-memory implementation over `compose::plan_write`. Wiring the gateway to the live daemon
write path + subscribing to the F2 `SceneSelection` + the R-HUX-011 instrumented input→commit latency
measurement is the CEF-host integration path (a trailing M5 surface), out of this headless panel's scope.

## Building + testing (default `dev` gate — no CEF)

```bash
cmake -S src --preset dev && cd src && cmake --build --preset dev
ctest --preset dev -R "^gui-panel-inspector-"   # model / panel / a11y
```

- `gui-panel-inspector-test_inspector_model` — the schema→field derivation (types, units, override
  marking, immutable/notes exclusion) + the override-write envelope routed through the **real**
  `compose::plan_write` (asserts an outermost override lands).
- `gui-panel-inspector-test_inspector_panel` — the widget tree (a11y + keyboard path), the L-20
  commit, and the **L-30 rebase-or-drop under a concurrent writer** (rebase on an unrelated field,
  loud drop on a colliding field) via an in-memory gateway.
- `gui-panel-inspector-test_a11y` — the **per-panel a11y scan + keyboard-only navigation assertion**
  (R-A11Y-001) across no-selection / populated / overridden / all-readonly worlds. Registered for the
  M5-F6 a11y harness in `../../a11y/coverage/inspector.json` + `../../a11y/coverage.manifest.jsonl`
  (append-only union-merge) and `../../a11y/registry.cpp`.

All three run inside the default `build` job's `ctest --preset dev` on ubuntu/macOS/windows.
