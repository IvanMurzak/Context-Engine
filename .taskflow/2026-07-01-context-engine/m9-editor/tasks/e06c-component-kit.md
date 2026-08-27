---
id: e06c-component-kit
title: editor (06c) — component kit (buttons/fields/tabs/trees/tables/chips/badges/toasts/…) consuming tokens only
group: C
sequence: 13
repo: "."
base_branch: "main"
depends_on: [e06a-tokens-themes, e06b-theme-engine]
importance: 7
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [06, 04]
split_from: e06-tokens-theme-engine   # owner ruling 2026-07-22
superseded_by: [e06c1-kit-foundation-role-widgets, e06c2-authored-component-kit]   # owner ruling 2026-07-23
---

> # ⛔ SUPERSEDED — DECOMPOSED into [`e06c1`](e06c1-kit-foundation-role-widgets.md) + [`e06c2`](e06c2-authored-component-kit.md)
>
> **Owner ruling 2026-07-23**, off the standing pre-screen directive — **never dispatched, so this
> split cost zero wasted runs** (unlike e05 / e05d / e07 / e14, each of which paid a halted run).
> Pre-screen verdict: 12 component families + a blocking tokens-only lint + rewiring the hydration
> widget layer is the exact chunk that made the parent `e06` milestone-sized.
>
> Split axis = **the hydration seam**: `e06c1` = kit module + tokens-only lint + the closed 12-role
> `ctx-widget-*` hydration layer ("every C++-modeled panel is themed"); `e06c2` = the authored
> component families of 06 §3 on that foundation ("chrome and package authors have components").
> `e06d` now needs **`e06c2`**.
>
> This file is kept as origin-of-record. **Do not dispatch it.**

> **Split from [`e06-tokens-theme-engine.md`](e06-tokens-theme-engine.md)** (owner ruling 2026-07-22).
> Third of e06a→e06d, group C. Consumes [`e06a`](e06a-tokens-themes.md) tokens + [`e06b`](e06b-theme-engine.md)
> engine. This is the UI component LIBRARY the hydration widget classes resolve to.

## Goal

The component kit (06 §3): the reusable editor UI components, consuming design tokens ONLY (no raw
values), so every panel/chrome element theme-switches for free and the hydration widget classes have
kit components to resolve to.

## Scope & seams

- **Components** (06 §3): buttons, fields, tabs, trees, tables, chips, badges, toasts, empty-states,
  skeletons, dialogs, tooltips — each consuming tokens (CSS custom properties from e06b) ONLY.
- **Hydration widget classes are kit components**: the hydration runtime's widget classes (keyed by
  node role/type, from e05d1) resolve to these — so a themed panel is themed because its widgets are
  kit components. Do not fork a second styling path.
- **Tokens-only discipline**: a lint / audit fails (blocking, T1) on any raw colour/size/font value in
  a kit component — everything routes through tokens.
- Kit tests run in the **`webui-tests` job** (e07a). Out of scope: Settings panel + user config (e06d).

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set — enumerate which panels/widgets
   consume each component before assuming the list.
2. Read CI before reviewing, even on a NORMAL entry.
3. A passing sibling only exonerates a suspected flake if that leg registers + runs the affected code.
4. Known flakes CE #319 / #322 / #335 + the e05d4 self-hosted-Windows CEF env flake. Toolchain seam
   (e05a): `src/runtime/ts` tool paths NOT visible from `src/editor/`.

## Definition of Done

- [ ] The kit components exist and render from tokens; a theme switch (e06b) restyles them with no
      component change
- [ ] **Tokens-only lint blocking in T1** — no raw colour/size/font values in kit components
- [ ] Hydration widget classes (e05d1) resolve to kit components (no second styling path)
- [ ] `context_assert_shell_boundary` passes non-vacuously; FORBIDDEN list untouched
- [ ] T1 kit tests; tests same PR (R-QA-013); 3-OS CI green
