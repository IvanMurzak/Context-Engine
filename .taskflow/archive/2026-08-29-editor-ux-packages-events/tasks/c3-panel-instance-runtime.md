---
id: "c3-panel-instance-runtime"
title: "Panel identity becomes (panelId, instanceId) through both hosts, the wire, state, and layout restore"
group: "C"
sequence: 3
repo: "."
base_branch: "main"
depends_on: ["c2-manifest-v3"]
importance: 8
complexity: 9
security_critical: false
production_touching: false
model_hint: "top"
taskflow_refs: ["01-current-architecture.md", "04-panel-instances-and-menu.md", "08-compatibility-and-migration.md"]
---

## Goal

Implement D6's imperative half. Today a panel's identity **is** its manifest id — `PanelHost.#panels`
is keyed by it (`panelhost.ts:808`), `open` refuses on `has(manifest.id)` for every panel
(`:1091`), and the C++ `panel_host.h:168-254` keys every operation by `panel_id` with a provider
binding **one** model — so a panel can neither be reopened by name nor exist twice, and
`dock.singleton` was decorative. Identity becomes **`(panelId, instanceId)`**: `panelId` names the
kind, `instanceId` the live copy, honoured end to end.

## Scope & seams

| Surface | Change |
|---|---|
| C++ `PanelHost` (`panel_host.h:168-254`) | The provider becomes a **factory**: one model per instance; every method (`provide`/`render`/`invoke`/`gesture`/`get_state`/`restore_state`/…) rekeys to the pair. `hostable_panel_ids()` still enumerates **kinds** |
| Wire | `instanceId` joins `panel.render`, `panel.command`, `panel.gesture`, `panel.state.get`, `panel.state.set` — **additive, but gated** (below) |
| `panels.ts` | `PanelManifest` carries `instances` + `path`; parsers stay total and fail-closed exactly as now |
| `panelhost.ts` | `#panels` rekeys; `open` consults `instances.mode`; `openById` → `openInstance` (the e10b tear-out seed path, `:1122`); Dockview panel ids become instance ids |
| D6 state | Persisted **per instance** |
| Layout restore | `layoutrestore.test.ts` round-trips instance ids |
| e10b tear-out / rehome | Moves an **instance**, not a kind |
| a11y | **Unchanged — keyed by panel KIND** (planner ruling): auditing N copies of one model proves nothing the first copy did not |

- **Open semantics**: `singleton` — a second open **focuses** the existing instance and returns an
  honest "already open" outcome, not a failure. `limited` — refused past `max` with a diagnostic
  naming the limit. `unlimited` — mints a new instance.
- **The gate to plan around** (`08` §2): `tools/check_webui_assets.py --panel-contract` (ctest
  `webui-panel-contract`) byte-compares the TS vocabulary against the C++ constants **out of the
  BUILT bundle**. The TS and C++ constants move in the **same commit** or the gate reds — and because
  it reads the built bundle, rebuild before interpreting a failure. A split lands a silently unbound
  panel surface (renderer calls a method the Shell no longer routes; the editor comes up empty with
  no build error).
- The first *shipping* multi-instance panel is `e3`'s viewport; this task proves the three modes via
  test fixtures on both hosts, not by promoting an existing singleton panel.
- Out of scope: the Window menu (`d1`); making `builtin.viewport` hostable (`e3`); any new panel.

## Definition of Done

- Both hosts rekeyed; `instanceId` on all five wire methods; **`webui-panel-contract` green with the
  TS and C++ constants moved in one commit** (the set's named gate).
- Open-semantics tests, all three modes, both the refusal and its sibling: singleton second-open
  focuses and reports "already open"; limited opens up to `max` and the `max+1` open is refused with
  the limit named; unlimited mints distinct instances with distinct state.
- Per-instance state: two instances of one kind hold and restore **different** state; a state write
  to one does not leak to the other.
- Layout restore round-trips instance ids (`layoutrestore.test.ts` extended), including the
  restore-after-close-and-reopen path.
- Tear-out/rehome moves the intended instance and its state (e10b path test).
- `editor-shell-test_builtin_panels` and the four-anchor gates stay green; a11y coverage remains
  keyed by kind with **no** per-instance entries.
- PR body cites D6.
