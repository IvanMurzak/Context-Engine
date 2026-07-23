# `src/editor/webui/kit/` — `@context-engine/editor-kit`

The M9 editor **component kit** (design `06-theme-design-system.md` §3, `04-editor-web-app-docking-panels.md` §4).
A sibling workspace package of `core/` and `tokens/`, built by the same e05a toolchain — no npm
install, no Node at runtime.

## What e06c1 shipped (the foundation)

| Piece | Where |
|---|---|
| The module + its published surface | `src/index.ts` — `WIDGET_CLASSES`, `WIDGET_CLASS_PREFIX`, `KIT_STYLESHEET`, `widgetClassForRole` |
| The closed 12-role hydration widget layer | `styles/kit.css` — served as `context-editor://app/kit.css` |
| The blocking tokens-only lint | `tools/check_kit_tokens.py --tokens-only` → ctest **`webui-kit-tokens-only`** |
| The closedness + one-owner gate | `tools/check_kit_tokens.py --role-coverage` → ctest **`webui-kit-role-coverage`** |
| The T1 browser tier | `core/src/test/kit.test.ts` → ctest `webui-ts-unit` (the e07a `webui-tests` job) |

e06c2 adds the **authored component families** — tabs, tables, dialogs, toasts, chips, badges,
empty-states, skeletons, tooltips — into this same package. They inherit both gates by construction:
a new stylesheet under `styles/` is scanned by the lint with no edit anywhere, and a new module under
`src/` rides the same tsgo typecheck as the rest of the workspace.

## Why the kit is a package, not a module inside `core/`

Design 06 §3 puts the kit *and* the tokens on the publish path for package authors ("tokens
mandatory, kit strongly recommended"). A module inside `@context-engine/editor-core` could not be
offered to a package author without offering them the whole editor app — its bridge, its boot
sequence, its command registry. A sibling package draws the line where the design draws it, and it
costs nothing: `core/tsconfig.json` already type-checks a sibling package transitively through the
import (`theme.ts` → `../../tokens/src/index.js` is the same shape), so the kit rides the existing
typecheck, bundle and test wiring rather than adding a second toolchain seam.

The kit owns `WIDGET_CLASSES` (moved out of `core/src/hydration.ts`, which re-exports it, so no
importer changed). That move is what makes "one styling owner" checkable: the class names and the
rules that style them now live in one package, and a gate can compare them.

## The tokens-only lint — jurisdiction, stated

Design 06 §1: *"components reference ONLY semantic tokens; raw values live in themes alone."*

**Files, today:** every `*.css` under `kit/styles/`. e06c2's stylesheets land there and are covered
automatically. `core/`'s `app/app.css` is deliberately **outside** the lint — it is the app document's
base stylesheet (the pre-theme placeholder pair, the docking-surface re-point, the welcome screen,
the command palette), not kit source. It still carries **23 raw colour literals** (14 distinct);
none of them is a widget-layer value any more. See § Known gaps.

**Values:** inside kit stylesheets the lint rejects

- **any raw colour** — hex, `rgb()`/`rgba()`/`hsl()`/`hsla()`, or a named CSS colour — in any
  property, in any position;
- **a raw font value** in `font` / `font-family` / `font-size` / `font-weight` / `letter-spacing` /
  `line-height` / `font-variant-numeric`;
- **a raw length** in the shape family — `border*`, `outline*`, `border-radius` — which is exactly the
  family `shape` tokens cover (radius, border width, focus-ring width, selection-bar width);
- **a raw time or easing** in `transition*` / `animation*`, which `motion` tokens cover;
- **any `var()` fallback at all**, and any `var(--x)` that is not a `--ctx-` token. A fallback is a
  raw value smuggled past the first three rules, and a kit-local `--` variable is a second source of
  truth for something the theme owns.

Keywords (`inherit`, `initial`, `unset`, `revert`, `none`, `normal`, `currentColor`, `transparent`,
`auto`) are values, not literals, and are allowed; `calc()` over a token is allowed, which is how the
focus ring's negative offset is derived from the ring width instead of restated as `-2px`.

**Proven non-vacuous**, the way the e05d3 boundary gate was: `tools/tests/test_check_kit_tokens.py`
plants a raw colour, a raw font size, a raw border radius, a named colour and a `var()` fallback into
a temporary kit stylesheet and asserts the lint EXITS 1 on each — so the proof runs on every
`python-tests` CI job, not once by hand at authoring time.

### Known gaps (findings, not silent scope reductions)

1. **Box spacing is not tokenised.** `padding` / `margin` lengths in `kit.css` are raw (`2px 6px`,
   `0 0 6px`, `8px`). e06a publishes no spacing or padding token: design 06 §1 describes the density
   group as "control heights, **paddings**", but the shipped schema's `shape.density` is three control
   HEIGHTS (`compact`/`default`/`comfortable`), and `mockups/TOKENS.md` §3 records the 2/4/6/8/10/12/
   16/20/24/32px spacing scale as a PROPOSAL "not named as a token group in doc 06 at all". Linting
   those lengths today would force `calc()` over a *height* token — a bypass wearing compliance.
   **Follow-up:** adopt the padding half of the density group in `@context-engine/editor-tokens`
   (schema + the four built-in themes + the T1 drift test), then widen this lint's value jurisdiction
   to `padding`/`margin`/`gap`.
2. **`app/app.css` still carries 23 raw colour literals** in the welcome screen, the command palette
   and the pre-theme placeholder pair. The placeholder pair (`--editor-bg` / `--editor-fg`) is
   intentional and documented in that file — it is what a document looks like before the bundle
   applies a theme. The welcome and palette surfaces are not: they are pre-kit UI that e06c2 (chips,
   dialogs, empty-states) and e06d (the Settings panel) are the natural homes for.
   **Follow-up:** rebuild those two surfaces on kit components in e06c2/e06d, at which point they
   move under `kit/styles/` and inherit the lint.

## Build + test

`kit/src/**` and `kit/styles/**` are inputs of the `context_editor_webui` target
(`../CMakeLists.txt`, `CONFIGURE_DEPENDS`), so a new file re-globs. `styles/kit.css` is staged into
the served asset root beside `app.css` and linked by `app/index.html`; it is also staged beside the
T1 test bundle, because `core/src/test/kit.test.ts` asserts what the browser actually COMPUTED and
would pass vacuously without the real stylesheet.

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
