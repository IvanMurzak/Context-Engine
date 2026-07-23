// The AUTHORED COMPONENT FAMILIES' T1 tier (M9 e06c2; design 06 §3, 04 §4). The half of e06c2's DoD
// that only a real browser can answer.
//
// WHAT THE SOURCE GATES ALREADY PROVE, so this file does not re-prove it: that the kit ships exactly
// the twelve families of 06 §3, each with an exported factory, a styled root class and a reuse claim
// naming a real hydration role (`webui-kit-family-coverage`); that no kit stylesheet carries a raw
// value and no kit module smuggles one past it (`webui-kit-tokens-only` + `webui-kit-source-tokens`);
// that the closed 12-role widget layer agrees across C++, TS and CSS (`webui-kit-role-coverage`).
// All four run on every `build` leg, with no browser.
//
// WHAT ONLY A BROWSER CAN ANSWER, and what is therefore here:
//
//   1. Did the authored stylesheet actually get SERVED, and does it style each family? A rule that
//      exists in the repo and never reaches the renderer lints perfectly and paints nothing.
//   2. Does an authored `Button` COMPUTE the same paint as a hydrated `ctx-widget-button`? "Reuses
//      the primitive" is a claim about the resolved cascade — the one form of it a reader cannot
//      talk themselves out of, and the difference between building on e06c1 and forking it.
//   3. Does a theme switch restyle every family with no component change? That is the DoD's first
//      line, and it is false the moment one family resolves to an inherited or user-agent value.
//   4. Are the interactive families actually KEYBOARD-OPERABLE — not "do they carry a role
//      attribute", which is what an audit that never pressed a key reports. Tabs move on arrows and
//      own ONE tab stop; a modal dialog makes the rest of the document genuinely inert and restores
//      focus to its opener; a tooltip appears on FOCUS and dismisses on Escape; a toast reaches the
//      right politeness lane.
//
// ⚠ THE ENGINE IS DRIVEN WITH REDUCED MOTION ON, for the reason `kit.test.ts` and `theme_dom.test.ts`
// both paid for: app.css arms a `transition` on `html[data-theme-transition="true"] *` for the
// cross-fade's duration, so a synchronous `getComputedStyle` right after a SWITCH reports the colour
// the transition is animating AWAY from. Reduced motion collapses every duration to 0ms — the
// engine's own documented, unconditional override (06 §1) — which makes the resolved value
// observable the instant it is applied.

import { assert, assertEqual, type TestCase } from "./harness.js";
import {
    COMPONENT_FAMILIES,
    KIT_COMPONENT_STYLESHEET,
    WIDGET_CLASSES,
    createBadge,
    createButton,
    createCheckboxField,
    createChip,
    createDialog,
    createEmptyState,
    createList,
    createSelectField,
    createSkeleton,
    createTable,
    createTabs,
    createTextField,
    createToastRegion,
    createTooltip,
    createTree,
} from "../../../kit/src/index.js";
import { BUILTIN_DARK, BUILTIN_LIGHT, REDUCED_MOTION_QUERY, ThemeEngine } from "../theme.js";

import darkTheme from "../../../tokens/themes/dark.theme.json";
import lightTheme from "../../../tokens/themes/light.theme.json";

type Doc = Readonly<Record<string, unknown>>;

/** The computed properties this file asserts on. Narrow on purpose: each is a keyed read. */
type StyleProperty =
    | "color"
    | "backgroundColor"
    | "borderTopColor"
    | "borderBottomColor"
    | "borderTopLeftRadius"
    | "fontFamily";

/**
 * A colour off a theme DOCUMENT — `colors.<key>`, or `colors.semantic.<key>` when `semantic`.
 *
 * One reader for both groups rather than two near-identical ones: the only difference was which
 * object to index, and the `semantic` flag is already carried per-expectation.
 */
function colour(document_: Doc, key: string, semantic: boolean): string {
    const colors = document_["colors"] as Record<string, unknown> | undefined;
    const group = semantic
        ? (colors?.["semantic"] as Record<string, unknown> | undefined)
        : colors;
    const value = group?.[key];
    // Asserted rather than defaulted: a theme that stopped publishing the token must fail the case
    // loudly, not quietly compare a computed colour against `undefined`.
    assert(
        typeof value === "string",
        `the theme declares colors.${semantic ? "semantic." : ""}${key}`,
    );
    return value as string;
}

function toRgb(hex: string): string {
    const r = Number.parseInt(hex.slice(1, 3), 16);
    const g = Number.parseInt(hex.slice(3, 5), 16);
    const b = Number.parseInt(hex.slice(5, 7), 16);
    return `rgb(${r}, ${g}, ${b})`;
}

// ------------------------------------------------------------------------------------- the fixture

/** A mounted host, laid out inside a real panel slot so `font: inherit` picks up the type scale. */
interface Host {
    readonly element: HTMLElement;
    dispose(): void;
}

function mountHost(): Host {
    const element = window.document.createElement("div");
    element.className = "ctx-panel-body";
    window.document.body.appendChild(element);
    return {
        element,
        dispose: (): void => {
            element.remove();
        },
    };
}

