// @context-engine/editor-kit — THE PUBLISHED ENTRY POINT of the editor component kit
// (M9 e06c1 + e06c2; design 06 §3, 04 §4).
//
// WHAT THE KIT IS. Design 06 §3: "editor-core ships the kit (buttons, fields, tabs, trees, tables,
// chips, badges, toasts, empty-states, skeletons, dialogs, tooltips) consuming tokens only. The
// hydration runtime's widget classes (04 §4) are kit components — so C++-modeled panels and iframe
// panels look identical. The kit + tokens are published to package authors; policy: tokens mandatory,
// kit strongly recommended."
//
//   * e06c1 built the FOUNDATION and closed the widget half: the twelve `ctx-widget-*` classes
//     (`widgets.ts` + `styles/kit.css`), the blocking tokens-only lint, and the three-way closed-set
//     gate over C++ / TS / CSS.
//   * e06c2 adds the AUTHORED FAMILIES — the twelve named above — as framework-free factories in this
//     package, each returning real DOM. Six of them BUILD ON an e06c1 role primitive rather than
//     forking one (see `COMPONENT_FAMILIES.reusesRoles`), which is what makes an authored `Button`
//     and a hydrated `ctx-widget-button` the same button rather than two that merely look alike.
//
// THIS FILE IS THE SURFACE A PACKAGE AUTHOR PROGRAMS AGAINST. Everything a consumer needs is exported
// here: the family factories, the widget-class map (for a package that renders uitree-shaped markup
// itself), the stylesheet FILE NAMES it must link, and `COMPONENT_FAMILIES` — the machine-readable
// roster of what the kit ships. Nothing else in the package is part of the contract; `dom.ts` in
// particular is internal, precisely because a helper that accepted a colour or a length would be the
// hole in "tokens only".
//
// TWO GATES KEEP THE SURFACE HONEST, and both are SOURCE scans that ride the `webui-*` family on all
// three `build` legs (no browser, no build artifact):
//
//   * `webui-kit-family-coverage` reads `COMPONENT_FAMILIES` below and asserts it names EXACTLY the
//     twelve families of design 06 §3, that every declared factory really is exported by a kit
//     module, that every declared root class really is styled by a kit stylesheet, and that every
//     `reusesRoles` entry is a member of the closed `WIDGET_CLASSES` set. A family that were quietly
//     dropped, renamed, or left unstyled is a red build rather than a gap someone notices at e06d.
//   * `webui-kit-source-tokens` scans these TS sources for appearance smuggled past the stylesheet —
//     an `element.style.*` write, a `style` attribute, a runtime `<style>`/`insertRule`, or a raw
//     colour in a string literal. The CSS lint cannot see any of those, and they are the obvious way
//     a component author bypasses it.
//
// The BROWSER half — that a family actually resolves to the active theme's tokens, that an authored
// button computes identically to a hydrated one, and that the interactive families are keyboard-
// operable — is `core/src/test/kit_components.test.ts` in the e07a `webui-ts-*` tier. Neither tier
// subsumes the other: a source scan cannot see the cascade, and a browser tier cannot run on the
// three default `build` legs.

export * from "./widgets.js";
export * from "./button.js";
export * from "./field.js";
export * from "./tabs.js";
export * from "./collection.js";
export * from "./status.js";
export * from "./feedback.js";
export * from "./overlay.js";

// Re-exported from the internal helper module because a consumer's own code has to be able to NAME a
// tone when it calls `createBadge` / `createChip` / `KitToastRegion.show`. Nothing else in `dom.ts`
// is part of the surface.
export { TONE_ATTRIBUTE, type SemanticTone } from "./dom.js";

/**
 * The hydration widget layer's stylesheet, served beside `app.css` under `context-editor://app/`.
 *
 * Named here rather than spelled out at each use so the T1 tier can FIND the sheet it is asserting
 * about instead of hardcoding a path that a CMake rename would silently orphan — a test that cannot
 * find the stylesheet must fail loudly, not pass vacuously.
 */
export const KIT_STYLESHEET = "kit.css";

/** The authored families' stylesheet (e06c2), served beside `kit.css`. Same naming discipline. */
export const KIT_COMPONENT_STYLESHEET = "components.css";

/**
 * Every stylesheet a consumer of this kit must link, IN ORDER.
 *
 * Order is part of the contract: `components.css` may only ADD to what `kit.css` decides, and the two
 * are written so that nothing depends on which the browser reaches last — a component that overrides
 * a widget rule does so with a COMPOUND selector (`.ctx-widget-status.ctx-badge`, specificity 0,2,0),
 * never by arriving second. That keeps the e06c1 "document order must not decide appearance" property
 * true across both sheets.
 */
export const KIT_STYLESHEETS: readonly string[] = [KIT_STYLESHEET, KIT_COMPONENT_STYLESHEET];

/** One entry of the kit's published component roster. */
export interface ComponentFamily {
    /** The family's name in design 06 §3, verbatim. */
    readonly family: string;
    /** The primary factory a consumer calls. Other factories in the same family are listed in README. */
    readonly factory: string;
    /** The class the family's root element carries — what `styles/components.css` styles. */
    readonly rootClass: string;
    /**
     * The e06c1 hydration ROLES this family builds on, or `[]` when it is genuinely net-new.
     *
     * A non-empty list is a CLAIM the gates check: every entry must be a member of `WIDGET_CLASSES`,
     * and `kit_components.test.ts` proves in a real browser that the authored element computes the
     * same paint as the hydrated widget. It is how "do not fork a parallel implementation" stops
     * being a review note.
     */
    readonly reusesRoles: readonly string[];
}

/**
 * THE ROSTER — the twelve component families of design 06 §3, in the design's own order.
 *
 * `webui-kit-family-coverage` compares this to its own independently-held copy of the design's list,
 * so the two cannot drift into agreeing with each other; the tool's copy is the in-repo record of a
 * design document that lives outside this repository.
 */
export const COMPONENT_FAMILIES: readonly ComponentFamily[] = [
    { family: "buttons", factory: "createButton", rootClass: "ctx-button", reusesRoles: ["button"] },
    {
        family: "fields",
        factory: "createTextField",
        rootClass: "ctx-field",
        reusesRoles: ["textbox", "checkbox"],
    },
    { family: "tabs", factory: "createTabs", rootClass: "ctx-tabs", reusesRoles: [] },
    {
        family: "trees",
        factory: "createTree",
        rootClass: "ctx-tree",
        reusesRoles: ["tree", "treeitem", "list", "listitem"],
    },
    { family: "tables", factory: "createTable", rootClass: "ctx-table", reusesRoles: [] },
    { family: "chips", factory: "createChip", rootClass: "ctx-chip", reusesRoles: [] },
    { family: "badges", factory: "createBadge", rootClass: "ctx-badge", reusesRoles: ["status"] },
    { family: "toasts", factory: "createToastRegion", rootClass: "ctx-toast", reusesRoles: [] },
    {
        family: "empty-states",
        factory: "createEmptyState",
        rootClass: "ctx-empty-state",
        reusesRoles: [],
    },
    { family: "skeletons", factory: "createSkeleton", rootClass: "ctx-skeleton", reusesRoles: [] },
    { family: "dialogs", factory: "createDialog", rootClass: "ctx-dialog", reusesRoles: [] },
    { family: "tooltips", factory: "createTooltip", rootClass: "ctx-tooltip", reusesRoles: [] },
];
