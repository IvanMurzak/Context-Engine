---
id: e06c1-kit-foundation-role-widgets
title: editor (06c1) — kit foundation + tokens-only lint + the closed 12-role hydration widget layer
group: C
sequence: 14
repo: "."
base_branch: "main"
depends_on: [e06a-tokens-themes, e06b-theme-engine]
importance: 7
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [06, 04]
split_from: e06c-component-kit   # owner ruling 2026-07-23
---

> **Split from [`e06c`](e06c-component-kit.md)** (owner ruling 2026-07-23, off the standing
> pre-screen directive). First of e06c1→e06c2, group C. Consumes
> [`e06a`](e06a-tokens-themes.md) tokens + [`e06b`](e06b-theme-engine.md) engine.
>
> **Split axis = the hydration seam.** e06c1 builds the kit's *foundation* (module, tokens-only
> lint, T1 tier) and closes the **hydration widget layer** — the closed 12-role `ctx-widget-*`
> set that `hydration.ts` actually emits. [`e06c2`](e06c2-authored-component-kit.md) then builds
> the *authored* component families (06 §3) on top of it. Cutting here keeps each half coherent:
> this task owns "every C++-modeled panel is themed", e06c2 owns "chrome and package authors have
> components to build with".

## Goal

Stand up the component kit as a real module with a **blocking tokens-only lint**, and make the
hydration runtime's widget layer a first-class, fully-tokenised part of it — so every panel the
C++ model renders is themed because its widgets are kit widgets, with no second styling path.

## Scope & seams

- **Kit module**: give the kit a home in the `src/editor/webui/` workspace (sibling of `tokens/`
  and `core/`, wired into the e05a esbuild/codegen toolchain and the e07a `webui-tests` job).
  Whether it is a new workspace package or a module inside `core/` is an implementation call —
  make it, record why, and keep the e05a toolchain seam intact (`src/runtime/ts` tool paths are
  NOT visible from `src/editor/`).
- **The closed 12-role widget layer.** `hydration.ts`'s `WIDGET_CLASSES` maps a closed set of 12
  uitree roles onto `ctx-widget-*` classes: `tree · treeitem · list · listitem · button ·
  textbox · checkbox · heading · status · region · group · text`. Today `app.css` styles **9 of
  the 12** (missing `textbox`, `checkbox`, `text`) and it is app-level CSS, not kit. Move that
  layer into the kit and complete it — all 12 roles styled from tokens, including the
  `:hover` / `:focus-visible` states already present.
  - The closedness is load-bearing (`render_html` emits only the twelve tags `role_html_tag`
    maps to). **Assert it**: a test must fail if `WIDGET_CLASSES` grows a role the kit does not
    style, so a future role cannot silently ship unthemed. Do not duplicate the role list by
    hand — derive it.
- **Tokens-only lint, blocking in T1** (this is the piece that makes the discipline real for
  e06c2 too): no raw colour / size / font literals in kit sources — everything routes through
  the e06a/e06b CSS custom properties. `app.css` currently carries **8 raw colour literals**;
  whatever of that migrates into the kit must be tokenised, and the lint must be **non-vacuous**
  (prove it fails on a planted raw value, the way the e05d3 boundary gate was proven).
  - Decide and record what the lint's jurisdiction is *today* (kit sources) versus what e06c2
    inherits. Raw values that remain in non-kit `app.css` are a finding to report with a
    follow-up, not a reason to weaken the lint's scope silently.
- **No second styling path**: after this task the hydration widgets must not be styled from both
  `app.css` and the kit. One owner.
- Out of scope: the authored component families (tabs/tables/dialogs/toasts/… → e06c2), the
  Settings panel + user config (e06d), the `editor.ui` bus (e08c).

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set — enumerate every consumer of
   the widget classes (panels, `panelhost.ts`, the CEF smokes) before assuming this list is it.
2. Read CI before reviewing, even on a NORMAL entry.
3. A passing sibling only exonerates a suspected flake if that leg registers + runs the affected
   code.
4. **A local browser probe inherits the DEV HOST's ambient media state** (`prefers-color-scheme`,
   reduced-motion, locale, DPI); a CI runner has none. Force the CI condition before trusting a
   local green — this cost e06b a full CI round.
5. **e06b's hard-won lesson, directly relevant here**: `dockview-core` injects its own stylesheet
   at runtime **and writes inline CSSOM styles**, and an inline style beats any stylesheet
   selector. If a kit rule must win over dockview chrome, verify against the *rendered* result,
   not the specificity you reasoned about.
6. Beware a test mock more capable than the real thing (e08a's `granted_scopes()` hid for 6 tasks).
7. Known flakes: CE #319 (fixed, x3) / #322 / #335 + the self-hosted-Windows CEF env family.

## Definition of Done

- [ ] The kit module exists, is built by the e05a toolchain, and its tests run in the e07a
      `webui-tests` job
- [ ] **Tokens-only lint blocking in T1**, and proven **non-vacuous** (fails on a planted raw
      colour/size/font value)
- [ ] All **12** hydration roles are styled by the kit from tokens only; a test FAILS if
      `WIDGET_CLASSES` gains a role the kit does not style
- [ ] Hydration widgets have exactly ONE styling owner (no surviving `app.css` duplicate path)
- [ ] A theme switch (e06b) restyles the widget layer with no widget-code change
- [ ] `context_assert_shell_boundary` passes non-vacuously; FORBIDDEN list untouched
- [ ] Tests same PR (R-QA-013); 3-OS CI green

## Links

- Split from: [[e06c-component-kit]] · next: [[e06c2-authored-component-kit]]
- Consumes: [[e06a-tokens-themes]], [[e06b-theme-engine]] · hydration runtime from [[e05d1-panelhost-hydration-runtime]]
- Design: `06-theme-design-system.md` §3, `04-editor-web-app-docking-panels.md` §4