function documentEngine(): ThemeEngine {
    return new ThemeEngine({
        root: window.document.documentElement,
        probe: (query: string) => ({ matches: query === REDUCED_MOTION_QUERY }),
    });
}

function applyToDocument(engine: ThemeEngine, themeId: string): void {
    const report = engine.apply(themeId);
    assert(report.applied, `the engine applied ${themeId}: ${report.diagnostic}`);
    assertEqual(report.fadeDurationMs, 0, `${themeId} applied with no cross-fade in flight`);
}

/** The served authored-families stylesheet, found by NAME so a CMake rename fails loudly. */
function componentStyleSheet(): CSSStyleSheet {
    const sheets = [...window.document.styleSheets].filter(
        (sheet) => (sheet.href ?? "").endsWith(`/${KIT_COMPONENT_STYLESHEET}`),
    );
    assert(
        sheets.length === 1,
        `exactly one served stylesheet is ${KIT_COMPONENT_STYLESHEET} (found ${sheets.length}) — ` +
            `the harness page must <link> the SHIPPED sheet, or every assertion below is vacuous`,
    );
    return sheets[0] as CSSStyleSheet;
}

/**
 * ONE SAMPLE PER FAMILY, built through the family's real factory.
 *
 * Keyed by the family name from the kit's own roster, so the "all twelve exist" case can iterate the
 * roster and fail on a family this fixture forgot — rather than silently testing eleven.
 */
function buildSamples(): Readonly<Record<string, HTMLElement>> {
    const tooltipTrigger = createButton({ label: "Help" }).element;
    const tooltip = createTooltip({ trigger: tooltipTrigger, text: "What this does" });
    tooltip.show();
    return {
        buttons: createButton({ label: "Apply", tone: "primary" }).element,
        fields: createTextField({ label: "Project name", value: "roll-3d" }).element,
        tabs: createTabs({
            label: "Inspector sections",
            tabs: [
                { id: "a", label: "Transform", content: window.document.createTextNode("a") },
                { id: "b", label: "Rendering", content: window.document.createTextNode("b") },
                { id: "c", label: "Physics", content: window.document.createTextNode("c") },
            ],
        }).element,
        trees: createTree({
            label: "Scene",
            nodes: [
                {
                    id: "root",
                    label: "Root",
                    expanded: true,
                    children: [
                        { id: "child", label: "Child" },
                        { id: "sibling", label: "Sibling" },
                    ],
                },
                { id: "leaf", label: "Leaf" },
            ],
        }).element,
        tables: createTable({
            caption: "Packages",
            columns: [
                { key: "name", label: "Name" },
                { key: "count", label: "Entities", numeric: true, sortable: true },
            ],
            rows: [{ id: "r1", cells: { name: "physics3d", count: "12" } }],
        }).element,
        chips: createChip({ label: "errors", tone: "bad", onRemove: () => undefined }).element,
        badges: createBadge({ label: "7", tone: "warn" }).element,
        toasts: (() => {
            const region = createToastRegion();
            region.show({ message: "Build finished", tone: "good" });
            return region.element;
        })(),
        "empty-states": createEmptyState({
            title: "No project open",
            description: "Open a folder to begin.",
        }).element,
        skeletons: createSkeleton({ lines: 3 }).element,
        dialogs: createDialog({
            title: "Discard changes?",
            description: "This cannot be undone.",
            cancelLabel: "Keep",
            confirm: { label: "Discard", tone: "danger" },
        }).element,
        tooltips: tooltip.element,
    };
}

/** One theme expectation: which family's element, which computed property, which theme colour. */
interface ThemeExpectation {
    readonly family: string;
    readonly selector: string;
    readonly property: StyleProperty;
    readonly token: string;
    readonly semantic?: boolean;
}

/**
 * At least one colour-bearing expectation for EVERY one of the twelve families.
 *
 * Complete coverage is what makes "a theme switch restyles them" a claim about the KIT rather than
 * about whichever component happened to be sampled — and the Dark/Light disagreement case below is
 * what stops any of these from passing on an inherited value.
 */
const THEME_EXPECTATIONS: readonly ThemeExpectation[] = [
    { family: "buttons", selector: ".ctx-button", property: "backgroundColor", token: "accent" },
    { family: "fields", selector: ".ctx-field__label", property: "color", token: "muted" },
    {
        family: "tabs",
        selector: '.ctx-tabs__tab[aria-selected="true"]',
        property: "borderBottomColor",
        token: "accent",
    },
    { family: "trees", selector: ".ctx-tree__item", property: "color", token: "ink" },
    { family: "tables", selector: ".ctx-table__caption", property: "color", token: "muted" },
    {
        family: "chips",
        selector: ".ctx-chip",
        property: "color",
        token: "bad",
        semantic: true,
    },
    {
        family: "badges",
        selector: ".ctx-badge",
        property: "color",
        token: "warn",
        semantic: true,
    },
    { family: "toasts", selector: ".ctx-toast", property: "backgroundColor", token: "panel2" },
    {
        family: "empty-states",
        selector: ".ctx-empty-state__description",
        property: "color",
        token: "muted",
    },
    {
        family: "skeletons",
        selector: ".ctx-skeleton__line",
        property: "backgroundColor",
        token: "panel2",
    },
    { family: "dialogs", selector: ".ctx-dialog", property: "backgroundColor", token: "panel" },
    { family: "tooltips", selector: ".ctx-tooltip", property: "backgroundColor", token: "panel2" },
];

