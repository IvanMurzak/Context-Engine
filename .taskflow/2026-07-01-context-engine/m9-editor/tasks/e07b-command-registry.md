---
id: e07b-command-registry
title: editor-core (07b) — command registry {id,title,category,when,handler} + when-evaluator + contract-verb auto-projection (D8)
group: C
sequence: 20
repo: "."
base_branch: "main"
depends_on: [e07a-webui-ts-test-tier]
importance: 8
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [05, 02]
split_from: e07-commands-palette-keymap   # owner ruling 2026-07-21
---

> **Split from [`e07-commands-palette-keymap.md`](e07-commands-palette-keymap.md)** (owner ruling
> 2026-07-21). Second of the serial chain e07a→e07d (all group C, share `src/editor/webui/`). Needs
> the [`e07a`](e07a-webui-ts-test-tier.md) TS test tier so its `T1-tested` DoD items have somewhere
> to run.

## Goal

Build the single **command registry** (D8) — `{id, title, category, when, handler}` — with the
**when-context evaluator**, so every UI capability is command-invocable. This is the substrate that
makes T2 command-driven testing and the agent-parity story (persona C) structurally true; the palette
UI + T2 smoke ride on it in [`e07d`](e07d-palette-t2-smoke.md).

## Scope & seams

- **Registry** (editor-core, pure TS), three entry sources:
  (a) **contract verbs auto-projected** — the `help_model.h` pattern generalized; palette entries
  carry introspected docs, so a new contract verb appears with NO hand-written entry;
  (b) **editor commands** — window / dock / theme / navigation, incl. the move-panel keyboard paths;
  (c) **panel-manifest commands** — manifest v2 `commands` (04 §3), read from e05b's promoted roster.
- **when-contexts** (05 §6): `panelFocus`, `panelType`, `viewportMode`, `playState`,
  `textInputFocus`, `windowType` — evaluated from `editor.ui` + session state. ⚠ **e08 (session
  state) has not landed** — read `playState`/selection contexts from **local stubs behind the same
  interface**, so e08 swaps the source with zero registry change. Resolution order (03 §6):
  text-input > focused panel > window > global.
- **Drift test (T1):** adding a registry source entry (esp. an auto-projected contract verb) must
  surface without a hand-written palette entry — assert it so the auto-projection can't silently rot.
- **No palette UI, no keymap here.** The registry exposes a query/execute API; the palette (e07d) and
  keymap (e07c) are consumers. Keep the handler dispatch surface clean for both.
- ⚠ **Toolchain seam** (e05a): `src/runtime/ts` tool paths are NOT visible from `src/editor/`.
- ⚠ **Ripple-list lesson (e05b/e02):** enumerate EVERY consumer of the command/verb surface from the
  code (help projection, uitree activation, the CEF smoke harness) before assuming this list is whole.

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set.
2. Read CI before reviewing, even on a NORMAL entry.
3. A passing sibling only exonerates a suspected flake if that leg registers + runs the affected code.
4. Known flakes CE #319 / #322 + the e05d4 self-hosted-Windows CEF infra flake (env, not code).

## Definition of Done

- [ ] Registry executes commands from all three sources (contract verbs / editor commands /
      panel-manifest commands), each carrying introspected docs
- [ ] Contract verbs auto-projected — a registry addition appears queryable with no hand-written
      entry (**drift test**, T1 on the e07a tier)
- [ ] when-context evaluator: all six contexts + the text-input > focused-panel > window > global
      **resolution-order matrix T1-tested**; e08-owned contexts read a swappable local stub
- [ ] `context_assert_shell_boundary` still passes non-vacuously; FORBIDDEN list untouched
- [ ] Every behavior change ships WITH its tests same PR (R-QA-013); 3-OS CI green
