// The HYDRATION WIDGET LAYER's surface (M9 e06c1, design 04 §4 step 4) — the twelve presentation-only
// classes the hydration runtime puts on a C++ panel's uitree nodes.
//
// ⚠ MOVED HERE FROM `index.ts` BY e06c2, AND THE MOVE IS LOAD-BEARING RATHER THAN COSMETIC. e06c2's
// authored families (`button.ts`, `field.ts`, `collection.ts`, …) BUILD ON these classes — that is the
// spec's "an authored `Button` and a hydrated `ctx-widget-button` must resolve to the same visual
// contract, not a forked implementation" — so every one of them has to import the map. Leaving it in
// `index.ts`, which re-exports those same modules, would make the kit's own barrel a member of an
// import CYCLE: each family module would read a `const` from a module that is still initialising.
// Function-body reads happen to survive that, which is exactly what makes it a bad thing to rely on —
// the failure would be a temporal-dead-zone throw in the BROWSER, visible only on the `webui-ts-unit`
// leg, for a hazard that a five-line file removes outright.
//
// `index.ts` re-exports everything here, so every existing importer (`core/src/hydration.ts`,
// `core/src/test/kit.test.ts`) is unchanged; only `webui-kit-role-coverage`'s `--kit-source` argument
// now names this file, because the gate parses the literal rather than resolving the module graph.
//
// THE CLOSEDNESS IS LOAD-BEARING, WHICH IS WHY IT IS ASSERTED. `uitree::render_html` emits only the
// twelve tags `role_html_tag` maps to, and `HydrationRuntime.#normalise` looks a node's role up in
// this map — so a role the kit does not style renders UNTHEMED and silently. `check_kit_tokens.py
// --role-coverage` turns that into a red build the moment the C++ vocabulary grows a thirteenth role,
// and `kit.test.ts` proves in a real browser that each class resolves to the active theme's tokens.

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

/** The kit class for a uitree role, or `undefined` for a role the kit does not know. */
export function widgetClassForRole(role: string): string | undefined {
    return WIDGET_CLASSES[role];
}

/**
 * The kit class for a role the CALLER knows must exist — throwing rather than degrading when it does
 * not.
 *
 * This is what the authored families use, and the throw is the point. A family that fell back to `""`
 * on a renamed role would silently emit an element with no widget class: it would still render, still
 * be interactive, and would simply stop being themed — the precise silent failure the closed-set gate
 * exists to prevent, re-introduced one layer up. Failing loudly at construction turns it into a
 * `webui-ts-unit` red on the first mount instead.
 */
export function requireWidgetClass(role: string): string {
    const widgetClass = WIDGET_CLASSES[role];
    if (widgetClass === undefined) {
        throw new Error(
            `@context-engine/editor-kit: no widget class for role "${role}" — an authored component ` +
                `family claims to build on a hydration role the closed set does not contain`,
        );
    }
    return widgetClass;
}
