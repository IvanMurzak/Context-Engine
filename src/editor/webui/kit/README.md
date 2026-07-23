# `src/editor/webui/kit/` — `@context-engine/editor-kit`

The M9 editor **component kit** (design `06-theme-design-system.md` §3, `04-editor-web-app-docking-panels.md` §4).
A sibling workspace package of `core/` and `tokens/`, built by the same e05a toolchain — no npm
install, no Node at runtime.

Design 06 §3, in full: *"editor-core ships the kit (buttons, fields, tabs, trees, tables, chips,
badges, toasts, empty-states, skeletons, dialogs, tooltips) consuming tokens only. The hydration
runtime's widget classes (04 §4) are kit components — so C++-modeled panels and iframe panels look
identical. The kit + tokens are published to package authors; policy: **tokens mandatory, kit strongly
recommended** — a package panel on raw HTML still inherits colors/type/shape via injected tokens."*

## What the kit ships

| Piece | Where |
|---|---|
| The published entry point | `src/index.ts` — the family factories, `COMPONENT_FAMILIES`, `KIT_STYLESHEETS`, and the widget-class surface |
| The closed 12-role hydration widget layer (e06c1) | `src/widgets.ts` + `styles/kit.css` — served as `context-editor://app/kit.css` |
| The twelve authored component families (e06c2) | `src/{button,field,tabs,collection,status,feedback,overlay}.ts` + `styles/components.css` — served as `context-editor://app/components.css` |
| The blocking tokens-only lint (CSS) | `tools/check_kit_tokens.py --tokens-only` → ctest **`webui-kit-tokens-only`** |
| The blocking tokens-only lint (TS) | `tools/check_kit_tokens.py --source-tokens` → ctest **`webui-kit-source-tokens`** |
| The closedness + one-owner gate | `tools/check_kit_tokens.py --role-coverage` → ctest **`webui-kit-role-coverage`** |
| The twelve-family roster gate | `tools/check_kit_tokens.py --family-coverage` → ctest **`webui-kit-family-coverage`** |
| The T1 browser tiers | `core/src/test/kit.test.ts` (widget layer) + `core/src/test/kit_components.test.ts` (families) → ctest `webui-ts-unit` |

## The published surface (what a package author programs against)

Everything a consumer needs is exported from `src/index.ts`; nothing else in the package is part of
the contract (`dom.ts` in particular is internal — a helper that accepted a colour or a length would
be the hole in "tokens only").

```ts
import { createButton, createDialog, KIT_STYLESHEETS } from "@context-engine/editor-kit";
```

A consumer must link the sheets named by `KIT_STYLESHEETS`, **in order** (`kit.css`, then
`components.css`). Order is documented but nothing is allowed to *depend* on it — see § One styling
owner below.

### The twelve families

`COMPONENT_FAMILIES` is the machine-readable roster; this table is its prose form. "Builds on" names
the e06c1 hydration role the family reuses rather than forks — the authored element carries the
primitive's own `ctx-widget-*` class, so both resolve to one visual contract decided in `kit.css`.

| Family | Primary factory | Also | Root class | Builds on |
|---|---|---|---|---|
| buttons | `createButton` | — | `.ctx-button` | `button` |
| fields | `createTextField` | `createCheckboxField`, `createSelectField` | `.ctx-field` | `textbox`, `checkbox` |
| tabs | `createTabs` | — | `.ctx-tabs` | — |
| trees | `createTree` | `createList` | `.ctx-tree` | `tree`, `treeitem`, `list`, `listitem` |
| tables | `createTable` | — | `.ctx-table` | — |
| chips | `createChip` | — | `.ctx-chip` | — |
| badges | `createBadge` | — | `.ctx-badge` | `status` (opt-in — see below) |
| toasts | `createToastRegion` | — | `.ctx-toast` | — |
| empty-states | `createEmptyState` | — | `.ctx-empty-state` | — |
| skeletons | `createSkeleton` | — | `.ctx-skeleton` | — |
| dialogs | `createDialog` | — | `.ctx-dialog` | — |
| tooltips | `createTooltip` | — | `.ctx-tooltip` | — |

