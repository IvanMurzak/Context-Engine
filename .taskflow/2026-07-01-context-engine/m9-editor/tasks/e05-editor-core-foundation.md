---
id: e05-editor-core-foundation
title: editor-core foundation — npm workspace, app scheme, IPC bridge + JS client, Dockview shell, PanelHost + hydration, roster promotion, layout persistence
group: C
sequence: 2
repo: "."
base_branch: "main"
depends_on: [s1-dockview-cef-spike, e02-client-sdk-boundary, e04-window-shell-windows]
importance: 9
complexity: 8
security_critical: false
production_touching: false
model_hint: top
taskflow_refs: [02, 04, 05]
---

> # ⛔ SUPERSEDED — DECOMPOSED 2026-07-20 (owner ruling). DO NOT IMPLEMENT THIS AS ONE TASK.
>
> Run `9be14dcd847c` ground-truthed this spec against `Context-Engine@5b75dcb7` **before writing
> production code** and halted with `scope_exceeds_single_pass`: it bundles **four independently-
> shippable PRs**. Evidence — the repo's FIRST build-time esbuild bundling target; a from-ZERO CEF
> native↔JS channel (`CefMessageRouter`: zero hits repo-wide); a from-ZERO `context-editor://`
> scheme (`docs/shell.md:312-315` defers it here); a **BREAKING `kContractMajor` 1→2** against a
> deny-by-default registry; promotion of a stack-local `ExtensionRegistry` to a global roster; and
> a ~2000+ line net-new TS app. **~40% (the CEF surface) is un-buildable on the dev host.**
>
> **Implement these instead, in dependency order** (e05a ∥ e05b may run concurrently — disjoint trees):
>
> | | Spec | Why separate |
> |---|---|---|
> | **e05a** | [`e05a-webui-workspace-toolchain.md`](e05a-webui-workspace-toolchain.md) | TS toolchain; fully locally verifiable |
> | **e05b** | [`e05b-manifest-roster-state-contract.md`](e05b-manifest-roster-state-contract.md) | pure C++; carries the breaking major bump |
> | **e05c** | [`e05c-app-scheme-ipc-bridge.md`](e05c-app-scheme-ipc-bridge.md) | from-zero CEF; CI-gated only |
> | **e05d** | [`e05d-panelhost-hydration-layout.md`](e05d-panelhost-hydration-layout.md) | the usable-editor payload |
>
> This file is kept as the **origin of record** for the scope and the design references; the four
> specs above are authoritative. Live state: [`../ROADMAP.md`](../ROADMAP.md).
>
> ♻ **Preserved WIP:** commit `5f942fe` (dockview fetch channel, 22/22 green) is on
> `origin/worktree-9be14dcd847c` — **e05a starts from it**, do not re-author.

## Goal

Create the TS web application layer: the `src/editor/webui/` npm workspace on the repo's
pinned toolchain, the privileged Shell IPC bridge + generated-schema JS client, the Dockview
shell wrapped in our PanelHost, the hydration runtime binding uitree panels to live DOM, the
promoted panel roster (manifest v2), and per-window layout persistence — the editor becomes
usable: panels dock, render live project state, and survive restart.

## Scope & seams

- **Workspace**: `src/editor/webui/core/` → `@context-engine/editor-core`; bundling via the
  EXISTING SHA-pinned esbuild (`tools/ts-toolchain.json`, `tools/fetch_esbuild.py`); no Node
  at runtime, no npm in CI beyond pinned fetch-verified tooling; Dockview per s1's exact
  pinned package set (owner allowlist gate must have passed).
- **Assets + scheme**: static assets shipped in-app, served via `context-editor://app/…`
  (pinned scheme flags), never `file://` temp files.
- **IPC bridge**: CefMessageRouter / `context-editor://ipc`; Shell holds socket + attach
  token — editor-core NEVER sees them (04 §1); bridge is how editor-core reaches the daemon
  and the Shell (window registry, drag sessions, region maps).
- **JS client**: thin typed wrapper over the bridge; typings from e02's generated schema —
  hand-written typings prohibited.
- **PanelHost over Dockview**: Dockview = geometry only; PanelHost owns panel lifecycle;
  popout API unused (B-F2). Strict no-inline-script CSP.
- **Panel manifest v2 + roster promotion** (04 §3): extend `Contribution`
  (`extension.h:32-44`) — contractVersion 2, icon, dock defaults, content type
  (uitree|iframe), state schemaVersion, capabilities, commands, themes;
  **`ExtensionRegistry` becomes the single roster** (deny-by-default stands); regenerate the
  a11y hand-list (`a11y/registry.cpp:21-113`) FROM it and **ADD `builtin.session.undo`**
  (A-F2 — absent from both current anchors).
- **State contract (D6)**: `getState()/restoreState()` versioned blobs on every panel;
  purity rule panel = f(bridge state, blob); schemaVersion mismatch → `null` + diagnostic.
- **Hydration runtime v1** (04 §4): uitree HTML request over bridge → DOM mount; focusables
  follow `focus_order`; activation dispatches bound command ids; gesture verbs
  (begin/extend/commit/cancel) mapped; incremental DOM patches keyed by stable node ids;
  widget classes keyed by node role/type (presentation-only).
- **Escaping contract** (C-F6): `render_html` mandatory escaping on every text
  interpolation, T1-asserted with adversarial project strings; CSP as backstop.
- **Layout persistence**: Dockview `toJSON()` per window + placements → editor-owned
  `.editor/editor-state.json` (debounced + on-exit + crash-restore; Shell single-writer).
- Region-map publication to the Shell on layout change (input arbitration feed, 03 §6).

## Definition of Done

- [ ] App boots inside the e04 shell window from the app scheme under strict CSP
- [ ] Scene tree + Inspector + Problems hydrate from the LIVE daemon via bridge (read path);
      interactions dispatch commands to the C++ models
- [ ] Panels dock/split/tab/float; layout + panel state persist and restore across restart
- [ ] Roster promotion complete: manifest v2 drives panel listing; a11y hand-list is
      regenerated (mechanically enforced); `builtin.session.undo` covered
- [ ] T1: state-contract round-trips, escaping adversarial strings, schema-mismatch handling;
      T2: boot → dock → restore smoke
- [ ] Boundary discipline: editor-core deps = s1-approved set only; 3-OS CI green
