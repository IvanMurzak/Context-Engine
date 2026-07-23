// The component KIT's T1 tier (M9 e06c1; design 06 §3, 04 §4 step 4). The half of the kit's DoD that
// only a real browser can answer: not "does the stylesheet contain the right text" — `check_kit_tokens.py`
// already reads the source on every `build` leg — but **does the widget layer actually resolve to the
// active theme's tokens, and does a live theme switch carry it**.
//
// WHY BOTH TIERS EXIST, AND WHY NEITHER SUBSUMES THE OTHER. The source gate proves the kit REFERENCES
// only `var(--ctx-*)` and that the closed 12-role set agrees across C++, TS and CSS. It is blind to
// everything between a declaration and a painted element: whether the sheet was SERVED at all, whether
// a token name it references is one the theme engine actually writes (a typo in `--ctx-colors-chipp`
// lints clean and paints nothing), and whether some other stylesheet wins the cascade. Those are
// exactly the failures that went undetected until a CI-only CEF leg at e05c and again at e06b — this
// file turns them into a sub-2s local check.
//
// ⚠ THE ENGINE IS DRIVEN WITH REDUCED MOTION ON, and that is not a shortcut. app.css arms a
// `transition` on `html[data-theme-transition="true"] *` for the cross-fade's duration, so a
// synchronous `getComputedStyle` immediately after a SWITCH reports the colour the transition is
// animating AWAY from. Reduced motion collapses every duration to 0ms — an UNCONDITIONAL override
// (06 §1), i.e. the engine's own documented behaviour — which makes the resolved value observable the
// instant it is applied. `theme_dom.test.ts` paid a red run to learn this; the same reasoning applies
// verbatim here.

import { assert, assertEqual, type TestCase } from "./harness.js";
import {
    KIT_STYLESHEET,
    KIT_STYLESHEETS,
    WIDGET_CLASS_PREFIX,
    WIDGET_CLASSES,
} from "../../../kit/src/index.js";
import { BUILTIN_DARK, BUILTIN_LIGHT, REDUCED_MOTION_QUERY, ThemeEngine } from "../theme.js";

import darkTheme from "../../../tokens/themes/dark.theme.json";
import lightTheme from "../../../tokens/themes/light.theme.json";

/** A theme document, read as the plain JSON it is (the `theme.ts` discipline: no mirrored type). */
type Doc = Readonly<Record<string, unknown>>;

/** `colors.ink` -> the theme's hex string. Asserts rather than defaults: a miss must be loud. */
function colour(document_: Doc, key: string): string {
    const colors = document_["colors"] as Record<string, unknown> | undefined;
    const value = colors === undefined ? undefined : colors[key];
    assert(typeof value === "string", `the theme declares colors.${key}`);
    return value as string;
}

/** `#0a0a0a` -> `rgb(10, 10, 10)`, the form `getComputedStyle` reports a resolved colour in. */
function toRgb(hex: string): string {
    const r = Number.parseInt(hex.slice(1, 3), 16);
    const g = Number.parseInt(hex.slice(3, 5), 16);
    const b = Number.parseInt(hex.slice(5, 7), 16);
    return `rgb(${r}, ${g}, ${b})`;
}

/**
 * The tag `uitree::role_html_tag` emits for each role.
 *
 * Mirrored here ON PURPOSE and only here: the test mounts the SAME elements the hydration runtime
 * will, because a UA default (a `<button>`'s own background, an `<input>`'s own border) is part of
 * what the kit has to beat — asserting against a `<div>` wearing the class would pass while the real
 * control stayed in the platform's chrome. The C++-vs-TS agreement of the role SET itself is
 * `webui-kit-role-coverage`'s job, not this table's, so the two are not a duplicated source of truth.
 */
const ROLE_TAGS: Readonly<Record<string, string>> = {
    region: "section",
    group: "div",
    tree: "ul",
    treeitem: "li",
    list: "ul",
    listitem: "li",
    button: "button",
    textbox: "input",
    checkbox: "input",
    heading: "h2",
    status: "output",
    text: "span",
};

/** One expectation: a role, a computed CSS property, and the colour TOKEN it must resolve to. */
interface ColourExpectation {
    readonly role: string;
    readonly property: "color" | "backgroundColor" | "borderTopColor";
    readonly token: string;
}

