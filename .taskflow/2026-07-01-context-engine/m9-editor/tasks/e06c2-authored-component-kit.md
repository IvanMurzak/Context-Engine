---
id: e06c2-authored-component-kit
title: editor (06c2) — the authored component families (06 §3) on the e06c1 foundation
group: C
sequence: 15
repo: "."
base_branch: "main"
depends_on: [e06c1-kit-foundation-role-widgets]
importance: 7
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [06, 04]
split_from: e06c-component-kit   # owner ruling 2026-07-23
---

> **Split from [`e06c`](e06c-component-kit.md)** (owner ruling 2026-07-23). Second of
> e06c1→e06c2, group C. Builds on [`e06c1`](e06c1-kit-foundation-role-widgets.md)'s kit module,
> tokens-only lint and 12-role hydration widget layer.
>
> e06c1 delivered "every C++-modeled panel is themed". This task delivers "chrome, builtin panels
> and package authors have reusable components to build with" — the named families of 06 §3.

## Goal

Ship the authored component kit (06 §3): the reusable editor UI components, consuming design
tokens ONLY, on top of e06c1's foundation — so editor chrome and builtin panels (starting with
e06d's Settings) stop hand-rolling markup, and package authors get the published, themed surface
the design promises.

## Scope & seams

- **Component families** (06 §3, the full named list): buttons, fields, tabs, trees, tables,
  chips, badges, toasts, empty-states, skeletons, dialogs, tooltips.
  - Several of these have a **primitive already** in e06c1's role layer (`button`, `textbox`,
    `checkbox`, `tree`/`treeitem`, `list`/`listitem`, `status`). **Build on those, do not fork a
    parallel implementation** — an authored `Button` and a hydrated `ctx-widget-button` must
    resolve to the same visual contract. Where a family has no role primitive (tabs, tables,
    dialogs, toasts, chips, badges, empty-states, skeletons, tooltips), it is net-new.
  - Where reusing the primitive is genuinely wrong for a family, that is a finding to record with
    the reason — not a silent second path.
- **Tokens-only, enforced**: every new component falls under e06c1's blocking lint. No raw
  colour/size/font literals. If the lint's jurisdiction needs widening to cover the new sources,
  widen it in this PR.
- **Accessibility is part of the component, not a later pass**: keyboard reachability and correct
  roles/labels for the interactive families (tabs, dialogs, tooltips, toasts, tables). e16 audits
  a11y; it should not be *fixing* the kit. Dialogs and tooltips in particular need focus handling
  that a later audit cannot retrofit cheaply.
- **Published surface**: the kit + tokens are published to package authors (06 §3 policy —
  *tokens mandatory, kit strongly recommended*). Make the entry point explicit and stable; a
  package panel on raw HTML must still inherit colours/type/shape via injected tokens.
- Out of scope: the Settings panel + user config (e06d — its **first consumer**), the `editor.ui`
  bus (e08c), anything in the C++ Shell.

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set — enumerate which panels/chrome
   elements consume each family before assuming this list is complete.
2. Read CI before reviewing, even on a NORMAL entry.
3. A passing sibling only exonerates a suspected flake if that leg registers + runs the affected
   code.
4. A local browser probe inherits the DEV HOST's ambient media state; a CI runner has none —
   force the CI condition before trusting a local green (cost e06b a full CI round).
5. **`dockview-core` writes inline CSSOM styles at runtime, and an inline style beats any
   stylesheet selector** — for anything that must win over dockview chrome (tabs especially),
   verify against the *rendered* result, not the specificity you reasoned about. This exact trap
   cost e06b two CI rounds and produced a confident-but-false claim.
6. Prove, don't assert: a test must fail if a component is reverted to raw values or a forked
   styling path. Two runs this wave shipped claims that measurement falsified.
7. Known flakes: CE #319 (fixed, x3) / #322 / #335 + the self-hosted-Windows CEF env family.

## Definition of Done

- [ ] All 12 component families of 06 §3 exist and render from tokens; a theme switch (e06b)
      restyles them with no component change
- [ ] Families with an e06c1 role primitive **reuse it** (one visual contract, no forked path);
      any deliberate exception is recorded with its reason
- [ ] Tokens-only lint covers the new sources and stays blocking + non-vacuous
- [ ] Interactive families are keyboard-reachable with correct roles/labels (dialogs + tooltips
      focus-handled), verified by test — not deferred to e16
- [ ] The published kit entry point for package authors is explicit and documented
- [ ] `context_assert_shell_boundary` passes non-vacuously; FORBIDDEN list untouched
- [ ] T1 kit tests; tests same PR (R-QA-013); 3-OS CI green

## Links

- Split from: [[e06c-component-kit]] · builds on: [[e06c1-kit-foundation-role-widgets]]
- First consumer: [[e06d-settings-config]] · related: [[e06b-theme-engine]]
- Design: `06-theme-design-system.md` §3, `04-editor-web-app-docking-panels.md` §4
