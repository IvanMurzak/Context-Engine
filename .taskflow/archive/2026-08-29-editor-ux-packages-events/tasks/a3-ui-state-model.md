---
id: "a3-ui-state-model"
title: "Five-state interaction styling across the kit's 12 roles, the chrome, and the Dockview tab strip"
group: "A"
sequence: 3
repo: "."
base_branch: "main"
depends_on: []
importance: 6
complexity: 6
security_critical: false
production_touching: false
model_hint: "mid"
taskflow_refs: ["01-current-architecture.md", "07-ui-states.md"]
---

## Goal

Interactive elements give no hover/press feedback. Measured: `kit.css` styles the twelve closed
`uitree::Role` widget classes with **3** `:hover` rules (`:94` listitem, `:95` treeitem, `:115`
button), **0** `:active`, **0** `:disabled`/`[aria-disabled]`, **0** `transition` — while
`:focus-visible` covers all 12 roles (`:163-174`) and must survive. Propagate the state model the
component layer already uses (`components.css:52,149,212` transitions, `:55` disabled) down to the
widget layer and out to the chrome and the Dockview tab strip.

## Scope & seams

- **The state model is five states, not four** (`07` §2): Normal (base tokens) · Hover (**one
  border + surface step up — never a shadow, never an opacity change**; an opacity hover dilutes the
  high-contrast themes' AA+ ink ratio, a mistake `kit.css` documents having corrected once) ·
  Pressed (`:active`, one further step or the accent-tinted step, **visibly distinct from hover**) ·
  Disabled (**both** `:disabled` **and** `[aria-disabled="true"]`, always — `app.css:389-391` records
  why: the ARIA menu pattern keeps a disabled item focusable) · Focus (`:focus-visible`,
  **unchanged**, composing with the others).
- Transitions on colour-ish properties only (`background-color`, `border-color`, `color`) at
  `var(--ctx-motion-duration-fast) var(--ctx-motion-easing-standard)`. Not on `transform`, not on
  layout properties — the composite is damage-driven and a re-layout under transition is jitter.
- Files: `src/editor/webui/kit/styles/kit.css` (all 12 roles), `app.css` (chrome), and the Dockview
  bridge.
- **Dockview tab strip**: express tab hover through the `--dv-*` variables dockview reads, under the
  **`html`-prefixed** selector at `app.css:667` — a bare `.dockview-theme-dark` block is silently
  ignored for every variable `dockview.css` itself declares (specificity 0,1,0 vs 0,1,1; measured
  once already). `theme.ts`'s `DOCKVIEW_CHROME` table is the same map and moves with it.
  `app.css:639` records a T1 test coupled to the tab strip's surface step — those constants move in
  the same diff.
- Out of scope: new components; the duplicated title (`a4`); any theme redesign.

## Definition of Done

- All 12 roles carry the five states; `:focus-visible` present on all 12 exactly as before.
- **ctest `webui-kit-tokens-only` green**: no raw literal anywhere in `kit.css`, including inside a
  `var()` fallback. If a state needs a step the theme does not publish, add a theme token (e06a
  schema change) — never a `calc()` over an existing token.
- **ctest `webui-kit-role-coverage` green**: every Role styled in `kit.css`, no `.ctx-widget-` rule
  in `app.css`.
- Reduced motion needs **no new media query**: durations are read from tokens and
  `theme.ts:306-335` + `app.css:163` already collapse them — verify, don't duplicate.
- Tab-strip hover confirmed on the **rendered** result (a `webui-ts-*` browser-tier assertion over
  the computed style, or, if genuinely unassertable, a row in `docs/shell.md`'s manual table in its
  existing format) — reading the selector is not confirmation, per the measured specificity trap.
- Disabled styling proven for both `:disabled` and `[aria-disabled="true"]` paths.
- Tests land in the same PR (R-QA-013).
