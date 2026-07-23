// @context-engine/editor-kit — the component KIT's module surface (M9 e06c1, design 06 §3 / 04 §4).
//
// WHAT THE KIT IS. Design 06 §3: "editor-core ships the kit … consuming tokens only. The hydration
// runtime's widget classes (04 §4) ARE kit components — so C++-modeled panels and iframe panels look
// identical." e06c1 builds the FOUNDATION of that and closes the widget half; e06c2 adds the authored
// component families (tabs, tables, dialogs, toasts, chips, badges, empty-states, skeletons,
// tooltips) into this same package, on top of what is here.
//
// WHY THIS FILE OWNS `WIDGET_CLASSES` RATHER THAN hydration.ts. The map lived in the hydration
// runtime through e05d1, and `app/app.css` styled nine of its twelve classes — two owners, in two
// packages, with nothing tying them together: a role could gain a class and never gain an appearance,
// and no gate anywhere would notice. Moving the map HERE, beside the stylesheet that styles it, makes
// the kit the SINGLE owner of the widget layer — class names and appearance in one package — and lets
// `tools/check_kit_tokens.py --role-coverage` assert the three-way agreement mechanically (the C++
// `uitree::role_token` vocabulary ↔ this map ↔ `styles/kit.css`'s selectors) on every `build` leg.
// `hydration.ts` re-exports it, so every existing importer and the `index.ts` barrel are unchanged.
//
// THE CLOSEDNESS IS LOAD-BEARING, WHICH IS WHY IT IS ASSERTED. `uitree::render_html` emits only the
// twelve tags `role_html_tag` maps to, and `HydrationRuntime.#normalise` looks a node's role up in
// this map — so a role the kit does not style renders UNTHEMED and silently. The gate above turns
// that into a red build the moment the C++ vocabulary grows a thirteenth role, and `kit.test.ts`
// (the e07a `webui-ts-*` tier) proves in a real browser that each class actually resolves to the
// active theme's tokens.
//
// NO RAW VALUES LIVE IN THIS PACKAGE. Every appearance in `styles/kit.css` is a `var(--ctx-*)` the
// e06b theme engine writes from an e06a theme document — no literal, and not even a `var()` FALLBACK
// (a fallback is a second source of truth for a value the theme already owns). The
// `webui-kit-tokens-only` ctest enforces that on all three `build` legs; `README.md` records the
// jurisdiction it covers today and the one gap it deliberately does not.

/**
 * Presentation-only widget classes, keyed by node ROLE (04 §4 step 4).
 *
 * The VALUE semantics stay in the C++ model and the write path — these classes carry appearance and
 * nothing else. Keyed by role because role is what the uitree vocabulary actually publishes
 * (`uitree::role_token`); keying on anything derived from a panel id would be the special-casing the
 * hydration runtime exists to avoid, and would put a panel id in the kit.
 *
 * ⚠ CLOSED SET. These twelve are exactly `uitree::Role`, and `webui-kit-role-coverage` fails if this
 * map, the C++ vocabulary and `styles/kit.css` ever disagree in EITHER direction. Adding an entry
 * here without a kit rule for it is a red build, not an unthemed widget nobody notices.
 */
export const WIDGET_CLASSES: Readonly<Record<string, string>> = {
    tree: "ctx-widget-tree",
    treeitem: "ctx-widget-treeitem",
    list: "ctx-widget-list",
    listitem: "ctx-widget-listitem",
    button: "ctx-widget-button",
    textbox: "ctx-widget-textbox",
    checkbox: "ctx-widget-checkbox",
    heading: "ctx-widget-heading",
    status: "ctx-widget-status",
    region: "ctx-widget-region",
    group: "ctx-widget-group",
    text: "ctx-widget-text",
};

/** The prefix every widget class carries. The lint reads it; so does the "one owner" app.css scan. */
export const WIDGET_CLASS_PREFIX = "ctx-widget-";

/**
 * The kit stylesheet's served file name, beside `app.css` under `context-editor://app/`.
 *
 * Named here rather than spelled out at each use so the T1 tier can FIND the sheet it is asserting
 * about instead of hardcoding a path that a CMake rename would silently orphan — a test that cannot
 * find the stylesheet must fail loudly, not pass vacuously.
 */
export const KIT_STYLESHEET = "kit.css";

/** The kit class for a uitree role, or `undefined` for a role the kit does not know. */
export function widgetClassForRole(role: string): string | undefined {
    return WIDGET_CLASSES[role];
}
