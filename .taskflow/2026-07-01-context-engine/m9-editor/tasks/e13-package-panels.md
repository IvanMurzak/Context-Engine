---
id: e13-package-panels
title: Package panels end-to-end — iframe host, MessageChannel bridge, capability scopes, scaffold template, demo external package
group: C
sequence: 27
repo: "."
base_branch: "main"
depends_on: [e05-editor-core-foundation, e06-tokens-theme-engine, e07-commands-palette-keymap, e08-session-state-ui-bus]
importance: 8
complexity: 7
security_critical: true   # sandbox/capability enforcement is a security boundary
production_touching: false
model_hint: top           # mid by complexity, bumped for security
taskflow_refs: [04, 08, 05]
---

## Goal

Make the panel ecosystem real: third-party panels as sandboxed, capability-scoped iframes
with the full bridge API, install-consent scope grants, theme-token delivery — proven by a
demo "hello-panel" package living OUTSIDE the repo and a scaffold template (persona B).

## Scope & seams

- **Iframe host** (editor-core PanelHost, content type `iframe` per manifest v2):
  `<iframe sandbox="allow-scripts">`, per-extension origins `context-ext://<package-id>/…`
  (scheme registration pinned `STANDARD|SECURE|CORS_ENABLED`, all processes); strict CSP, no
  external hosts, no Node.
- **Bridge auth** (B-F6): sandboxed frames have an OPAQUE origin (`event.origin === "null"`)
  — authenticate via **MessageChannel ports handed at creation**, never origin strings;
  per-extension process isolation (Chromium `IsolateSandboxedIframes` — a feature default,
  not a CEF contract) **verified in T2** per the s1 probe findings.
- **Panel bridge API** (04 §5, postMessage RPC, promise-based): `bridge.call(verb, params)`
  — scope-checked in the DAEMON DISPATCHER (`dispatcher.cpp:203` model; adapters are
  bypassable, the dispatcher is not); `bridge.events.subscribe(topics)` (daemon facts);
  `bridge.ui.subscribe(topics)` — requires the `ui_events` capability (C-F18);
  `bridge.state.get/set` (own blob); `bridge.commands.register/execute` (manifest commands);
  `bridge.theme.tokens` + change events.
- **Capability model**: manifest `capabilities` → install-time consent surface (L-49;
  AI-installed packages default to sandbox tier); default scope = `read_query`;
  `build_install` never granted to panels by default; package custom `editor.ui` topics
  namespaced + manifest-declared.
- **Install flow**: npm content package (existing package system) → contribution registered
  deny-by-default (`registry.h:22-25,46`) → appears in layout targets + palette; package
  theme contributions validated (e06).
- **Scaffold template**: `context new --template extension-panel` (R-QA-006 runnable
  templates) → manifest + hello iframe ready (persona B ≤3-step budget).
- **Demo external package** ("hello-panel") in a separate repo/folder OUTSIDE
  Context-Engine — the contract is only real if an external package exercises it end-to-end
  (M9 exit clause 5).

## Definition of Done

- [ ] hello-panel installs from outside the repo; consent prompt lists requested scopes;
      panel appears in palette/dock targets; docks, floats, tears out like a built-in
- [ ] Iframe receives theme tokens + re-tokens on switch; state blob round-trips (reload
      preserves state — persona B iteration flow)
- [ ] Scope-denial: un-granted `file_write`/`build_install` calls rejected IN THE DISPATCHER
      — blocking T1 + T2 asserts (08 §3)
- [ ] MessageChannel-port auth + opaque-origin handling asserted; process-isolation probe
      result recorded in T2
- [ ] `ui_events` capability gates `bridge.ui.subscribe` (denied without grant — T2)
- [ ] Scaffold template ships and produces a working panel in ≤3 steps
- [ ] 3-OS CI green
