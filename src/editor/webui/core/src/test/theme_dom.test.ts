// The theme engine's EFFECTIVE-VALUE tests (M9 e06b) — the only tier that reads what the browser
// actually COMPUTED rather than what the code intended.
//
// WHY THIS FILE EXISTS, AND WHY IT IS THE ONE TEST THAT WOULD HAVE SAVED TWO CI ROUND-TRIPS.
// Every other theme test asserts against a RecordingRoot: it proves `ThemeEngine` wrote the right
// variable names and values, which is necessary and completely blind to whether those values SURVIVE
// the cascade. Between the engine's `setProperty` and a painted pixel sit two things no unit test
// can see:
//
//   1. `dockview.css` declares the SAME `--dv-*` variables on the SAME `.dockview-theme-dark`
//      element, and dockview-core INJECTS that stylesheet into the document at runtime — so a
//      variable set at `:root` is out-ranked by an element-level declaration, and even an
//      element-level rule of equal specificity loses on document order. app.css wins it with
//      `html .dockview-theme-dark` (0,1,1); nothing but a computed read proves that still holds.
//   2. WHICH THEME IS ACTIVE. `colors.panel` is per-theme, so "the panel colour paints" is only
//      meaningful once you know which panel colour you are asking about.
//
// Both broke together at e06b and the ONLY signal was the CI-only `editor-cef-smoke-shell` leg —
// one full round-trip per attempt, and a local screenshot probe reported a confident 99.67% false
// positive because this dev host happened to prefer dark. These cases turn that into a sub-2s local
// check by asserting `getComputedStyle` on a REAL Dockview root, driven through the REAL app.css and
// the REAL pinned engine (harness.html loads all three — see src/editor/webui/CMakeLists.txt).
//
// SCOPE: this is the CASCADE + LIVE-SWITCH half. The pixel half (does the composited frame actually
// carry that colour over a tenth of its area) stays the live CEF smoke's; these tests make it
// impossible for that smoke to be the FIRST place a broken cascade is noticed.

import { assert, assertEqual, type TestCase } from "./harness.js";
import { DOCKVIEW_BACKGROUND_VARIABLES } from "./theme.test.js";
import {
    BUILTIN_DARK,
    BUILTIN_HC_DARK,
    BUILTIN_HC_LIGHT,
    BUILTIN_LIGHT,
    REDUCED_MOTION_QUERY,
    ThemeEngine,
    type ThemeDocument,
} from "../theme.js";
import { detectDockview, type DockviewApi, type DockviewContentRenderer } from "../dockview.js";

import darkTheme from "../../../tokens/themes/dark.theme.json";
import lightTheme from "../../../tokens/themes/light.theme.json";
import hcDarkTheme from "../../../tokens/themes/high-contrast-dark.theme.json";
import hcLightTheme from "../../../tokens/themes/high-contrast-light.theme.json";

const BUILTINS: readonly (readonly [string, ThemeDocument])[] = [
    [BUILTIN_DARK, darkTheme as ThemeDocument],
    [BUILTIN_LIGHT, lightTheme as ThemeDocument],
    [BUILTIN_HC_DARK, hcDarkTheme as ThemeDocument],
    [BUILTIN_HC_LIGHT, hcLightTheme as ThemeDocument],
];

function panelColour(document_: ThemeDocument): string {
    const colors = document_["colors"] as Record<string, unknown> | undefined;
    const panel = colors === undefined ? undefined : colors["panel"];
    assert(typeof panel === "string", "the built-in theme declares colors.panel");
    return (panel as string).toLowerCase();
}

/** `#0a0a0a` -> `rgb(10, 10, 10)` — the form `getComputedStyle` reports a resolved colour in. */
function toRgb(hex: string): string {
    const r = Number.parseInt(hex.slice(1, 3), 16);
    const g = Number.parseInt(hex.slice(3, 5), 16);
    const b = Number.parseInt(hex.slice(5, 7), 16);
    return `rgb(${r}, ${g}, ${b})`;
}