The roster is not decoration: `webui-kit-family-coverage` reads it and fails the build when a family
is missing from it, when the design names one it does not, when a declared factory is not exported,
when a declared root class is styled by no kit stylesheet, or when a `reusesRoles` entry names a role
outside the closed `WIDGET_CLASSES` set. That the reuse then *resolves the same* is a browser
question, answered by `kit_components.test.ts` on the computed style of an authored `Button` against a
hand-built `ctx-widget-button`.

## Accessibility is part of the component, not a later pass

e16 audits a11y; it must not be *fixing* the kit. Every interactive family therefore ships its
keyboard and semantic contract, and each one is asserted in the browser tier:

- **tabs** — the ARIA tablist pattern: one tab stop for the whole strip (roving `tabindex`),
  ArrowLeft/Right + Home/End move selection *and* focus, `aria-selected` / `aria-controls` /
  `aria-labelledby` wired both ways.
- **trees** — the ARIA tree pattern: one tab stop, Arrow keys walk the *visible* items,
  ArrowRight/Left expand and collapse, Enter activates, explicit `aria-level` / `aria-setsize` /
  `aria-posinset`.
- **lists** — deliberately *not* a composite widget: activatable rows are real `<button>` elements,
  one tab stop each. Short lists (recent projects, a settings index) do not need roving state, and
  the browser supplies Enter/Space activation for free.