/**
 * The colour-bearing surface of the widget layer.
 *
 * Every role that carries a colour appears at least once, which is what makes "a theme switch
 * restyles the widget layer" a claim about the LAYER rather than about one lucky element. The
 * container roles (`region`/`group`/`list`/`tree`) carry layout only, by design — they are covered by
 * the "every role has a kit rule" case instead.
 */
const COLOUR_EXPECTATIONS: readonly ColourExpectation[] = [
    { role: "heading", property: "color", token: "ink" },
    { role: "status", property: "color", token: "muted" },
    { role: "text", property: "color", token: "ink" },
    { role: "listitem", property: "color", token: "ink" },
    { role: "treeitem", property: "color", token: "ink" },
    { role: "button", property: "color", token: "ink" },
    { role: "button", property: "backgroundColor", token: "chip" },
    { role: "button", property: "borderTopColor", token: "line" },
    { role: "textbox", property: "color", token: "ink" },
    { role: "textbox", property: "backgroundColor", token: "panel2" },
    { role: "textbox", property: "borderTopColor", token: "line" },
];

/** Every widget class mounted at once, on its real tag, inside a real (laid-out) container. */
interface MountedWidgets {
    readonly elements: ReadonlyMap<string, HTMLElement>;
    dispose(): void;
}

function mountWidgets(): MountedWidgets {
    const host = window.document.createElement("div");
    // The kit is authored for the panel slot, so mount it in one: `.ctx-panel-body` is what supplies
    // the inherited type scale a widget's `font: inherit` picks up.
    host.className = "ctx-panel-body";
    window.document.body.appendChild(host);
    const elements = new Map<string, HTMLElement>();
    for (const [role, widgetClass] of Object.entries(WIDGET_CLASSES)) {
        const tag = ROLE_TAGS[role];
        assert(tag !== undefined, `the test knows which tag role \`${role}\` renders as`);
        const element = window.document.createElement(tag as string);
        if (role === "checkbox") {
            (element as HTMLInputElement).type = "checkbox";
        }
        element.classList.add(widgetClass);
        element.textContent = role;
        host.appendChild(element);
        elements.set(role, element);
    }
    return {
        elements,
        dispose: (): void => {
            host.remove();
        },
    };
}

/** An engine over the LIVE document root with the cross-fade off — see the header's warning. */
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

/** The served kit stylesheet, found by NAME so a CMake rename fails loudly instead of vacuously. */
function kitStyleSheet(): CSSStyleSheet {
    const sheets = [...window.document.styleSheets].filter(
        (sheet) => (sheet.href ?? "").endsWith(`/${KIT_STYLESHEET}`),
    );
    assert(
        sheets.length === 1,
        `exactly one served stylesheet is ${KIT_STYLESHEET} (found ${sheets.length}) — the harness ` +
            `page must <link> the SHIPPED kit stylesheet, or every assertion below is vacuous`,
    );
    return sheets[0] as CSSStyleSheet;
}

/** Every `.ctx-widget-*` class named by any selector in `sheet`. */
function widgetClassesStyledBy(sheet: CSSStyleSheet): Set<string> {
    const found = new Set<string>();
    const pattern = new RegExp(`\\.(${WIDGET_CLASS_PREFIX}[a-z0-9-]+)`, "g");
    for (const rule of [...sheet.cssRules]) {
        for (const match of rule.cssText.matchAll(pattern)) {
            const name = match[1];
            if (name !== undefined) {
                found.add(name);
            }
        }
    }
    return found;
}

function assertColoursMatch(mounted: MountedWidgets, themeId: string, document_: Doc): void {
    for (const expectation of COLOUR_EXPECTATIONS) {
        const element = mounted.elements.get(expectation.role);
        assert(element !== undefined, `the ${expectation.role} widget is mounted`);
        const computed = window.getComputedStyle(element as HTMLElement);
        assertEqual(
            computed[expectation.property],
            toRgb(colour(document_, expectation.token)),
            `${themeId}: the COMPUTED ${expectation.property} of \`.${WIDGET_CLASSES[expectation.role]}\` ` +
                `is the theme's colors.${expectation.token} — a widget that does not resolve to a token ` +
                `is one the theme cannot reach, which is the whole property e06c1 exists to establish`,
        );
    }
}