/** A live Dockview instance mounted into a detached-from-nothing, real, laid-out container. */
interface Mounted {
    readonly api: DockviewApi;
    readonly container: HTMLElement;
    dispose(): void;
}

/**
 * Mount a REAL Dockview with two real panels.
 *
 * A real one, not a `<div class="dockview-theme-dark">` stand-in: the whole point is that the
 * ENGINE's own element, class placement and injected stylesheet are in play. A hand-made element
 * would pass while the shipped app failed, which is the exact failure mode this file exists to
 * prevent.
 */
function mountDockview(): Mounted {
    const dockview = detectDockview();
    assert(
        dockview !== undefined,
        "the pinned dockview-core UMD global is loaded — harness.html must load " +
            "dockview-core.min.js before the test bundle, or this whole file passes vacuously",
    );
    // Named `dv`, not `engine`: everywhere else in this file `engine` is the THEME engine.
    const dv = dockview as NonNullable<typeof dockview>;
    const container = window.document.createElement("div");
    container.style.width = "800px";
    container.style.height = "600px";
    window.document.body.appendChild(container);
    const api = dv.createDockview(container, {
        theme: dv.themeDark,
        disableFloatingGroups: true,
        createComponent: (): DockviewContentRenderer => {
            const element = window.document.createElement("div");
            element.className = "ctx-panel-body";
            return { element, init: (): void => {} };
        },
    });
    api.addPanel({ id: "left", component: "left", title: "Left" });
    api.addPanel({
        id: "right",
        component: "right",
        title: "Right",
        position: { referencePanel: "left", direction: "right" },
    });
    api.layout(800, 600);
    return {
        api,
        container,
        dispose: (): void => {
            api.dispose();
            container.remove();
        },
    };
}

/** The element dockview-core puts its theme class on — the one the `--dv-*` cascade is decided at. */
function themedElement(mounted: Mounted): HTMLElement {
    const element = mounted.container.querySelector<HTMLElement>('[class*="dockview-theme"]');
    assert(element !== null, "dockview-core applied its theme class to an element we can read");
    return element as HTMLElement;
}

/**
 * An engine over the LIVE document root, with the 350 ms cross-fade OFF.
 *
 * ⚠ THE FADE IS NOT INCIDENTAL HERE — it silently breaks a synchronous colour read, and finding that
 * out cost this file a red run. app.css arms `transition: background-color
 * var(--ctx-theme-fade-duration)` on `html[data-theme-transition="true"] *` for exactly the fade's
 * duration, so immediately after a SWITCH `getComputedStyle(...).backgroundColor` reports the value
 * the transition is animating AWAY from — the OLD theme's colour — even though the underlying
 * variable already holds the new one. (The live CEF smokes never see this: they scan a settled frame
 * 30 seconds later.) Reduced motion collapses every duration to 0 ms, which makes the resolved
 * colour observable the instant it is applied — and reduced motion is an UNCONDITIONAL override
 * (06 §1), so this is the engine's own documented behaviour, not a test-only shortcut. What these
 * tests are about is the CASCADE; the fade has its own cases in theme.test.ts.
 */
function documentEngine(): ThemeEngine {
    return new ThemeEngine({
        root: window.document.documentElement,
        probe: (query: string) => ({ matches: query === REDUCED_MOTION_QUERY }),
    });
}

/** Apply a theme to the LIVE document root — the same root boot.ts hands the engine. */
function applyToDocument(engine: ThemeEngine, themeId: string): void {
    const report = engine.apply(themeId);
    assert(report.applied, `the engine applied ${themeId}: ${report.diagnostic}`);
    assertEqual(report.fadeDurationMs, 0, `${themeId} applied with no cross-fade in flight`);
}