- **dialogs** — a native `<dialog>` driven by `showModal()`, so the rest of the document is genuinely
  **inert** (not a Tab-cycling trap a screen reader's virtual cursor walks straight past). The kit
  adds deterministic initial focus and focus restoration to the opener, and handles Escape twice — the
  native `cancel` event *and* a `keydown`, because a synthetic event cannot drive native behaviour and
  an untestable a11y contract is one that silently regresses.
- **tooltips** — shown on **focus** as well as hover (a hover-only tooltip does not exist for a
  keyboard or touch user), dismissible with Escape (WCAG 1.4.13), and hoverable because the bubble
  lives inside the hover host rather than behind a timer. `aria-describedby`, never `aria-labelledby`.
- **toasts** — two permanently-mounted live lanes, `polite` (`role="status"`) and `assertive`
  (`role="alert"`), because a live region must exist *before* its content arrives. Only the `bad` tone
  interrupts: `warn` means ACTIVE WORK in this design language (06 §2), not danger.
- **tables** — a real `<table>` with a `<caption>`, `scope`d column *and row* headers, and `aria-sort`
  on every sortable column (`none` while unsorted, so a sortable column stays distinguishable from a
  fixed one). The sort control is a real `<button>`.
- **fields** — a real `<label for>` (a click target, which `aria-label` is not), and
  `aria-describedby` wired only when a description actually exists.

The kit's key handlers are in the jurisdiction of the `webui-no-raw-key-handlers` lint (05 §6),
which scans **both** `core/src` and `kit/src` since e06c2; each carries a `key-handler-ok:` marker
naming why it is intra-widget ARIA navigation rather than an editor shortcut.

## The tokens-only lints — jurisdiction, stated

Design 06 §1: *"components reference ONLY semantic tokens; raw values live in themes alone."*

**CSS (`--tokens-only`).** Every `*.css` under `kit/styles/` — so `components.css` inherited the gate
by existing. Inside a kit stylesheet the lint rejects

- **any raw colour** — hex, `rgb()`/`rgba()`/`hsl()`/`hsla()`, or a named CSS colour — in any
  property, in any position;
- **a raw font value** in `font` / `font-family` / `font-size` / `font-weight` / `letter-spacing` /
  `line-height` / `font-variant-numeric`;
- **a raw length** in the shape family — `border*`, `outline*`, `border-radius`;
- **a raw time or easing** in `transition*` / `animation*`;
- **any `var()` fallback at all**, and any `var(--x)` that is not a `--ctx-` token.

Keywords (`inherit`, `none`, `currentColor`, `transparent`, …) are values, not literals; `calc()` over
a token is allowed.

**TypeScript (`--source-tokens`, new in e06c2).** The CSS lint reads CSS. e06c2's families are
TypeScript that *builds elements*, and TypeScript can put appearance where no CSS lint has ever
looked. So every `*.ts` under `kit/src/` is scanned for an inline-style write (`element.style.*`,
`setProperty`, `cssText`), a `style` attribute, a runtime `<style>` element or CSSOM mutation, and a
raw hex/`rgb()` colour inside a string literal. Named CSS colours are deliberately **not** scanned for
here: in a CSS value position `red` can only be a colour, but in a TypeScript string it is prose.

Both are **proven non-vacuous**, not asserted to be: `tools/tests/test_check_kit_tokens.py` plants
each violation and requires exit 1 — including the one-character bypass the naive TS implementation
had (a regex stripping `//` to end-of-line also eats the contents of a string containing `//`, so the
scan tracks string state instead). `core/src/test/kit_components.test.ts` adds the runtime
counterpart: no element any factory built carries a `style` attribute.

## One styling owner — stated at the level it is about

e06c1 enforced this as *"no other file declares a `.ctx-widget-` rule"*, which was the same thing
while the kit was one stylesheet. e06c2 made it two, and one authored family legitimately reaches a
widget class: a **live badge IS the `status` role primitive**, reused rather than copied.

The property was always about the *hazard*, not the file count: **appearance must never be decided by
document order**, which CMake staging and a vendored engine's runtime injection settle invisibly. So
the rule is now:

- no **non-kit** stylesheet may style a `ctx-widget-*` class at all (unchanged — `app.css` is checked
  by `--role-coverage`, and `kit.test.ts` re-checks every served sheet in the browser);
- inside the kit, only `kit.css` may style a widget class with a **bare** selector. Another kit sheet
  must compound a family class onto it — `.ctx-widget-status.ctx-badge` is specificity 0,2,0 and beats
  the widget layer's 0,1,0 whichever sheet the browser reaches last. A bare rule would *tie*.

Both halves are gated on every `build` leg (`--role-coverage`) and in the browser (`kit.test.ts`).

## Recorded findings (not silent scope reductions)

1. **A live badge is opt-in, and that is the one deliberate reuse exception.** Design 06 §2 names the
   "problems badge" among the surfaces bound to the status semantics, so the obvious reading is that
   *every* badge is a `ctx-widget-status`. That reading is quietly harmful: `<output>` is an ARIA live
   region, and an editor is full of badges that change for cosmetic reasons — a screen-reader user
   would be read a stream of numbers nobody asked for. `createBadge({ live: true })` renders the real
   primitive (the Problems count, a build verdict); the default renders a `<span>` with the identical
   painted appearance.
2. **`select` has no e06c1 role primitive.** The closed `uitree::Role` vocabulary has `textbox` and
   `checkbox` and no select/combobox member, because no C++ panel model emits one. e06d's Settings
   theme picker (06 §4) needs one, so `createSelectField` is net-new styling on a native `<select>`.
   **Follow-up:** when the C++ vocabulary grows a `select` role, add it to `WIDGET_CLASSES` +
   `kit.css` and re-point `.ctx-field__select` at it — `--role-coverage` will force that conversation
   rather than let two paths coexist quietly.
3. **Box spacing is not tokenised** (unchanged from e06c1). `padding` / `margin` / `gap` lengths are
   raw because e06a publishes no spacing token: design 06 §1 describes the density group as "control
   heights, **paddings**", but the shipped schema's `shape.density` is three control HEIGHTS, and
   `mockups/TOKENS.md` §3 records the 2/4/…/32px scale as a PROPOSAL "not named as a token group in
   doc 06 at all". Linting those lengths today would force `calc()` over a *height* token — a bypass
   wearing compliance. **Follow-up:** adopt the padding half of the density group in
   `@context-engine/editor-tokens` (schema + the four built-in themes + the T1 drift test), then widen
   the lint's value jurisdiction to `padding`/`margin`/`gap`.
4. **There is no LONG-LOOP motion duration token, so skeletons are static.** A shimmer needs a
   ~1.2–1.6s loop. `motion.duration` publishes the micro-transition steps (fast/base/slow, 120–160ms)
   plus the 350ms theme cross-fade; the only longer values in the schema are
   `motion.flourish.states.*.duration`, which are Pulse-of-Work *play-state* semantics (06 §2) and
   mean something specific. Animating at 160ms would be a strobe — an accessibility hazard, not a
   design choice — and `calc(var(--ctx-motion-duration-slow) * 10)` would be a raw value wearing a
   token's clothes. **Follow-up:** add an ambient/long-loop duration step to the `motion` group.
5. **There is no SCRIM opacity token.** The dialog backdrop composes the theme's `colors.canvas` with a
   raw `opacity`, which is outside the lint's value jurisdiction by construction — exactly as box
   spacing is, and for the same reason: no token exists. **Follow-up:** add an overlay/scrim token to
   the `colors` or `elevation` group.
6. **A `busyLabel` skeleton is a NAMED region, not a guaranteed announcement.** `createSkeleton({
   busyLabel })` sets `role="status"` + `aria-label`, which names the region for anyone who navigates
   into it. It does **not** reliably announce to a user who is elsewhere on the page, for exactly the
   reason `src/feedback.ts`'s own header gives for the toast lanes: a live region announces content
   changes observed *after* it is registered, and a skeleton is mounted complete, with its label as an
   attribute rather than as content. Stating this rather than letting the attribute imply parity with
   the toast region. **Follow-up:** if a panel genuinely needs the interruption, it wants a standing
   polite region it appends a sentence into (the `KitToastRegion` shape) — a caller contract change,
   deliberately not folded into e06c2.
7. **`app/app.css` still carries raw colour literals** in the welcome screen and the command palette
   (unchanged from e06c1). The pre-theme placeholder pair (`--editor-bg` / `--editor-fg`) is
   intentional and documented in that file. The welcome and palette surfaces are pre-kit UI.
   **Follow-up:** rebuild them on kit components (the palette is a dialog + list + fields; the welcome
   screen is an empty-state + list + buttons), at which point their rules move under `kit/styles/` and
   inherit the lints. Deliberately **not** done in e06c2: it is a behaviour change to two live
   surfaces with their own T1 suites and a live CEF pixel smoke calibrated against them, and folding
   it in would have made this PR's diff two features wide.

## Build + test

`kit/src/**` are inputs of the `context_editor_webui` target (`../CMakeLists.txt`, `CONFIGURE_DEPENDS`),
so a new module re-globs. Each stylesheet under `styles/` is staged into the served asset root beside
`app.css` and linked by `app/index.html`; each is also staged beside the T1 test bundle, because the
browser tiers assert what the browser actually COMPUTED and would pass vacuously without the real
stylesheets. **Adding a sheet means adding it to the CMake staging list AND to both HTML documents.**

```sh
cmake -S src --preset dev && cd src && cmake --build --preset dev && ctest --preset dev -R '^webui-'
```

The T1 browser tier is opt-in (it needs a browser), exactly as for the rest of `webui-ts-*`:

```sh
cmake -S src --preset dev -DCONTEXT_WEBUI_BROWSER_TESTS=ON
cmake --build src/build/dev --target context_editor_webui_test
CONTEXT_WEBUI_TEST_BROWSER="…/msedge.exe" ctest --preset dev -R '^webui-ts-'
cmake -S src --preset dev -DCONTEXT_WEBUI_BROWSER_TESTS=OFF   # the option is CACHED — flip it back
```