function resolveExpectation(host: HTMLElement, expectation: ThemeExpectation): HTMLElement {
    const root = host.querySelector<HTMLElement>(`[data-family="${expectation.family}"]`);
    assert(root !== null, `the ${expectation.family} sample is mounted`);
    const element = (root as HTMLElement).querySelector<HTMLElement>(expectation.selector);
    assert(
        element !== null,
        `the ${expectation.family} sample contains \`${expectation.selector}\` — a selector that ` +
            `matches nothing would make its theme assertion vacuous`,
    );
    return element as HTMLElement;
}

function mountSamples(host: HTMLElement): Readonly<Record<string, HTMLElement>> {
    const samples = buildSamples();
    for (const [family, element] of Object.entries(samples)) {
        const wrapper = window.document.createElement("div");
        wrapper.setAttribute("data-family", family);
        wrapper.appendChild(element);
        host.appendChild(wrapper);
    }
    return samples;
}

function expectedColour(document_: Doc, expectation: ThemeExpectation): string {
    return toRgb(colour(document_, expectation.token, expectation.semantic === true));
}

// -------------------------------------------------------------------------------------- the tests

export const kitComponentTests: readonly TestCase[] = [
    {
        name: "kit/components: the served components stylesheet styles EVERY family's root class",
        run: () => {
            const sheet = componentStyleSheet();
            const text = [...sheet.cssRules].map((rule) => rule.cssText).join("\n");
            for (const family of COMPONENT_FAMILIES) {
                assert(
                    text.includes(`.${family.rootClass}`),
                    `the served ${KIT_COMPONENT_STYLESHEET} styles \`.${family.rootClass}\` (the ` +
                        `${family.family} family) — a family whose rules never reached the renderer ` +
                        `lints perfectly and paints nothing`,
                );
            }
            assertEqual(
                COMPONENT_FAMILIES.length,
                12,
                "the kit publishes the twelve families of design 06 §3",
            );
        },
    },
    {
        name: "kit/components: every family's factory builds an element carrying its root class",
        run: () => {
            const host = mountHost();
            try {
                const samples = mountSamples(host.element);
                for (const family of COMPONENT_FAMILIES) {
                    const element = samples[family.family];
                    assert(
                        element !== undefined,
                        `the T1 fixture builds a \`${family.family}\` sample — a family the fixture ` +
                            `forgot would be silently untested by every case below`,
                    );
                    const root = element as HTMLElement;
                    assert(
                        root.classList.contains(family.rootClass) ||
                            root.querySelector(`.${family.rootClass}`) !== null,
                        `the \`${family.family}\` factory produces \`.${family.rootClass}\` — the ` +
                            `class its roster entry promises, and the one components.css styles`,
                    );
                }
            } finally {
                host.dispose();
            }
        },
    },
    {
        name: "kit/components: an authored Button COMPUTES identically to a hydrated ctx-widget-button",
        run: () => {
            // THE reuse assertion. `webui-kit-family-coverage` proves the button family CLAIMS the
            // `button` role; this proves the claim is true where it matters — in the resolved
            // cascade. A forked implementation (a second rule set under `.ctx-button`) passes the
            // source gate and fails here.
            const engine = documentEngine();
            const host = mountHost();
            try {
                applyToDocument(engine, BUILTIN_DARK);
                const authored = createButton({ label: "Run" }).element;
                const hydrated = window.document.createElement("button");
                hydrated.className = WIDGET_CLASSES["button"] as string;
                hydrated.textContent = "Run";
                host.element.append(authored, hydrated);

                const paint: readonly StyleProperty[] = [
                    "color",
                    "backgroundColor",
                    "borderTopColor",
                    "borderTopLeftRadius",
                    "fontFamily",
                ];
                const authoredStyle = window.getComputedStyle(authored);
                const hydratedStyle = window.getComputedStyle(hydrated);
                for (const property of paint) {
                    assertEqual(
                        authoredStyle[property],
                        hydratedStyle[property],
                        `the authored Button and the hydrated \`.ctx-widget-button\` agree on ` +
                            `${property} — they are ONE visual contract owned by kit.css, not two ` +
                            `that happen to match today (design 06 §3; the spec's "do not fork a ` +
                            `parallel implementation")`,
                    );
                }
                // The authored family adds LAYOUT and nothing else, which is the other half of the
                // claim: if `.ctx-button` were painting, the loop above could not have passed.
                assertEqual(
                    authoredStyle.display,
                    "inline-flex",
                    "the authored Button adds its own layout on top of the shared paint",
                );
            } finally {
                host.dispose();
            }
        },
    },
    {
        name: "kit/components: authored fields and trees reuse their role primitives' paint too",
        run: () => {
            // THE SAME ASSERTION THE BUTTON CASE MAKES, FOR THE OTHER FIVE REUSED ROLES, AND IT HAS
            // TO BE ON THE COMPUTED STYLE. Class presence alone is exactly what a FORKED stylesheet
            // would also satisfy: `components.css` is permitted to narrow a widget class through a
            // compound selector (`.ctx-widget-textbox.ctx-field__control`, 0,2,0), so an authored
            // field could carry the primitive's class and still be repainted by a rule of its own —
            // passing every source gate, and passing a `classList.contains` check, while the whole
            // "one visual contract" property was gone. Only the resolved cascade can tell.
            const engine = documentEngine();
            const host = mountHost();
            try {
                applyToDocument(engine, BUILTIN_DARK);
                const textField = createTextField({ label: "Name" });
                const checkboxField = createCheckboxField({ label: "Snap" });
                const tree = createTree({ label: "Scene", nodes: [{ id: "n", label: "Node" }] });
                const list = createList({ label: "Recent", items: [{ id: "i", label: "Item" }] });
                host.element.append(
                    textField.element,
                    checkboxField.element,
                    tree.element,
                    list.element,
                );
                const treeItem = tree.element.querySelector<HTMLElement>('[role="treeitem"]');
                assert(treeItem !== null, "the authored tree renders a treeitem");
                const listItem = list.element.querySelector<HTMLElement>("li");
                assert(listItem !== null, "the authored list renders a row");

                // The hand-built counterpart of each authored element: the SAME tag the factory uses,
                // carrying ONLY the e06c1 widget class. Same tag matters — a user-agent stylesheet
                // difference between, say, `<input>` and `<div>` would show up as a paint difference
                // that has nothing to do with the kit.
                const cases: readonly (readonly [HTMLElement, string, keyof HTMLElementTagNameMap])[] =
                    [
                        [textField.control, "textbox", "input"],
                        [checkboxField.control, "checkbox", "input"],
                        [tree.element, "tree", "ul"],
                        [treeItem as HTMLElement, "treeitem", "li"],
                        [list.element, "list", "ul"],
                        [listItem as HTMLElement, "listitem", "li"],
                    ];
                const paint: readonly StyleProperty[] = [
                    "color",
                    "backgroundColor",
                    "borderTopColor",
                    "borderTopLeftRadius",
                    "fontFamily",
                ];
                for (const [element, role, tag] of cases) {
                    const widgetClass = WIDGET_CLASSES[role] as string;
                    assert(
                        element.classList.contains(widgetClass),
                        `the authored ${role} carries \`.${widgetClass}\` — the e06c1 primitive, ` +
                            `not a second class that looks like it`,
                    );
                    const hydrated = window.document.createElement(tag);
                    hydrated.className = widgetClass;
                    if (element instanceof HTMLInputElement) {
                        (hydrated as HTMLInputElement).type = element.type;
                    }
                    // Mounted in the SAME container, so anything inherited from the panel body is
                    // inherited identically by both and cannot mask a difference either way.
                    host.element.append(hydrated);
                    const authoredStyle = window.getComputedStyle(element);
                    const hydratedStyle = window.getComputedStyle(hydrated);
                    for (const property of paint) {
                        assertEqual(
                            authoredStyle[property],
                            hydratedStyle[property],
                            `the authored ${role} and a bare \`.${widgetClass}\` agree on ` +
                                `${property} — kit.css owns that decision for both, and a compound ` +
                                `rule in components.css that repainted it would be a fork wearing a ` +
                                `reuse label (design 06 §3)`,
                        );
                    }
                }
            } finally {
                host.dispose();
            }
        },
    },
    {
        name: "kit/components: every family renders from the ACTIVE theme's tokens",
        run: () => {
            const engine = documentEngine();
            const host = mountHost();
            try {
                applyToDocument(engine, BUILTIN_DARK);
                mountSamples(host.element);
                for (const expectation of THEME_EXPECTATIONS) {
                    const element = resolveExpectation(host.element, expectation);
                    assertEqual(
                        window.getComputedStyle(element)[expectation.property],
                        expectedColour(darkTheme as Doc, expectation),
                        `${expectation.family}: the COMPUTED ${expectation.property} of ` +
                            `\`${expectation.selector}\` is the Dark theme's colour — a family that ` +
                            `does not resolve to a token is one the theme cannot reach`,
                    );
                }
            } finally {
                host.dispose();
            }
        },
    },
    {
        name: "kit/components: a theme switch restyles every family, and Dark and Light DISAGREE",
        run: () => {
            // The DoD's first line, plus the guard against a vacuous pass of the case above: if a
            // rule silently stopped applying, both themes would resolve to the same inherited or
            // user-agent value and the per-theme assertion could still pass on one of them.
            const engine = documentEngine();
            const host = mountHost();
            try {
                applyToDocument(engine, BUILTIN_DARK);
                mountSamples(host.element);
                const elements = THEME_EXPECTATIONS.map((expectation) =>
                    resolveExpectation(host.element, expectation),
                );
                const dark = elements.map((element, index) =>
                    window.getComputedStyle(element)[
                        (THEME_EXPECTATIONS[index] as ThemeExpectation).property
                    ],
                );
                applyToDocument(engine, BUILTIN_LIGHT);
                for (let index = 0; index < THEME_EXPECTATIONS.length; index += 1) {
                    const expectation = THEME_EXPECTATIONS[index] as ThemeExpectation;
                    const element = elements[index] as HTMLElement;
                    assertEqual(
                        window.getComputedStyle(element)[expectation.property],
                        expectedColour(lightTheme as Doc, expectation),
                        `${expectation.family}: switching to Light restyled ` +
                            `\`${expectation.selector}\` with NO component change`,
                    );
                    assert(
                        dark[index] !== window.getComputedStyle(element)[expectation.property],
                        `${expectation.family}: \`${expectation.selector}\`'s ` +
                            `${expectation.property} DIFFERS between Dark and Light — equal values ` +
                            `mean the kit rule is not the thing deciding it`,
                    );
                }
            } finally {
                host.dispose();
                applyToDocument(engine, BUILTIN_DARK);
            }
        },
    },
    {
        name: "kit/components: no kit component writes an inline style",
        run: () => {
            // The RUNTIME counterpart of `webui-kit-source-tokens`. The source gate reads the kit's
            // own modules; this one reads what a mount actually produced, so a style written through
            // a helper the scanner did not recognise still fails.
            const host = mountHost();
            try {
                mountSamples(host.element);
                const styled = [...host.element.querySelectorAll("[style]")].filter(
                    (element) => (element.getAttribute("style") ?? "") !== "",
                );
                assertEqual(
                    styled.map((element) => element.className),
                    [],
                    "no element a kit factory built carries a `style` attribute — appearance comes " +
                        "from the token-linted stylesheets and from nowhere else",
                );
            } finally {
                host.dispose();
            }
        },
    },
    {
        name: "kit/components: tabs own ONE tab stop and move on the arrow keys",
        run: () => {
            const host = mountHost();
            try {
                const tabs = createTabs({
                    label: "Inspector sections",
                    tabs: [
                        { id: "a", label: "A", content: window.document.createTextNode("a") },
                        { id: "b", label: "B", content: window.document.createTextNode("b") },
                        { id: "c", label: "C", content: window.document.createTextNode("c") },
                    ],
                });
                host.element.append(tabs.element);
                const buttons = [
                    ...tabs.element.querySelectorAll<HTMLButtonElement>('[role="tab"]'),
                ];
                assertEqual(buttons.length, 3, "three tabs rendered with role=tab");
                assertEqual(
                    buttons.filter((button) => button.tabIndex === 0).length,
                    1,
                    "exactly ONE tab is tabbable — a tablist is one Tab stop, not N (an eight-tab " +
                        "panel would otherwise cost a keyboard user eight presses to get past)",
                );
                assertEqual(buttons[0]?.getAttribute("aria-selected"), "true", "tab 0 is selected");

                buttons[0]?.focus();
                buttons[0]?.dispatchEvent(
                    new KeyboardEvent("keydown", { key: "ArrowRight", bubbles: true }),
                );
                assertEqual(tabs.activeId, "b", "ArrowRight moved the selection");
                assertEqual(
                    window.document.activeElement,
                    buttons[1],
                    "ArrowRight moved FOCUS as well as selection (roving tabindex)",
                );
                assertEqual(
                    buttons[1]?.getAttribute("aria-selected"),
                    "true",
                    "the newly selected tab announces itself",
                );

                buttons[1]?.dispatchEvent(
                    new KeyboardEvent("keydown", { key: "End", bubbles: true }),
                );
                assertEqual(tabs.activeId, "c", "End jumps to the last tab (part of the pattern)");
                buttons[2]?.dispatchEvent(
                    new KeyboardEvent("keydown", { key: "Home", bubbles: true }),
                );
                assertEqual(tabs.activeId, "a", "Home jumps to the first tab");

                const panels = [
                    ...tabs.element.querySelectorAll<HTMLElement>('[role="tabpanel"]'),
                ];
                assertEqual(
                    panels.filter((panel) => !panel.hidden).length,
                    1,
                    "exactly one tabpanel is visible",
                );
                assertEqual(
                    panels[0]?.getAttribute("aria-labelledby"),
                    buttons[0]?.id,
                    "the visible panel is labelled by its own tab",
                );
            } finally {
                host.dispose();
            }
        },
    },
    {
        name: "kit/components: a tree is one tab stop, arrow-navigable, and expands on ArrowRight",
        run: () => {
            const host = mountHost();
            try {
                const tree = createTree({
                    label: "Scene",
                    nodes: [
                        {
                            id: "root",
                            label: "Root",
                            children: [{ id: "child", label: "Child" }],
                        },
                        { id: "leaf", label: "Leaf" },
                    ],
                });
                host.element.append(tree.element);
                const items = [...tree.element.querySelectorAll<HTMLElement>('[role="treeitem"]')];
                assertEqual(items.length, 3, "three treeitems rendered");
                assertEqual(
                    items.filter((item) => item.tabIndex === 0).length,
                    1,
                    "the tree owns exactly ONE tab stop",
                );
                assertEqual(items[0]?.getAttribute("aria-level"), "1", "top-level items are level 1");
                assertEqual(
                    items[0]?.getAttribute("aria-posinset"),
                    "1",
                    "position in set is stated, not left to be counted",
                );
                assertEqual(items[0]?.getAttribute("aria-expanded"), "false", "root starts closed");

                items[0]?.focus();
                items[0]?.dispatchEvent(
                    new KeyboardEvent("keydown", { key: "ArrowRight", bubbles: true }),
                );
                assertEqual(
                    items[0]?.getAttribute("aria-expanded"),
                    "true",
                    "ArrowRight expands a collapsed node (the ARIA tree pattern)",
                );

                items[0]?.dispatchEvent(
                    new KeyboardEvent("keydown", { key: "ArrowDown", bubbles: true }),
                );
                assertEqual(
                    window.document.activeElement,
                    items[1],
                    "ArrowDown walks to the now-VISIBLE child — the walk follows the expanded " +
                        "state, not the flat DOM order",
                );

                let activated = "";
                const activatable = createTree({
                    label: "Scene",
                    nodes: [{ id: "only", label: "Only" }],
                    onActivate: (id) => {
                        activated = id;
                    },
                });
                host.element.append(activatable.element);
                const only = activatable.element.querySelector<HTMLElement>('[role="treeitem"]');
                only?.focus();
                only?.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true }));
                assertEqual(activated, "only", "Enter activates the focused item");
            } finally {
                host.dispose();
            }
        },
    },
    {
        name: "kit/components: a modal dialog is inert-outside, Escape-dismissible and restores focus",
        run: () => {
            const host = mountHost();
            try {
                const opener = createButton({ label: "Delete" }).element;
                const outside = createButton({ label: "Outside" }).element;
                let closedWith = "";
                const dialog = createDialog({
                    title: "Discard changes?",
                    description: "This cannot be undone.",
                    cancelLabel: "Keep",
                    confirm: { label: "Discard", tone: "danger" },
                    onClose: (reason) => {
                        closedWith = reason;
                    },
                });
                host.element.append(opener, outside, dialog.element);

                assertEqual(
                    dialog.element.getAttribute("aria-labelledby"),
                    dialog.element.querySelector(".ctx-dialog__title")?.id,
                    "the dialog is NAMED by its own title, not left anonymous",
                );

                opener.focus();
                dialog.open();
                assert(dialog.isOpen, "the dialog opened");
                assertEqual(
                    dialog.element.getAttribute("aria-modal"),
                    "true",
                    "the dialog announces its modality",
                );
                assert(
                    dialog.element.contains(window.document.activeElement),
                    "focus moved INTO the dialog deterministically — not left wherever the UA's " +
                        "autofocus heuristics happened to put it",
                );

                // THE trap assertion, and the reason a native <dialog> was chosen over a hand-built
                // focus cycle: everything outside a modal is genuinely INERT, so `.focus()` on it
                // does nothing. A Tab-cycling trap would pass a synthetic-Tab test and still let a
                // screen reader's virtual cursor walk straight out.
                outside.focus();
                assert(
                    window.document.activeElement !== outside,
                    "an element OUTSIDE the open modal cannot take focus — inertness, not a " +
                        "Tab-cycling trap",
                );

                dialog.element.dispatchEvent(
                    new KeyboardEvent("keydown", { key: "Escape", bubbles: true }),
                );
                assert(!dialog.isOpen, "Escape dismissed the dialog");
                assertEqual(closedWith, "dismiss", "the close reason distinguishes dismissal");
                assertEqual(
                    window.document.activeElement,
                    opener,
                    "focus returned to the element that opened the dialog — the half a later a11y " +
                        "audit cannot retrofit without rewriting the component",
                );
            } finally {
                host.dispose();
            }
        },
    },
    {
        name: "kit/components: a tooltip is focus-reachable, describes its trigger, and Escape-dismisses",
        run: () => {
            const host = mountHost();
            try {
                const trigger = createButton({ label: "Bake" }).element;
                host.element.append(trigger);
                const tooltip = createTooltip({ trigger, text: "Rebuilds the lightmap" });

                assert(tooltip.element.contains(trigger), "the host wraps the trigger");
                assert(
                    tooltip.element.contains(tooltip.bubble),
                    "the bubble lives INSIDE the hover host — WCAG 1.4.13 'hoverable' falls out of " +
                        "the DOM shape rather than out of a timer",
                );
                assertEqual(
                    trigger.getAttribute("aria-describedby"),
                    tooltip.bubble.id,
                    "the tooltip DESCRIBES the trigger — `aria-describedby`, never `aria-labelledby`, " +
                        "which would replace the control's own name with the explanation",
                );
                assertEqual(tooltip.bubble.getAttribute("role"), "tooltip", "role=tooltip");
                // `=== true` rather than a bare truthiness check: the DOM lib types `hidden` as
                // `boolean | string` (the `until-found` value), so a bare read is not a boolean.
                assert(tooltip.bubble.hidden === true, "the bubble starts hidden");

                trigger.focus();
                tooltip.element.dispatchEvent(new FocusEvent("focusin", { bubbles: true }));
                assert(
                    tooltip.visible && !tooltip.bubble.hidden,
                    "FOCUS shows the tooltip — a hover-only tooltip does not exist for a keyboard " +
                        "or touch user at all",
                );

                tooltip.element.dispatchEvent(
                    new KeyboardEvent("keydown", { key: "Escape", bubbles: true }),
                );
                assert(
                    !tooltip.visible && tooltip.bubble.hidden === true,
                    "Escape dismisses it without moving the pointer (WCAG 1.4.13)",
                );
            } finally {
                host.dispose();
            }
        },
    },
    {
        name: "kit/components: toasts reach the right politeness lane and dismiss",
        run: () => {
            const host = mountHost();
            try {
                const region = createToastRegion();
                host.element.append(region.element);
                const lanes = [
                    ...region.element.querySelectorAll<HTMLElement>(".ctx-toast-region__lane"),
                ];
                assertEqual(lanes.length, 2, "the region mounts BOTH live lanes up front");
                assertEqual(lanes[0]?.getAttribute("role"), "status", "the polite lane is a status");
                assertEqual(lanes[1]?.getAttribute("role"), "alert", "the assertive lane is an alert");
                assertEqual(
                    region.element.getAttribute("role"),
                    "presentation",
                    "the container is NOT itself a live region — nesting them duplicates " +
                        "announcements unpredictably",
                );

                const good = region.show({ message: "Build finished", tone: "good" });
                const bad = region.show({ message: "Build failed", tone: "bad" });
                const warn = region.show({ message: "Compiling", tone: "warn" });
                assertEqual(region.count, 3, "three toasts on screen");
                assert(lanes[0]?.contains(good.element) === true, "a success is polite");
                assert(
                    lanes[1]?.contains(bad.element) === true,
                    "an error INTERRUPTS — the one tone that goes assertive",
                );
                assert(
                    lanes[0]?.contains(warn.element) === true,
                    "`warn` means ACTIVE WORK in this design system (06 §2), not danger, so it " +
                        "stays polite — an assertive announcement per build step would make the " +
                        "editor unusable with a screen reader",
                );

                const close = bad.element.querySelector<HTMLButtonElement>(".ctx-toast__close");
                assert(close !== null, "every toast carries a dismiss control");
                assertEqual(
                    close?.getAttribute("aria-label"),
                    "Dismiss notification",
                    "the glyph-only dismiss control has an accessible name",
                );
                close?.click();
                assertEqual(region.count, 2, "dismissing removes exactly one toast");
                bad.dismiss();
                assertEqual(region.count, 2, "dismissal is idempotent");
            } finally {
                host.dispose();
            }
        },
    },
    {
        name: "kit/components: a table is a real table — caption, header scopes, and aria-sort",
        run: () => {
            const host = mountHost();
            try {
                const sorted: string[] = [];
                const table = createTable({
                    caption: "Packages",
                    columns: [
                        { key: "name", label: "Name" },
                        { key: "count", label: "Entities", numeric: true, sortable: true },
                    ],
                    rows: [
                        { id: "r1", cells: { name: "physics3d", count: "12" } },
                        { id: "r2", cells: { name: "particles", count: "4" } },
                    ],
                    onSort: (key, direction) => {
                        sorted.push(`${key}:${direction}`);
                    },
                });
                host.element.append(table.element);

                assertEqual(
                    table.element.querySelector("caption")?.textContent,
                    "Packages",
                    "the table has a real <caption> — its accessible name",
                );
                const headers = [...table.element.querySelectorAll<HTMLTableCellElement>("thead th")];
                assertEqual(headers.length, 2, "two column headers");
                assertEqual(headers[0]?.scope, "col", "column headers are scoped");
                const rowHeader = table.element.querySelector<HTMLTableCellElement>("tbody th");
                assertEqual(
                    rowHeader?.scope,
                    "row",
                    "the first column is a ROW header, so any later cell announces which row it is " +
                        "in instead of making the user count columns",
                );

                const sortable = headers[1] as HTMLTableCellElement;
                assertEqual(
                    sortable.getAttribute("aria-sort"),
                    "none",
                    "a sortable column announces that it IS sortable while unsorted — dropping the " +
                        "attribute makes it indistinguishable from a fixed one",
                );
                const sortButton = sortable.querySelector<HTMLButtonElement>(".ctx-table__sort");
                assert(sortButton !== null, "the sort control is a real button (keyboard-reachable)");
                sortButton?.click();
                assertEqual(sortable.getAttribute("aria-sort"), "ascending", "first click ascends");
                sortButton?.click();
                assertEqual(
                    sortable.getAttribute("aria-sort"),
                    "descending",
                    "a second click reverses",
                );
                assertEqual(sorted, ["count:ascending", "count:descending"], "onSort was told both");
                assertEqual(headers[0]?.getAttribute("aria-sort"), null, "a fixed column has none");
            } finally {
                host.dispose();
            }
        },
    },
    {
        name: "kit/components: fields are labelled, described and disable-able",
        run: () => {
            const host = mountHost();
            try {
                const text = createTextField({
                    label: "Project name",
                    description: "Shown in the window title.",
                });
                const checkbox = createCheckboxField({ label: "Snap to grid", checked: true });
                const select = createSelectField({
                    label: "Theme",
                    options: [
                        { value: "builtin.dark", label: "Dark" },
                        { value: "builtin.light", label: "Light" },
                    ],
                    value: "builtin.light",
                });
                host.element.append(text.element, checkbox.element, select.element);

                const label = text.element.querySelector<HTMLLabelElement>(".ctx-field__label");
                assertEqual(
                    label?.htmlFor,
                    text.control.id,
                    "the label is a real `<label for>` — a click target that focuses the control, " +
                        "which an `aria-label` is not",
                );
                const describedBy = text.control.getAttribute("aria-describedby");
                assert(describedBy !== null, "the description is wired");
                assertEqual(
                    text.element.querySelector(`#${describedBy}`)?.textContent,
                    "Shown in the window title.",
                    "and it points at an element that EXISTS and carries the text",
                );
                assertEqual(
                    createTextField({ label: "Plain" }).control.getAttribute("aria-describedby"),
                    null,
                    "a field with no description gets NO dangling `aria-describedby` — one pointing " +
                        "at nothing announces nothing while the author believes it describes",
                );

                assertEqual(checkbox.control.type, "checkbox", "the checkbox is a native control");
                assert(checkbox.control.checked, "its initial state is honoured");
                assertEqual(select.control.value, "builtin.light", "the select's value is honoured");

                text.setDisabled(true);
                assert(text.control.disabled, "a field can be disabled through its handle");
            } finally {
                host.dispose();
            }
        },
    },
    {
        name: "kit/components: chips and badges carry reserved semantics, and a live badge is a status",
        run: () => {
            const host = mountHost();
            try {
                let removed = 0;
                const chip = createChip({
                    label: "errors",
                    tone: "bad",
                    onRemove: () => {
                        removed += 1;
                    },
                });
                host.element.append(chip.element);
                const remove = chip.element.querySelector<HTMLButtonElement>(".ctx-chip__remove");
                assert(remove !== null, "a removable chip has a real button");
                assertEqual(
                    remove?.getAttribute("aria-label"),
                    "Remove errors",
                    "the glyph-only remove control names WHICH chip it removes",
                );
                remove?.click();
                assertEqual(removed, 1, "removal fires once");

                const inert = createBadge({ label: "7" });
                const live = createBadge({ label: "7", live: true, accessibleLabel: "7 problems" });
                host.element.append(inert.element, live.element);
                assertEqual(
                    inert.element.getAttribute("role"),
                    null,
                    "a plain badge is NOT a live region — an editor full of cosmetic counters would " +
                        "otherwise read a stream of numbers nobody asked for",
                );
                assertEqual(live.element.tagName, "OUTPUT", "a live badge reuses the status primitive");
                assert(
                    live.element.classList.contains(WIDGET_CLASSES["status"] as string),
                    "and carries the e06c1 `status` widget class rather than a copy of it",
                );
                assertEqual(live.element.getAttribute("aria-live"), "polite", "it is polite");
                assertEqual(
                    window.getComputedStyle(live.element).display,
                    "inline-flex",
                    "the compound `.ctx-widget-status.ctx-badge` selector wins over the widget " +
                        "layer's `display: block` by SPECIFICITY, not by arriving in a later " +
                        "stylesheet — document order must never decide appearance",
                );
            } finally {
                host.dispose();
            }
        },
    },
    {
        name: "kit/components: an empty state offers one action, and a skeleton hides its bars",
        run: () => {
            const host = mountHost();
            try {
                let opened = 0;
                const empty = createEmptyState({
                    title: "No project open",
                    description: "Open a folder to begin.",
                    action: {
                        label: "Open folder",
                        onActivate: () => {
                            opened += 1;
                        },
                    },
                });
                host.element.append(empty.element);
                const action = empty.element.querySelector<HTMLButtonElement>(".ctx-button");
                assert(action !== null, "the action is a kit button");
                action?.click();
                assertEqual(opened, 1, "and it fires");
                assertEqual(
                    empty.element.querySelector("h1, h2, h3, h4, h5, h6"),
                    null,
                    "the title is NOT a heading — a kit component cannot know its level, and a " +
                        "wrong one corrupts the outline a screen-reader user navigates by",
                );

                const decorative = createSkeleton({ lines: 3 });
                const announced = createSkeleton({ lines: 2, busyLabel: "Loading panels" });
                host.element.append(decorative.element, announced.element);
                assertEqual(
                    decorative.element.getAttribute("aria-hidden"),
                    "true",
                    "placeholder bars are decorative by default",
                );
                assertEqual(
                    decorative.element.querySelectorAll(".ctx-skeleton__line").length,
                    3,
                    "three bars",
                );
                assertEqual(
                    announced.element.getAttribute("role"),
                    "status",
                    "a `busyLabel` skeleton is a NAMED polite region rather than N unlabelled " +
                        "boxes — what it is not is a guaranteed interruption, which is a live-region " +
                        "property this shape cannot have (../../../kit/README.md § Recorded findings)",
                );
                assertEqual(announced.element.getAttribute("aria-label"), "Loading panels", "named");
            } finally {
                host.dispose();
            }
        },
    },
];