function assertChromeMatches(mounted: Mounted, themeId: string, expectedHex: string): void {
    const style = window.getComputedStyle(themedElement(mounted));
    for (const variable of DOCKVIEW_BACKGROUND_VARIABLES) {
        assertEqual(
            style.getPropertyValue(variable).trim().toLowerCase(),
            expectedHex,
            `${themeId}: the COMPUTED ${variable} on Dockview's own themed element — a stylesheet ` +
                `that loses this cascade leaves the engine's stock greys and only the live CEF ` +
                `smoke would notice`,
        );
    }
    const groupView = mounted.container.querySelector<HTMLElement>(".dv-groupview");
    assert(groupView !== null, "the mounted Dockview has a group view to paint");
    assertEqual(
        window.getComputedStyle(groupView as HTMLElement).backgroundColor,
        toRgb(expectedHex),
        `${themeId}: the group view's RESOLVED background-color — this is the surface the live ` +
            `smokes' per-pixel coverage floor actually scans`,
    );
}

export const themeDomTests: readonly TestCase[] = [
    {
        name: "dom: every built-in theme's colors.panel is what Dockview's chrome COMPUTES to",
        run: () => {
            const engine = documentEngine();
            const mounted = mountDockview();
            try {
                for (const [themeId, document_] of BUILTINS) {
                    applyToDocument(engine, themeId);
                    assertChromeMatches(mounted, themeId, panelColour(document_));
                }
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "dom: a LIVE switch re-tokens the already-mounted docking chrome",
        run: () => {
            // The e06b live-switch claim, asserted where it is falsifiable: no remount, no reload —
            // the same Dockview instance must follow the variables the engine rewrites. A cascade
            // that only resolved correctly at MOUNT time would pass the case above and fail here.
            const engine = documentEngine();
            const mounted = mountDockview();
            try {
                applyToDocument(engine, BUILTIN_DARK);
                assertChromeMatches(mounted, BUILTIN_DARK, panelColour(darkTheme as ThemeDocument));
                applyToDocument(engine, BUILTIN_LIGHT);
                assertChromeMatches(mounted, BUILTIN_LIGHT, panelColour(lightTheme as ThemeDocument));
                applyToDocument(engine, BUILTIN_DARK);
                assertChromeMatches(mounted, BUILTIN_DARK, panelColour(darkTheme as ThemeDocument));
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "dom: a Dockview mounted AFTER a theme switch is themed too (D6 restore/recreate)",
        run: () => {
            // Dockview roots are destroyed and recreated by layout restore (D6), so the theming must
            // be a property of the CASCADE rather than of anything done once at first mount.
            const engine = documentEngine();
            applyToDocument(engine, BUILTIN_HC_LIGHT);
            const mounted = mountDockview();
            try {
                assertChromeMatches(
                    mounted,
                    BUILTIN_HC_LIGHT,
                    panelColour(hcLightTheme as ThemeDocument),
                );
            } finally {
                mounted.dispose();
                // Leave the document on the shipped first-run appearance for any later test.
                applyToDocument(engine, BUILTIN_DARK);
            }
        },
    },
    {
        name: "dom: the theme's panel colour is not merely dockview-core's own stock grey",
        run: () => {
            // The guard against a vacuous pass. dockview.css's stock dark surface is #1e1e1e; if a
            // future edit lost the cascade AND a theme happened to declare that same colour, the
            // assertions above would pass while the chrome was actually unthemed. Requiring the
            // computed value to differ from the vendored stock value keeps them honest.
            const engine = documentEngine();
            const mounted = mountDockview();
            try {
                applyToDocument(engine, BUILTIN_DARK);
                const computed = window
                    .getComputedStyle(themedElement(mounted))
                    .getPropertyValue("--dv-group-view-background-color")
                    .trim()
                    .toLowerCase();
                assert(
                    computed !== "#1e1e1e",
                    "the computed chrome colour is the THEME's, not dockview.css's stock #1e1e1e",
                );
                assertEqual(
                    computed,
                    panelColour(darkTheme as ThemeDocument),
                    "and it is exactly the Dark theme's colors.panel",
                );
            } finally {
                mounted.dispose();
            }
        },
    },
];
