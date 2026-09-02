# 07 — The four-state design system, and the duplicated title

Covers tasks `a3`, `a4`. Read before touching `kit/styles/kit.css`, `kit/styles/components.css`,
`app/app.css`, or the Dockview theme bridge.

---

## 1. What is missing, measured

`src/editor/webui/kit/styles/kit.css` styles the twelve closed `uitree::Role` widget classes — the
content of **every C++-modelled panel**. Counted in the file:

| Selector | Count | Where |
|---|---|---|
| `:hover` | **3** | `:94` listitem, `:95` treeitem, `:115` button |
| `:active` | **0** | — |
| `:disabled` / `[aria-disabled]` | **0** | — |
| `transition` | **0** | — |
| `:focus-visible` | **12** | `:163-174`, every role |

The motion tokens exist and are used one layer over, in the kit *component* sheet:
`components.css:52,149,212` already spell
`transition: <prop> var(--ctx-motion-duration-fast) var(--ctx-motion-easing-standard)`, and
`components.css:55` is the kit's one `:disabled` rule.

So this is not "invent a design system" — it is **propagate the one the component layer already
uses** down to the widget layer and out to the chrome.

## 2. The state model

Five states, not four: the owner asked for Normal / Hover / Pressed / Disabled, and `:focus-visible`
already exists on all twelve roles and **must survive** — it is the R-A11Y-001 requirement made
visible, and widening it from three roles to twelve was a deliberate past fix.

| State | Selector | Rule |
|---|---|---|
| Normal | base | the resting token set |
| Hover | `:hover` | one surface step up. The elevation model is **border + surface step, never a shadow**, and never an opacity change — a hover that brightened by opacity dilutes the high-contrast themes' AA+ ink ratio, which is the exact mistake `kit.css` already documents having corrected once |
| Pressed | `:active` | one further step, or the accent-tinted step; must be visibly distinct from hover, not merely darker by a hair |
| Disabled | `:disabled` **and** `[aria-disabled="true"]` | both, always. `app.css:389-391` records why: the ARIA menu pattern keeps a disabled item **focusable**, so `:disabled` alone misses it |
| Focus | `:focus-visible` | **unchanged**. Do not fold it into the others; it composes with them |

Transitions on colour-ish properties only (`background-color`, `border-color`, `color`), at
`--ctx-motion-duration-fast` with `--ctx-motion-easing-standard`. Not on `transform`, not on layout
properties — a docked panel re-laying out under a transition is a jitter source, and the composite is
damage-driven.

## 3. The three constraints that will bite

1. **`webui-kit-tokens-only`** rejects a raw literal *anywhere* in `kit.css`, **including inside a
   `var()` fallback**. Every new value is a token. If a state needs a step the theme does not publish,
   the answer is a theme token (an e06a schema change), **not** a `calc()` over an existing one —
   that is a bypass wearing compliance, and the file's header says so.
2. **`webui-kit-role-coverage`** asserts every `Role` is styled in `kit.css` and that **no
   `.ctx-widget-` rule survives in `app.css`**. Adding a state rule in the wrong file is a red build,
   which is the intended outcome.
3. **Reduced motion is unconditional.** `theme.ts:306-335` collapses every `--ctx-motion-duration-*`
   and every flourish duration to 0 when `prefers-reduced-motion: reduce` matches, whatever the theme
   asked for, and `app.css:163` re-asserts it. Because durations are read from tokens, new transitions
   are inert under reduced motion **for free** — do not add a second media query to "handle" it.

## 4. The Dockview tab strip

The owner explicitly asked for the draggable tab strip to react on hover like the buttons do.

Dockview is themed through the `--dv-*` bridge at `app.css:667`. Before editing it, read the ⚠⚠⚠ note
above it: `dockview.css` declares the same variables on the same `.dockview-theme-dark` selector
(specificity 0,1,0) and dockview-core injects its stylesheet at runtime, so a bare
`.dockview-theme-dark` block here is **silently ignored** for every variable dockview itself declares.
Only the `html`-prefixed selector (0,1,1) wins. This was measured once already — only
`--dv-background-color` survived, because it is the one variable `dockview.css` never declares.

Practical consequence: express the tab hover through the `--dv-*` variables dockview reads, under the
`html`-prefixed selector, and confirm on the **rendered** result rather than by reading the selector.
`theme.ts`'s `DOCKVIEW_CHROME` table is the same map and must move with it.

Note also that `app.css:639` records a T1 test coupled to the tab strip's surface step — giving the
strip its own step means moving those constants in the same diff.

## 5. `a4` — the duplicated panel title

Every C++ panel emits a `Role::heading` as the first child of the panel root (nine sites, listed in
`01` §10), and Dockview renders `manifest.title` in the tab as well. **Eight** of the nine set the
heading's *text* to their roster title and are therefore visibly duplicated; Tilemap Painter sets only
an accessible label (`tilemap_paint_panel.cpp:60`), so it renders an empty `<h2>` and duplicates
nothing — the rule below correctly leaves it alone.

**The fix is renderer-side, not model-side** (planner ruling, `README.md`). The heading is not
decorative: `role_requires_name(Role::heading)` is true, it renders as `<h2>`, headless and CLI
consumers read the same model, and `gui-a11y-coverage` gates the roster. Deleting nine headings to fix
a visual duplication would spend an accessibility landmark on a CSS problem.

So: in the hydration runtime, when a panel's **first** rendered node is a `heading` whose text equals
that panel's `manifest.title`, mark it visually hidden — present in the accessibility tree, absent from
the picture.

Two constraints:

- **Visually hidden, not `display: none`.** `display:none` removes it from the accessibility tree,
  which is the whole thing being preserved. Use the standard clip-rect pattern.
- **Match on the rendered text, not on a hardcoded node-id list.** The ids differ per panel
  (`scenetree.heading`, `inspector.heading`, `tilemap.title`, …) and a package panel will have its own.
  A rule keyed to specific panels is exactly the special-casing `kit.css`'s header forbids: *"NO PANEL
  ID APPEARS BELOW, and none may."*

Test in the `webui-ts-*` browser tier over the real DOM: the heading is **not** visible, and it **is**
still reachable as an accessible heading with its name. Both halves — one alone would pass with the
node deleted.
