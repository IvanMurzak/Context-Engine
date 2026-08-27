---
id: e07-commands-palette-keymap
title: One command registry + palette + keymap + when-contexts (D8) — every interaction is a command
group: C
sequence: 18
repo: "."
base_branch: "main"
depends_on: [e05-editor-core-foundation]
importance: 8
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [05, 04, 02]
status: superseded
superseded_by: [e07a-webui-ts-test-tier, e07b-command-registry, e07c-keymap-shell-bridge, e07d-palette-t2-smoke]
---

> ⛔ **SUPERSEDED → e07a / e07b / e07c / e07d** (owner ruling 2026-07-21). This task halted at
> `02-implement` **before any code** with `scope_exceeds_single_pass`: it is milestone-sized (~6
> independently-shippable DoD items across pure TS + a NEW C++ Shell keybindings bridge + a **missing
> editor-core TS test tier** + CI wiring). Decomposed into a strictly-serial chain (all group C, all
> share `src/editor/webui/`), mirroring the e05d1–e05d4 split:
> **[e07a](e07a-webui-ts-test-tier.md)** webui TS T1 test tier (the missing prerequisite; closes the
> R-QA-013 gap) → **[e07b](e07b-command-registry.md)** command registry + when-evaluator +
> contract-verb auto-projection → **[e07c](e07c-keymap-shell-bridge.md)** keymap + the Shell
> keybindings read/watch bridge (new C++ + contract-gate) + undo/redo binding →
> **[e07d](e07d-palette-t2-smoke.md)** palette UI + keyboard-only reachability + raw-key lint + T2
> command-driven CEF smoke (closes e07). This spec's Goal/Scope/DoD below are preserved verbatim as
> the origin-of-record; the children carry the authoritative, sliced DoD. Do NOT implement THIS file.

## Goal

Build the single command registry (D8) with when-context evaluation, the command palette
(R-HUX-004 ships), and the hot-reloaded per-user keymap — making every UI capability
command-invocable, which is what makes T2 command-driven testing and the agent-parity story
(persona C) structurally true.

## Scope & seams

- **Registry** (editor-core): entries `{id, title, category, when, handler}`; three sources:
  (a) contract verbs auto-projected (the `help_model.h` pattern generalized; palette entries
  carry introspected docs), (b) editor commands (window/dock/theme/navigation, move-panel
  keyboard paths), (c) panel-manifest commands (manifest v2 `commands` — 04 §3).
- **when-contexts** (05 §6): `panelFocus`, `panelType`, `viewportMode`, `playState`,
  `textInputFocus`, `windowType` — evaluated from `editor.ui` + session state (until e08
  lands, playState/selection contexts read local stubs behind the same interface).
  Resolution order: text-input > focused panel > window > global (03 §6).
- **Palette**: fuzzy filter, executes any registered command; kit components (e06 may land
  after — use kit primitives when present, plain tokens otherwise).
- **Keymap**: default map in editor-core; user overrides `~/.context/keybindings.json`
  (canonical JSON, `$schema`/version, schema-validated), watched + hot-reloaded; every
  binding targets a command id; bindings resolve through when-contexts; Shell keyboard
  arbitration hook (e04's focus-class rule) consumes the resolver.
- **No raw key handlers**: enforced by a T1 lint over editor-core sources (05 §6).
- **Undo/redo**: `session.undo/redo` (`undo_journal.h:91-92`) bound to Ctrl+Z / Ctrl+Y
  through this path (full wire replay lands in e09; binding + dispatch here).

## Definition of Done

- [ ] Palette opens, filters, and executes commands from all three sources with docs shown
- [ ] Contract verbs auto-projected — a registry addition appears in the palette with no
      hand-written entry (drift test)
- [ ] Keymap: default + user-file override + hot reload + schema rejection all T1-tested;
      when-context resolution-order matrix T1-tested
- [ ] Every dock/window/theme operation reachable as a command (keyboard-only path exists —
      feeds R-A11Y-001/R-CLI-001)
- [ ] Raw-key-handler lint blocking in T1
- [ ] T2 smoke drives a scenario purely via palette/commands; 3-OS CI green
