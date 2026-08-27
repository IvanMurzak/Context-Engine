# 04 — editor-core: web app, docking, panels

## 1. editor-core (the TS web app)

Net-new npm workspace (repo has zero web infra — 01 §2): `src/editor/webui/` with packages
`@context-engine/editor-core` (app) and `@context-engine/editor-tokens` (06). Toolchain rides the
EXISTING SHA-pinned esbuild (`tools/ts-toolchain.json`, `tools/fetch_esbuild.py`) — bundling is a
build step producing static assets; **no Node at runtime, no npm in CI beyond the pinned
fetch-verified toolchain** (repo convention). Assets ship inside the app and load via a custom
CEF scheme (`context-editor://app/…`), not `file://` temp files.

One editor-core instance per window. Instances coordinate through the Shell (window registry,
drag sessions, region maps) over a small privileged IPC bridge (CefMessageRouter /
`context-editor://ipc`), which is ALSO how editor-core reaches the daemon: the Shell owns the
socket + attach token; editor-core never sees them (the `ExtensionBridge` "a panel never holds
the socket/token" rule, applied one layer up too).

## 2. Docking (Dockview, D2)

- Dockview provides: splits, tabs, floating groups, serialization, CSS-variable theming (pin
  **v7.x**; ~~the v7 package split — `dockview` core + `dockview-modules`, incl. the a11y
  module — is named exactly at s1; B-F9~~ → ✅ **s1 NAMED IT 2026-07-19: exactly ONE package,
  `dockview-core@7.0.2`** — MIT, **0 runtime deps**, framework-agnostic core. The assumed
  core+`dockview-modules` set is **not** needed; that assumption is retired. Owner-approved into
  the production allowlist and **VERSION-PINNED** — a bump past `7.0.2` or any additional
  `dockview-*` package re-triggers the 08 §3 standing consent gate). We wrap it in a `PanelHost` that owns panel
  lifecycle; Dockview manages geometry only. Its **popout API is deliberately unused** (B-F2:
  v7 rejects non-http(s) popout URLs before `window.open` ever fires, and its popout is
  opener-owned DOM transfer — incompatible with independent per-window editor-core instances).
- **Tear-out = a first-class PanelHost/Shell mechanism** (command + drag), not a browser popup:
  drag past window bounds or invoke the tear-out command → panel serializes (D6) → Shell
  creates an `EditorWindow` + fresh editor-core (03 §1) → a Dockview root seeded with the moved
  panel restores it from state (§3).
- **Cross-window drag**: a Shell-mediated drag session — drag leaves a window's bounds → Shell
  tracks global cursor, renders the drag ghost, targets the window under cursor, asks its
  editor-core for the drop zone; drop = rehome (§3). Within one window, Dockview native DnD.
- Layout persistence: Dockview `toJSON()` per window + window placements → the editor-owned
  `.editor/editor-state.json` (03 §1 ownership split; debounced, on-exit, crash-restore).
- **Spike s1 ratifies** Dockview under: CEF 149, per-window instances, sandboxed-iframe panel
  content, serialize/restore, the PanelHost tear-out flow (popout API unused), a per-extension
  process-isolation probe (B-F6), a11y scan of its chrome, exact v7 package-set naming.
  Fallbacks in order: Golden Layout, then Lumino + our own layer (D2).
  ✅ **s1 DONE 2026-07-19 — verdict RATIFY** (CE PR #304 `e8508d2`). All measurable probes PASS
  (docking/CSP, sandboxed-iframe, `toJSON` restore, non-http(s) popout rejected — confirming
  B-F2 — and a11y); the OS process-isolation probe was recorded, not gated. Package set named
  above. **Fallback NOT triggered.** Ledger: [`ROADMAP.md`](ROADMAP.md).

## 3. Panel model — the extended R-EDIT-001 contract

`Contribution` (as-built `extension.h:32-44`) is extended into a **panel manifest**:

```jsonc
{
  "id": "builtin.inspector",            // existing
  "kind": "panel",                      // existing (panel|inspector|gizmo|asset_kind_editor)
  "title": "Inspector",                 // existing
  "contractVersion": 2,                  // M9 bumps kContractMajor
  "icon": "inspect",                    // icon-set name (06)
  "dock": { "defaultZone": "right", "singleton": false, "minSize": [280, 200] },
  "content": { "type": "uitree" | "iframe", "entry": "<url for iframe>" },
  "state": { "schemaVersion": 1 },
  "capabilities": ["read_query", "ui_events"],  // scope grants incl. editor.ui read (08)
  "commands": [ { "id": "inspector.edit", "title": "Edit field", "when": "panelFocus == inspector" } ],
  "themes": [ "<optional theme.json paths>" ]   // theme contributions (06)
}
```

- **`ExtensionRegistry` becomes the single roster** (deny-by-default rules as-built stand); the
  hand-list `a11y/registry.cpp:21-113` is regenerated FROM it (a11y coverage stays mechanically
  enforced — panels can't dodge the scan). The promotion must ADD `builtin.session.undo`,
  absent from both current anchors today (A-F2).
- **State contract (D6)**: every panel implements `getState() → {schemaVersion, data}` /
  `restoreState(state)`. Purity rule: panel = f(project state via bridge, state blob). The host
  persists blobs with the layout; rehome/tear-out/crash-restore/reload all use it. No
  retainContext. Migration: on schemaVersion mismatch, panel receives `null` state + a
  diagnostic (never a crash).

## 4. Built-in panels: hydration (D17)

The C++ panel models stay the logic + a11y authority. The hydration runtime:

1. Requests the panel's uitree render (semantic HTML + node metadata: ids, roles, focusable,
   bound command ids) over the bridge.
2. Mounts it into the panel's DOM slot; binds interactions: focusables follow `focus_order`;
   activation dispatches the node's bound command through the command registry → bridge → panel
   model; continuous gestures (tilemap paint, gizmo drags, tree drag) map to the models' explicit
   gesture verbs (`begin/extend/commit/cancel` — already designed for this).
3. Subscribes to the panel's change events; re-renders are incremental DOM patches keyed by
   stable node ids.
4. Rich widgets (virtualized trees, numeric drag-inputs, color fields) are hydration-runtime
   **widget classes keyed by node role/type** — presentation-only; the VALUE semantics stay in
   the C++ model + write path.

Security note (C-F6): the uitree HTML renders **authored project strings** (entity names, the
schema-blessed `notes`, any string field). `render_html` carries a **mandatory escaping
contract** on every text interpolation, asserted in T1 with adversarial project strings; the
strict no-inline-script CSP is the backstop. Hostile project content must never execute in the
trusted editor-core zone.

Why not rewrite panels in TS: 9 tested panels + the a11y/T1 harness already exist in C++; the
law requires headless-instantiable editor UI logic (R-EDIT-001 "testable-by-construction"), and
one logic implementation serving both CI and the live editor is the maintainability-optimal shape
(D4). Third-party panels are free to be pure web apps (iframe type).

## 5. Third-party panels (iframe type)

- Sandboxed `<iframe sandbox="allow-scripts">`, per-extension isolated renderer (site-isolation
  via distinct origins `context-ext://<package-id>/…`), strict CSP, no Node — the R-EDIT-001
  sandbox clauses, now concrete. Scheme registration is pinned (`STANDARD|SECURE|CORS_ENABLED`,
  in all processes); sandboxed frames have an **opaque origin** (`event.origin === "null"`), so
  the bridge authenticates via **MessageChannel ports** handed to the frame at creation — never
  origin strings; per-extension process isolation rides Chromium's `IsolateSandboxedIframes`
  (a feature default, not a CEF contract) and is therefore **verified in s1/T2** (B-F6).
- The **panel bridge API** (postMessage RPC, promise-based): `bridge.call(verb, params)` (scope-
  checked in the daemon dispatcher — never in the adapter), `bridge.events.subscribe(topics)`
  (daemon facts), `bridge.ui.subscribe(topics)` (`editor.ui` facts — requires the `ui_events` capability,
  C-F18), `bridge.state.get/set`
  (its own blob), `bridge.commands.register/execute` (its manifest commands),
  `bridge.theme.tokens` (+ change events).
- Panels ship in npm content packages (existing package system); install → manifest contribution
  registered → available in layout/palette. AI-installed packages default to sandbox tier
  (L-49); panel capability grants surface at install consent.
- A **demo external package** ("hello-panel") is part of the M9 deliverable — the contract is
  only real if a package outside the repo exercises it end-to-end.

## 6. Accessibility & polish floor (per panel, enforced)

- R-A11Y-001 continues mechanically: uitree panels keep the existing scan; iframe panels get an
  axe-style scan in T2 + keyboard-only navigation test; **Dockview chrome itself** (tabs, drop
  zones, floating controls) enters the a11y scan + keyboard map (tab strips arrow-navigable,
  dock/undock/move-to-window available as commands — no pointer-only capability, R-CLI-001
  structural property).
- Empty states, loading skeletons, and error surfaces are part of each panel's definition of
  done (06 kit provides the components) — not ad-hoc.