export const kitTests: readonly TestCase[] = [
    {
        name: "kit: the served kit stylesheet styles EVERY hydration role",
        run: () => {
            // The closed-set assertion, in the browser. `webui-kit-role-coverage` proves the same
            // property over the SOURCE on all three build legs; this one proves it over the bytes the
            // renderer actually received, which is what a staging or link-order mistake would break
            // without touching a single source file.
            const styled = widgetClassesStyledBy(kitStyleSheet());
            for (const [role, widgetClass] of Object.entries(WIDGET_CLASSES)) {
                assert(
                    styled.has(widgetClass),
                    `role \`${role}\` (.${widgetClass}) is styled by the served ${KIT_STYLESHEET} — ` +
                        `an unstyled role renders real, interactive DOM in the browser's own chrome ` +
                        `and nothing else reports it`,
                );
            }
        },
    },
    {
        name: "kit: the widget layer has exactly ONE styling owner among the served stylesheets",
        run: () => {
            // The DoD's "no surviving app.css duplicate path", asserted where it is decided: in the
            // document. A second sheet styling `.ctx-widget-*` makes appearance depend on load order
            // — which is settled by CMake staging and by a vendored engine's runtime injection, not
            // by anything a reader of either stylesheet can see.
            //
            // ⚠ WIDENED BY e06c2 FROM "one FILE" TO "one PACKAGE", which is the level the property
            // was always about. The kit now ships TWO sheets, and one authored family legitimately
            // reaches a widget class: a LIVE badge IS the `status` role primitive reused rather than
            // copied. The order-dependence hazard is closed by SPECIFICITY instead — asserted in the
            // very next case, and by `check_kit_tokens.py --role-coverage` on every `build` leg — so
            // this case keeps its exact original meaning for every NON-kit stylesheet.
            let inspected = 0;
            for (const sheet of [...window.document.styleSheets]) {
                const href = sheet.href ?? "";
                if (KIT_STYLESHEETS.some((name) => href.endsWith(`/${name}`))) {
                    continue;
                }
                let classes: Set<string>;
                try {
                    classes = widgetClassesStyledBy(sheet);
                } catch {
                    // A cross-origin sheet refuses `cssRules`. None is served here, but skipping it
                    // silently is what the `inspected` floor below exists to make impossible.
                    continue;
                }
                inspected += 1;
                assertEqual(
                    [...classes],
                    [],
                    `${href === "" ? "an injected <style>" : href} styles widget classes — the kit is ` +
                        `the single owner of that layer (e06c1)`,
                );
            }
            assert(
                inspected >= 2,
                `at least the app + dockview stylesheets were inspected (saw ${inspected}) — a run ` +
                    `where nothing was readable would pass this case having checked nothing`,
            );
        },
    },
    {
        name: "kit: a second kit sheet may only narrow a widget class by SPECIFICITY, never by order",
        run: () => {
            // The half of "one styling owner" that survives the kit having more than one stylesheet
            // (e06c2). `kit.css` owns the widget layer; any OTHER kit sheet that reaches a widget
            // class must compound a class onto it — `.ctx-widget-status.ctx-badge` is 0,2,0 and beats
            // the widget layer's 0,1,0 whichever sheet the browser reaches last. A BARE
            // `.ctx-widget-status` in a second sheet would TIE, and the winner would then be decided
            // by CMake staging order and by a vendored engine's runtime injection.
            //
            // Asserted here on the SERVED bytes as well as by `check_kit_tokens.py --role-coverage`
            // on the source: the source gate cannot see whether the sheet was served at all.
            // The lookahead EXCLUDES the class-name characters as well as the dot. Without
            // `[a-z0-9-]` in it, `[a-z0-9-]+` simply backtracks — `.ctx-widget-status.ctx-badge`
            // matches as `…statu` followed by `s`, and the "is it compounded" question is answered
            // about a prefix of the class name instead of the class name. That is the same
            // greedy-backtracking hole e06c1's own role regex fell into, one file over.
            const bare = new RegExp(`\\.${WIDGET_CLASS_PREFIX}[a-z0-9-]+(?![a-z0-9-.])`);
            const widget = new RegExp(`\\.${WIDGET_CLASS_PREFIX}[a-z0-9-]+`);
            let inspected = 0;
            for (const sheet of [...window.document.styleSheets]) {
                const href = sheet.href ?? "";
                if (href.endsWith(`/${KIT_STYLESHEET}`)) {
                    continue;
                }
                if (!KIT_STYLESHEETS.some((name) => href.endsWith(`/${name}`))) {
                    continue;
                }
                inspected += 1;
                for (const rule of [...sheet.cssRules]) {
                    const selector = rule.cssText.split("{")[0] ?? "";
                    if (!widget.test(selector)) {
                        continue;
                    }
                    assert(
                        !bare.test(selector),
                        `${href} narrows a widget class with the BARE selector \`${selector.trim()}\`` +
                            ` — only ${KIT_STYLESHEET} may do that. Compound a family class onto it ` +
                            `so it wins by specificity rather than by document order`,
                    );
                }
            }
            assert(
                inspected >= 1,
                `at least one non-${KIT_STYLESHEET} kit stylesheet was served and inspected (saw ` +
                    `${inspected}) — a run where none was readable would pass this case having ` +
                    `checked nothing`,
            );
        },
    },
    {
        name: "kit: every colour-bearing widget COMPUTES to the active theme's tokens",
        run: () => {
            const engine = documentEngine();
            const mounted = mountWidgets();
            try {
                applyToDocument(engine, BUILTIN_DARK);
                assertColoursMatch(mounted, BUILTIN_DARK, darkTheme as Doc);
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "kit: a theme switch restyles the whole widget layer with no widget-code change",
        run: () => {
            // The e06c1 DoD line, asserted where it is falsifiable. Nothing about the mounted widgets
            // changes between the two applies — no remount, no class edit, no re-render — so a layer
            // that only resolved correctly at MOUNT time passes the case above and fails here.
            const engine = documentEngine();
            const mounted = mountWidgets();
            try {
                applyToDocument(engine, BUILTIN_DARK);
                assertColoursMatch(mounted, BUILTIN_DARK, darkTheme as Doc);
                applyToDocument(engine, BUILTIN_LIGHT);
                assertColoursMatch(mounted, BUILTIN_LIGHT, lightTheme as Doc);
                applyToDocument(engine, BUILTIN_DARK);
                assertColoursMatch(mounted, BUILTIN_DARK, darkTheme as Doc);
            } finally {
                mounted.dispose();
                applyToDocument(engine, BUILTIN_DARK);
            }
        },
    },
    {
        name: "kit: the widget appearance is the THEME's, not a user-agent default",
        run: () => {
            // The guard against a vacuous pass of the two cases above. Dark and Light must DISAGREE
            // on every colour expectation: if a kit rule silently stopped applying, both themes would
            // resolve to the same inherited/UA value and the per-theme assertions could still pass on
            // one of them. Requiring the two to differ is what makes them honest.
            const engine = documentEngine();
            const mounted = mountWidgets();
            try {
                applyToDocument(engine, BUILTIN_DARK);
                const dark = COLOUR_EXPECTATIONS.map((expectation) =>
                    window.getComputedStyle(
                        mounted.elements.get(expectation.role) as HTMLElement,
                    )[expectation.property],
                );
                applyToDocument(engine, BUILTIN_LIGHT);
                const light = COLOUR_EXPECTATIONS.map((expectation) =>
                    window.getComputedStyle(
                        mounted.elements.get(expectation.role) as HTMLElement,
                    )[expectation.property],
                );
                for (let index = 0; index < COLOUR_EXPECTATIONS.length; index += 1) {
                    const expectation = COLOUR_EXPECTATIONS[index] as ColourExpectation;
                    assert(
                        dark[index] !== light[index],
                        `\`.${WIDGET_CLASSES[expectation.role]}\`'s ${expectation.property} differs ` +
                            `between Dark and Light — equal values mean the kit rule is not the thing ` +
                            `deciding it (an inherited or user-agent value would satisfy both themes)`,
                    );
                }
            } finally {
                mounted.dispose();
                applyToDocument(engine, BUILTIN_DARK);
            }
        },
    },
];
