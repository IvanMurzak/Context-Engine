// T1 unit tests for the M9 e06b THEME ENGINE (theme.ts; design 06 §1-§4 / 04 §5, D11/D12).
//
// These are the DoD gates that can be proven without a live browser chrome: the token -> CSS
// custom-property flattening, the Dockview chrome map, the Pulse-of-Work derivation, the
// UNCONDITIONAL reduced-motion override (motion dropped, state colour RETAINED — the owner-picked
// static fallback, NOT an aurora), live switching, package/user theme validation-on-load, the
// generation-gated hot reload, and CSP-safe iframe delivery.
//
// They run in the SAME headless-Chromium `webui-ts-unit` tier as the rest of editor-core (e07a). The
// engine takes its root through a tiny structural interface, so these drive a RECORDING root rather
// than `document.documentElement`: the assertions then read exactly what was written, in order, with
// no interference from the harness page's own styles — and the same code path runs in production,
// where `HTMLElement` satisfies the same interface.
//
// The built-in `*.theme.json` files are imported by theme.ts ITSELF, so these tests exercise the
// EXACT shipped token data (the e06a discipline, one link further down the chain).

import { assert, assertEqual, type TestCase } from "./harness.js";
import {
    BUILTIN_DARK,
    BUILTIN_HC_DARK,
    BUILTIN_HC_LIGHT,
    BUILTIN_LIGHT,
    EditorUiBus,
    FLOURISH_STATES,
    IframeThemeChannel,
    LIGHT_SCHEME_QUERY,
    REDUCED_MOTION_QUERY,
    THEMES_GET_METHOD,
    THEME_CHANGED_EVENT,
    THEME_PIN_FLAG,
    THEME_TRANSITION_ATTRIBUTE,
    ThemeController,
    ThemeEngine,
    ThemeRegistry,
    TOKEN_VARIABLE_PREFIX,
    applyMotionPreference,
    bootThemeId,
    defaultMediaQueryProbe,
    defaultThemeId,
    kebabCase,
    parseDurationMs,
    parsePinnedThemeId,
    parseThemesSnapshot,
    prefersReducedMotion,
    themeCssVariables,
    type EditorUiEvent,
    type MediaQueryLike,
    type MediaQueryProbe,
    type ThemeDocument,
    type ThemeRoot,
    type ThemesSnapshot,
    type ThemesSource,
} from "../theme.js";

import darkTheme from "../../../tokens/themes/dark.theme.json";
import lightTheme from "../../../tokens/themes/light.theme.json";

/**
 * The five Dockview variables that decide the PAINTED REGION the live CEF smokes' coverage floor is
 * calibrated against, in ONE place because TWO tiers assert about them and they must agree: the case
 * below proves they all RESOLVE to `colors.panel`, and `theme_dom.test.ts` proves the browser still
 * COMPUTES that value on a real Dockview root after the cascade has had its say. A kit change that
 * gives the tab strip its own step has to move the smokes' `kAppBackground*` constants in the same
 * diff — this list is that tripwire, and duplicating it would let the two tiers drift apart.
 */
export const DOCKVIEW_BACKGROUND_VARIABLES = [
    "--dv-background-color",
    "--dv-group-view-background-color",
    "--dv-tabs-and-actions-container-background-color",
    "--dv-activegroup-visiblepanel-tab-background-color",
    "--dv-inactivegroup-visiblepanel-tab-background-color",
] as const;

// --------------------------------------------------------------------------------- fakes

/** A `ThemeRoot` that records everything written to it — the engine's observable side effects. */
class RecordingRoot implements ThemeRoot {
    readonly properties = new Map<string, string>();
    readonly attributes = new Map<string, string>();
    readonly style = {
        setProperty: (property: string, value: string): void => {
            this.properties.set(property, value);
        },
    };

    setAttribute(qualifiedName: string, value: string): void {
        this.attributes.set(qualifiedName, value);
    }
}

/** An iframe `contentWindow` stand-in: records the posts, in order, with their target origins. */
class RecordingFrame {
    readonly posts: { message: unknown; targetOrigin: string }[] = [];

    postMessage(message: unknown, targetOrigin: string): void {
        this.posts.push({ message, targetOrigin });
    }
}

/** A probe that answers exactly one query with `matches: true`; everything else is false. */
function probeMatching(query: string): MediaQueryProbe {
    return (asked: string): MediaQueryLike => ({ matches: asked === query });
}

/** A probe that cannot answer anything — the bare-harness case. */
const NULL_PROBE: MediaQueryProbe = (): null => null;

/** A fake Shell feed, so the controller is driven without a bridge. */
class FakeThemesSource implements ThemesSource {
    #next: ThemesSnapshot = { generation: 0, themes: [] };

    set(snapshot: ThemesSnapshot): void {
        this.#next = snapshot;
    }

    get(): Promise<ThemesSnapshot> {
        return Promise.resolve(this.#next);
    }
}

/** A minimal but VALID user theme: the shipped Dark document under a different name. */
function userThemeText(name: string, ink: string): string {
    const document = { ...(darkTheme as Record<string, unknown>), name, colors: {
        ...((darkTheme as { colors: Record<string, unknown> }).colors), ink,
    } };
    return JSON.stringify(document);
}

function engineWith(probe: MediaQueryProbe): { engine: ThemeEngine; root: RecordingRoot } {
    const root = new RecordingRoot();
    const engine = new ThemeEngine({ root, probe });
    return { engine, root };
}

export const themeTests: readonly TestCase[] = [
    // ------------------------------------------------------------------ variable naming + flattening
    {
        name: "kebabCase: camelCase splits, digits and existing lowercase are left alone",
        run: () => {
            assertEqual(kebabCase("focusRing"), "focus-ring", "camelCase splits");
            assertEqual(kebabCase("fontUi"), "font-ui", "two-letter tail splits");
            assertEqual(kebabCase("panel2"), "panel2", "a trailing digit is NOT a word boundary");
            assertEqual(kebabCase("2xs"), "2xs", "a size key survives verbatim");
            assertEqual(kebabCase("canvas"), "canvas", "already-lowercase is identity");
        },
    },
    {
        name: "themeCssVariables: every token group is flattened to --ctx-<group>-<path>",
        run: () => {
            const vars = themeCssVariables(darkTheme as ThemeDocument);
            assertEqual(vars[`${TOKEN_VARIABLE_PREFIX}colors-canvas`], "#000000", "colors.canvas");
            assertEqual(vars[`${TOKEN_VARIABLE_PREFIX}colors-panel`], "#0a0a0a", "colors.panel");
            assertEqual(
                vars[`${TOKEN_VARIABLE_PREFIX}colors-semantic-good`],
                "#3fb950",
                "a NESTED group flattens with its full path",
            );
            assertEqual(
                vars[`${TOKEN_VARIABLE_PREFIX}colors-focus-ring`],
                "rgba(237, 237, 237, 0.6)",
                "a camelCase token is kebab-cased",
            );
            assertEqual(
                vars[`${TOKEN_VARIABLE_PREFIX}typography-size-base`],
                "13.5px",
                "typography.size.base",
            );
            assertEqual(vars[`${TOKEN_VARIABLE_PREFIX}shape-radius-sm`], "7px", "shape.radius.sm");
            assertEqual(
                vars[`${TOKEN_VARIABLE_PREFIX}viewport-axis-x`],
                "#ff5a5f",
                "the viewport chroma exception is a token like any other",
            );
            assertEqual(
                vars[`${TOKEN_VARIABLE_PREFIX}elevation-model`],
                "border-step",
                "an enum leaf carries its token verbatim",
            );
        },
    },
    {
        name: "themeCssVariables: numeric and boolean leaves render as CSS-usable strings",
        run: () => {
            const vars = themeCssVariables(darkTheme as ThemeDocument);
            assertEqual(
                vars[`${TOKEN_VARIABLE_PREFIX}typography-line-height`],
                "1.45",
                "a number becomes its literal",
            );
            assertEqual(
                vars[`${TOKEN_VARIABLE_PREFIX}typography-weight-regular`],
                "430",
                "an integer weight becomes its literal",
            );
            assertEqual(
                vars[`${TOKEN_VARIABLE_PREFIX}motion-enable-spring-pills`],
                "true",
                "a boolean becomes true/false so a rule can branch on it",
            );
        },
    },
    {
        name: "themeCssVariables: the envelope is NOT emitted as tokens",
        run: () => {
            const vars = themeCssVariables(darkTheme as ThemeDocument);
            for (const key of Object.keys(vars)) {
                assert(
                    !key.includes("$schema") && key !== `${TOKEN_VARIABLE_PREFIX}version`,
                    `metadata must not leak into the token set: ${key}`,
                );
            }
        },
    },

    // ------------------------------------------------------------------ Dockview chrome (06 §2, D2)
    {
        name: "Dockview chrome is skinned from the SAME tokens (--dv-* resolved, not var() indirection)",
        run: () => {
            const vars = themeCssVariables(darkTheme as ThemeDocument);
            assertEqual(
                vars["--dv-group-view-background-color"],
                "#0a0a0a",
                "the docking surface takes colors.panel",
            );
            assertEqual(vars["--dv-tab-divider-color"], "#262626", "dividers take colors.line");
            assertEqual(
                vars["--dv-activegroup-visiblepanel-tab-color"],
                "#ededed",
                "the active tab's ink takes colors.ink",
            );
            assertEqual(
                vars["--dv-paneview-active-outline-color"],
                "#ededed",
                "the active outline takes colors.accent",
            );
            const light = themeCssVariables(lightTheme as ThemeDocument);
            assert(
                light["--dv-group-view-background-color"] !== vars["--dv-group-view-background-color"],
                "the chrome genuinely re-tokens between themes (the switch is not a no-op)",
            );
        },
    },
    {
        name: "the five Dockview BACKGROUND variables share one token (the CEF-smoke coverage lockstep)",
        run: () => {
            // The live editor-cef-smoke-shell / -restore legs scan the composited frame for ONE
            // colour and require it over a tenth of the surface. app.css previously pointed exactly
            // these five at the single placeholder --editor-bg; keeping them on ONE token preserves
            // the painted REGION and moves only the colour, so the smokes' coverage floor is
            // unchanged. A future kit change that gives the tab strip its own step must move the
            // smokes' kAppBackground* constants in the SAME diff — this test is that tripwire.
            const vars = themeCssVariables(darkTheme as ThemeDocument);
            const panel = vars[`${TOKEN_VARIABLE_PREFIX}colors-panel`];
            for (const variable of DOCKVIEW_BACKGROUND_VARIABLES) {
                assertEqual(vars[variable], panel, `${variable} resolves to colors.panel`);
            }
            assert(
                panel !== undefined && panel !== "#000000",
                "the anchor colour is distinguishable from a zero-filled (black) surface",
            );
        },
    },

    // ------------------------------------------------------------------ Pulse of Work (TOKENS.md §5)
    {
        name: "Pulse of Work: each state resolves to its RESERVED status hue (zero new colour tokens)",
        run: () => {
            const vars = themeCssVariables(darkTheme as ThemeDocument);
            const expected: Readonly<Record<string, string>> = {
                idle: "colors-semantic-idle",
                running: "colors-semantic-good",
                compiling: "colors-semantic-warn",
                error: "colors-semantic-bad",
                paused: "colors-semantic-idle",
            };
            for (const state of FLOURISH_STATES) {
                const token = expected[state];
                assert(token !== undefined, `the test names a hue for ${state}`);
                assertEqual(
                    vars[`${TOKEN_VARIABLE_PREFIX}flourish-${state}-color`],
                    vars[`${TOKEN_VARIABLE_PREFIX}${token ?? ""}`],
                    `${state} resolves to the reserved ${token} hue, not a new colour`,
                );
            }
        },
    },
    {
        name: "Pulse of Work: rhythm maps to a keyframes name and paused animates NOTHING",
        run: () => {
            const vars = themeCssVariables(darkTheme as ThemeDocument);
            assertEqual(
                vars[`${TOKEN_VARIABLE_PREFIX}flourish-idle-animation`],
                "ctx-state-breathe",
                "idle breathes",
            );
            assertEqual(
                vars[`${TOKEN_VARIABLE_PREFIX}flourish-running-animation`],
                "ctx-state-breathe",
                "running breathes",
            );
            assertEqual(
                vars[`${TOKEN_VARIABLE_PREFIX}flourish-compiling-animation`],
                "ctx-state-pulse",
                "compiling pulses",
            );
            assertEqual(
                vars[`${TOKEN_VARIABLE_PREFIX}flourish-error-animation`],
                "ctx-state-pulse",
                "error pulses",
            );
            assertEqual(
                vars[`${TOKEN_VARIABLE_PREFIX}flourish-paused-animation`],
                "none",
                "paused is frozen",
            );
        },
    },
    {
        name: "Pulse of Work: animation speed tracks activity level (compiling fastest, idle slowest)",
        run: () => {
            const vars = themeCssVariables(darkTheme as ThemeDocument);
            const ms = (state: string): number =>
                parseDurationMs(vars[`${TOKEN_VARIABLE_PREFIX}flourish-${state}-duration`] ?? "");
            assertEqual(ms("idle"), 7000, "idle breathes at 7s — at rest, slowest");
            assertEqual(ms("running"), 2600, "running breathes at 2.6s — live, middle");
            assertEqual(ms("compiling"), 950, "compiling pulses at 0.95s — active work, fastest");
            assertEqual(ms("error"), 1400, "error pulses at 1.4s — an insistent alert");
            assert(
                ms("compiling") < ms("error") && ms("error") < ms("running") && ms("running") < ms("idle"),
                "the owner-directed ordering holds: compiling < error < running < idle",
            );
        },
    },

    // ------------------------------------------------------------------ durations
    {
        name: "parseDurationMs: ms / s / zero / garbage all resolve totally",
        run: () => {
            assertEqual(parseDurationMs("350ms"), 350, "milliseconds");
            assertEqual(parseDurationMs("2.6s"), 2600, "fractional seconds");
            assertEqual(parseDurationMs("0s"), 0, "zero");
            assertEqual(parseDurationMs(" 120ms "), 120, "surrounding whitespace");
            assertEqual(parseDurationMs("fast"), 0, "an unparseable value is 0, never NaN");
            assertEqual(parseDurationMs(""), 0, "empty is 0");
            assertEqual(parseDurationMs("-5s"), 0, "a negative duration is refused, not applied");
        },
    },

    // ------------------------------------------------------------------ reduced motion (UNCONDITIONAL)
    {
        name: "applyMotionPreference(false) changes nothing (the override is genuinely conditional on the user)",
        run: () => {
            const base = themeCssVariables(darkTheme as ThemeDocument);
            assertEqual(applyMotionPreference(base, false), base, "not-reduced is the identity");
        },
    },
    {
        name: "reduced motion drops EVERY duration and animation, unconditionally",
        run: () => {
            const reduced = applyMotionPreference(themeCssVariables(darkTheme as ThemeDocument), true);
            for (const key of Object.keys(reduced)) {
                if (key.startsWith(`${TOKEN_VARIABLE_PREFIX}motion-duration-`)) {
                    assertEqual(parseDurationMs(reduced[key] ?? ""), 0, `${key} collapsed to zero`);
                }
            }
            for (const state of FLOURISH_STATES) {
                assertEqual(
                    reduced[`${TOKEN_VARIABLE_PREFIX}flourish-${state}-animation`],
                    "none",
                    `${state} animates nothing under reduced motion`,
                );
                assertEqual(
                    parseDurationMs(reduced[`${TOKEN_VARIABLE_PREFIX}flourish-${state}-duration`] ?? ""),
                    0,
                    `${state} has no duration under reduced motion`,
                );
            }
            assertEqual(
                reduced[`${TOKEN_VARIABLE_PREFIX}motion-enable-spring-pills`],
                "false",
                "spring pills are disabled even though the theme enables them",
            );
            assertEqual(
                parseDurationMs(reduced[`${TOKEN_VARIABLE_PREFIX}theme-fade-duration`] ?? ""),
                0,
                "the 350ms cross-fade becomes instant",
            );
            assertEqual(reduced[`${TOKEN_VARIABLE_PREFIX}motion-reduced`], "1", "the switch is set");
        },
    },
    {
        name: "reduced motion RETAINS the state colour and opacity — the static Pulse-of-Work fallback",
        run: () => {
            const base = themeCssVariables(darkTheme as ThemeDocument);
            const reduced = applyMotionPreference(base, true);
            for (const state of FLOURISH_STATES) {
                assertEqual(
                    reduced[`${TOKEN_VARIABLE_PREFIX}flourish-${state}-color`],
                    base[`${TOKEN_VARIABLE_PREFIX}flourish-${state}-color`],
                    `${state} keeps its status colour (motion dropped, colour retained — NOT an aurora)`,
                );
                assertEqual(
                    reduced[`${TOKEN_VARIABLE_PREFIX}flourish-${state}-opacity`],
                    base[`${TOKEN_VARIABLE_PREFIX}flourish-${state}-opacity`],
                    `${state} keeps its opacity`,
                );
            }
            assertEqual(
                reduced[`${TOKEN_VARIABLE_PREFIX}colors-semantic-warn`],
                base[`${TOKEN_VARIABLE_PREFIX}colors-semantic-warn`],
                "no colour token is touched by the motion policy",
            );
        },
    },
    {
        name: "prefersReducedMotion / defaultThemeId read the media queries, and fail honest when they cannot",
        run: () => {
            assert(prefersReducedMotion(probeMatching(REDUCED_MOTION_QUERY)), "reduce is honoured");
            assert(!prefersReducedMotion(probeMatching("(min-width: 0px)")), "an unrelated match is not reduce");
            assert(!prefersReducedMotion(NULL_PROBE), "an undetectable preference is not a claimed one");
            assertEqual(defaultThemeId(probeMatching(LIGHT_SCHEME_QUERY)), BUILTIN_LIGHT, "light scheme");
            assertEqual(defaultThemeId(NULL_PROBE), BUILTIN_DARK, "Dark when undetectable (C-F22)");
            // The default probe must be total in a scope with no matchMedia at all.
            assertEqual(defaultThemeId(defaultMediaQueryProbe({})), BUILTIN_DARK, "no matchMedia -> Dark");
        },
    },

    // ------------------------------------------------------------------ apply + live switch
    {
        name: "apply: writes the variables, marks the document, and does NOT fade the first paint",
        run: () => {
            const { engine, root } = engineWith(NULL_PROBE);
            const report = engine.apply(BUILTIN_DARK);
            assert(report.applied, "the built-in applied");
            assertEqual(report.themeId, BUILTIN_DARK, "the active id");
            assertEqual(report.appearance, "dark", "appearance");
            assertEqual(report.reducedMotion, false, "no reduced-motion preference was expressed");
            assertEqual(report.fadeDurationMs, 0, "there is nothing to cross-fade FROM at boot");
            assertEqual(
                root.attributes.get(THEME_TRANSITION_ATTRIBUTE),
                "false",
                "so the transition is never armed for the first paint",
            );
            assertEqual(report.diagnostic, "", "no diagnostic on success");
            assertEqual(
                root.properties.get(`${TOKEN_VARIABLE_PREFIX}colors-canvas`),
                "#000000",
                "the token reached the root",
            );
            assertEqual(root.attributes.get("data-theme"), BUILTIN_DARK, "data-theme");
            assertEqual(root.attributes.get("data-appearance"), "dark", "data-appearance");
            assertEqual(root.attributes.get("data-high-contrast"), "false", "data-high-contrast");
            assertEqual(root.attributes.get("data-reduced-motion"), "false", "data-reduced-motion");
            assert(report.variableCount === root.properties.size, "every reported variable was written");
        },
    },
    {
        name: "live switch: Dark -> Light -> HC re-tokens with no restart",
        run: () => {
            const { engine, root } = engineWith(NULL_PROBE);
            engine.apply(BUILTIN_DARK);
            assertEqual(root.properties.get(`${TOKEN_VARIABLE_PREFIX}colors-ink`), "#ededed", "dark ink");
            engine.apply(BUILTIN_LIGHT);
            assertEqual(root.properties.get(`${TOKEN_VARIABLE_PREFIX}colors-ink`), "#171717", "light ink");
            assertEqual(root.attributes.get("data-appearance"), "light", "appearance followed");
            assertEqual(
                root.properties.get("--dv-group-view-background-color"),
                root.properties.get(`${TOKEN_VARIABLE_PREFIX}colors-panel`),
                "the docking chrome re-tokened with the rest",
            );
            const hc = engine.apply(BUILTIN_HC_LIGHT);
            assert(hc.applied && hc.highContrast, "the high-contrast pair is reachable live");
            assertEqual(root.attributes.get("data-high-contrast"), "true", "HC is marked on the root");
        },
    },
    {
        name: "a SWITCH runs the 350ms cross-fade, armed for exactly its own duration",
        run: () => {
            const root = new RecordingRoot();
            const pending: { callback: () => void; delayMs: number }[] = [];
            const engine = new ThemeEngine({
                root,
                probe: NULL_PROBE,
                schedule: (callback, delayMs) => pending.push({ callback, delayMs }),
            });
            engine.apply(BUILTIN_DARK); // first paint: no fade, nothing scheduled
            assertEqual(pending.length, 0, "the first apply schedules no disarm");

            const switched = engine.apply(BUILTIN_LIGHT);
            assertEqual(switched.fadeDurationMs, 350, "the 350ms theme cross-fade (06 §2)");
            assertEqual(
                root.attributes.get(THEME_TRANSITION_ATTRIBUTE),
                "true",
                "the transition is ARMED across the switch",
            );
            assertEqual(
                root.properties.get(`${TOKEN_VARIABLE_PREFIX}theme-fade-duration`),
                "350ms",
                "and the duration CSS reads is the same one the report names",
            );
            assertEqual(pending.length, 1, "exactly one disarm was scheduled");
            assertEqual(pending[0]?.delayMs, 350, "scheduled for the fade's own duration");
            pending[0]?.callback();
            assertEqual(
                root.attributes.get(THEME_TRANSITION_ATTRIBUTE),
                "false",
                "and disarmed afterwards, so hovers are never slowed for the rest of the session",
            );
        },
    },
    {
        name: "apply under reduced motion: fade is instant, colours survive, the root is marked",
        run: () => {
            const { engine, root } = engineWith(probeMatching(REDUCED_MOTION_QUERY));
            engine.apply(BUILTIN_LIGHT);
            // A SWITCH, so the 0ms is the reduced-motion override rather than the first-paint rule.
            const report = engine.apply(BUILTIN_DARK);
            assert(report.applied && report.reducedMotion, "the preference was honoured");
            assertEqual(report.fadeDurationMs, 0, "no cross-fade under reduced motion");
            assertEqual(
                root.attributes.get(THEME_TRANSITION_ATTRIBUTE),
                "false",
                "and the transition is never armed",
            );
            assertEqual(root.attributes.get("data-reduced-motion"), "true", "the root is marked");
            assertEqual(
                root.properties.get(`${TOKEN_VARIABLE_PREFIX}flourish-compiling-animation`),
                "none",
                "the flourish is frozen",
            );
            assertEqual(
                root.properties.get(`${TOKEN_VARIABLE_PREFIX}flourish-compiling-color`),
                root.properties.get(`${TOKEN_VARIABLE_PREFIX}colors-semantic-warn`),
                "and keeps its state colour",
            );
        },
    },
    {
        name: "apply: an unknown theme id is REFUSED and the previous theme stands",
        run: () => {
            const { engine, root } = engineWith(NULL_PROBE);
            engine.apply(BUILTIN_LIGHT);
            const refused = engine.apply("user.does-not-exist");
            assert(!refused.applied, "the apply was refused");
            assert(refused.diagnostic.includes("user.does-not-exist"), "the diagnostic names the id");
            assertEqual(engine.activeId, BUILTIN_LIGHT, "the active theme is unchanged");
            assertEqual(
                root.properties.get(`${TOKEN_VARIABLE_PREFIX}colors-ink`),
                "#171717",
                "and the document still carries the previous theme's tokens",
            );
        },
    },
    {
        name: "toggleAppearance: flips dark<->light and PRESERVES high contrast",
        run: () => {
            const { engine } = engineWith(NULL_PROBE);
            engine.apply(BUILTIN_DARK);
            assertEqual(engine.toggleAppearance().themeId, BUILTIN_LIGHT, "dark -> light");
            assertEqual(engine.toggleAppearance().themeId, BUILTIN_DARK, "light -> dark");
            engine.apply(BUILTIN_HC_DARK);
            assertEqual(
                engine.toggleAppearance().themeId,
                BUILTIN_HC_LIGHT,
                "a high-contrast user keeps high contrast across the toggle",
            );
        },
    },

    // ------------------------------------------------------------------ the editor.ui stub bus
    {
        name: "apply emits editor.ui.theme-changed with the e08 envelope",
        run: () => {
            const { engine } = engineWith(NULL_PROBE);
            const seen: EditorUiEvent[] = [];
            engine.bus.subscribe(THEME_CHANGED_EVENT, (event) => seen.push(event));
            engine.apply(BUILTIN_LIGHT);
            assertEqual(seen.length, 1, "one event per switch");
            const event = seen[0];
            assert(event !== undefined, "the event was delivered");
            assertEqual(event?.type, THEME_CHANGED_EVENT, "the event name is e08's");
            assertEqual(event?.payload.themeId, BUILTIN_LIGHT, "the payload names the theme");
            assertEqual(event?.payload.appearance, "light", "and its appearance");
            assertEqual(
                event?.payload.variables[`${TOKEN_VARIABLE_PREFIX}colors-ink`],
                "#171717",
                "and carries the full variable set a consumer needs to re-token",
            );
        },
    },
    {
        name: "the bus isolates a throwing subscriber and honours dispose()",
        run: () => {
            const bus = new EditorUiBus();
            let reached = 0;
            bus.subscribe(THEME_CHANGED_EVENT, () => {
                throw new Error("a panel blew up");
            });
            const subscription = bus.subscribe(THEME_CHANGED_EVENT, () => {
                reached += 1;
            });
            const event: EditorUiEvent = {
                type: THEME_CHANGED_EVENT,
                payload: {
                    themeId: BUILTIN_DARK,
                    name: "Dark",
                    appearance: "dark",
                    highContrast: false,
                    reducedMotion: false,
                    variables: {},
                },
            };
            bus.emit(event);
            assertEqual(reached, 1, "one subscriber's failure does not deny the others");
            subscription.dispose();
            bus.emit(event);
            assertEqual(reached, 1, "a disposed subscription stops receiving");
        },
    },

    // ------------------------------------------------------------------ iframe delivery (04 §5)
    {
        name: "iframe delivery: tokens are POSTED as data (no script, no DOM reach-in) on every switch",
        run: () => {
            const { engine } = engineWith(NULL_PROBE);
            const frame = new RecordingFrame();
            engine.iframes.register(frame);
            engine.apply(BUILTIN_DARK);
            engine.apply(BUILTIN_LIGHT);
            assertEqual(frame.posts.length, 2, "the frame is re-tokened on every theme change");
            const last = frame.posts[1];
            assert(last !== undefined, "the second post was recorded");
            assertEqual(last?.targetOrigin, "*", "a sandboxed frame's origin is opaque, so '*'");
            const message = last?.message as EditorUiEvent;
            assertEqual(message.type, THEME_CHANGED_EVENT, "the SAME envelope the bus emits");
            assertEqual(
                message.payload.variables[`${TOKEN_VARIABLE_PREFIX}colors-ink`],
                "#171717",
                "carrying the tokens the panel applies to its own root",
            );
            assert(
                JSON.stringify(message).indexOf("<script") === -1,
                "the payload is DATA — nothing script-shaped crosses the boundary",
            );
        },
    },
    {
        name: "iframe delivery: a frame registered mid-session is tokened immediately, never unthemed",
        run: () => {
            const { engine } = engineWith(NULL_PROBE);
            engine.apply(BUILTIN_DARK);
            const late = new RecordingFrame();
            engine.iframes.register(late);
            assertEqual(late.posts.length, 1, "the current theme is posted on registration");
            engine.iframes.unregister(late);
            engine.apply(BUILTIN_LIGHT);
            assertEqual(late.posts.length, 1, "an unregistered frame stops receiving");
        },
    },
    {
        name: "iframe delivery: a throwing/detached frame cannot break the switch for the others",
        run: () => {
            const channel = new IframeThemeChannel();
            const healthy = new RecordingFrame();
            channel.register({
                postMessage: (): void => {
                    throw new Error("detached frame");
                },
            });
            channel.register(healthy);
            assertEqual(channel.size, 2, "both frames are registered");
            channel.broadcast({
                type: THEME_CHANGED_EVENT,
                payload: {
                    themeId: BUILTIN_DARK,
                    name: "Dark",
                    appearance: "dark",
                    highContrast: false,
                    reducedMotion: false,
                    variables: {},
                },
            });
            assertEqual(healthy.posts.length, 1, "the healthy frame still got its tokens");
        },
    },

    // ------------------------------------------------------------------ contributed themes (load-time validation)
    {
        name: "registry: the four built-ins are present and are all that exists before any contribution",
        run: () => {
            const registry = new ThemeRegistry();
            assertEqual(registry.list().length, 4, "exactly the four built-ins");
            for (const id of [BUILTIN_DARK, BUILTIN_LIGHT, BUILTIN_HC_DARK, BUILTIN_HC_LIGHT]) {
                assert(registry.has(id), `${id} is registered`);
            }
            assertEqual(registry.idsFrom("user").length, 0, "no user themes yet");
        },
    },
    {
        name: "registry: a VALID contribution is accepted from either source and becomes switchable",
        run: () => {
            const registry = new ThemeRegistry();
            const result = registry.replaceContributed(["user", "package"], [
                { id: "user.mine", source: "user", text: userThemeText("Mine", "#abcdef") },
                { id: "package.acme.dusk", source: "package", text: userThemeText("Dusk", "#fedcba") },
            ]);
            assertEqual(result.rejected, [], "nothing was rejected");
            assertEqual(result.accepted.length, 2, "both were accepted");
            assertEqual(registry.get("user.mine")?.name, "Mine", "the display name came from the file");
            assertEqual(registry.get("package.acme.dusk")?.source, "package", "the source is recorded");
        },
    },
    {
        name: "registry: a malformed / schema-invalid contribution is REJECTED with a diagnostic, never applied",
        run: () => {
            const registry = new ThemeRegistry();
            const result = registry.replaceContributed(["user"], [
                { id: "user.broken-json", source: "user", text: "{ not json" },
                {
                    id: "user.unknown-key",
                    source: "user",
                    text: JSON.stringify({ ...(darkTheme as Record<string, unknown>), surprise: 1 }),
                },
                { id: "user.not-a-theme", source: "user", text: JSON.stringify({ hello: "world" }) },
                { id: "user.good", source: "user", text: userThemeText("Good", "#123456") },
            ]);
            assertEqual(result.accepted, ["user.good"], "the healthy sibling still loaded");
            assertEqual(result.rejected.length, 3, "each bad theme was refused individually");
            const byId = new Map(result.rejected.map((entry) => [entry.id, entry.diagnostic]));
            assert(
                (byId.get("user.broken-json") ?? "").includes("not valid JSON"),
                "unparseable JSON is named as such",
            );
            assert(
                (byId.get("user.unknown-key") ?? "").includes("surprise"),
                "the schema rejection names the offending key",
            );
            assert(
                (byId.get("user.not-a-theme") ?? "").includes("schema"),
                "a structurally wrong document is refused by the schema",
            );
            assert(!registry.has("user.broken-json"), "a rejected theme is NOT switchable");
        },
    },
    {
        name: "registry: a contribution cannot shadow a built-in id or a sibling contribution",
        run: () => {
            const registry = new ThemeRegistry();
            const result = registry.replaceContributed(["user"], [
                { id: BUILTIN_DARK, source: "user", text: userThemeText("Impostor", "#ff0000") },
                { id: "user.dup", source: "user", text: userThemeText("First", "#111111") },
                { id: "user.dup", source: "user", text: userThemeText("Second", "#222222") },
            ]);
            assertEqual(result.accepted, ["user.dup"], "only the first of the duplicates loaded");
            assertEqual(result.rejected.length, 2, "the built-in claim and the duplicate were refused");
            assertEqual(registry.get(BUILTIN_DARK)?.source, "builtin", "Dark is still the built-in");
            assertEqual(registry.get("user.dup")?.name, "First", "first writer wins, deterministically");
        },
    },
    {
        name: "registry: replaceContributed REPLACES the contributed set (a deleted file disappears)",
        run: () => {
            const registry = new ThemeRegistry();
            registry.replaceContributed(["user", "package"], [
                { id: "user.a", source: "user", text: userThemeText("A", "#111111") },
                { id: "user.b", source: "user", text: userThemeText("B", "#222222") },
            ]);
            assertEqual(registry.idsFrom("user").length, 2, "both loaded");
            registry.replaceContributed(["user", "package"], [
                { id: "user.a", source: "user", text: userThemeText("A", "#111111") },
            ]);
            assertEqual(registry.idsFrom("user"), ["user.a"], "the removed file is gone");
            assertEqual(registry.list().length, 5, "and the built-ins were never touched");
        },
    },

    // ------------------------------------------------------------------ the Shell feed + hot reload
    {
        name: "parseThemesSnapshot: total against a malformed or partial envelope",
        run: () => {
            assertEqual(parseThemesSnapshot(null), { generation: 0, themes: [] }, "null");
            assertEqual(parseThemesSnapshot({}), { generation: 0, themes: [] }, "an empty object");
            assertEqual(
                parseThemesSnapshot({ generation: "3", themes: "nope" }),
                { generation: 0, themes: [] },
                "wrong member types degrade rather than throw",
            );
            const parsed = parseThemesSnapshot({
                generation: 4,
                themes: [
                    { id: "user.a", source: "user", text: "{}" },
                    { id: "pkg.b", source: "package", text: "{}" },
                    { id: "user.c", source: "martian", text: "{}" },
                    { id: "", source: "user", text: "{}" },
                    "not an object",
                ],
            });
            assertEqual(parsed.generation, 4, "the generation");
            assertEqual(parsed.themes.length, 3, "the id-less entry and the non-object are dropped");
            assertEqual(parsed.themes[1]?.source, "package", "a package contribution keeps its source");
            assertEqual(parsed.themes[2]?.source, "user", "an unknown source degrades to user, not dropped");
        },
    },
    {
        name: "THEMES_GET_METHOD is the wire name the Shell routes",
        run: () => {
            // The cross-language authority is themes_bridge.h; `webui-panel-contract` compares the two
            // out of the BUILT bundle. This asserts the constant is exported and non-empty, so a
            // tree-shaken or renamed export fails here first, in a test that names the reason.
            assertEqual(THEMES_GET_METHOD, "themes.get", "the method name editor-core calls");
        },
    },
    {
        name: "controller: the generation gates the reload (an unchanged poll is a counter compare)",
        run: () => {
            const { engine } = engineWith(NULL_PROBE);
            const controller = new ThemeController(engine, new FakeThemesSource());
            const first = controller.applySnapshot({ generation: 0, themes: [] });
            assert(first.reloaded, "the FIRST snapshot always applies, establishing the baseline");
            const again = controller.applySnapshot({ generation: 0, themes: [] });
            assert(!again.reloaded, "an unchanged generation does nothing");
            const moved = controller.applySnapshot({
                generation: 1,
                themes: [{ id: "user.mine", source: "user", text: userThemeText("Mine", "#abcdef") }],
            });
            assert(moved.reloaded, "a moved generation re-registers");
            assertEqual(moved.registration?.accepted, ["user.mine"], "the watched theme loaded");
            assertEqual(controller.generation, 1, "the controller tracks the generation");
        },
    },
    {
        name: "controller: editing the ACTIVE watched theme hot-reloads it onto the live document",
        run: () => {
            const { engine, root } = engineWith(NULL_PROBE);
            const controller = new ThemeController(engine, new FakeThemesSource());
            controller.applySnapshot({
                generation: 1,
                themes: [{ id: "user.mine", source: "user", text: userThemeText("Mine", "#aaaaaa") }],
            });
            engine.apply("user.mine");
            assertEqual(
                root.properties.get(`${TOKEN_VARIABLE_PREFIX}colors-ink`),
                "#aaaaaa",
                "the watched theme is on screen",
            );
            const reload = controller.applySnapshot({
                generation: 2,
                themes: [{ id: "user.mine", source: "user", text: userThemeText("Mine", "#bbbbbb") }],
            });
            assert(reload.reloaded && reload.reapplied, "the edit was re-applied without a restart");
            assertEqual(
                root.properties.get(`${TOKEN_VARIABLE_PREFIX}colors-ink`),
                "#bbbbbb",
                "the edited value reached the live document",
            );
        },
    },
    {
        name: "controller: an edit that BREAKS the active theme falls back — never a broken UI",
        run: () => {
            const { engine, root } = engineWith(NULL_PROBE);
            const controller = new ThemeController(engine, new FakeThemesSource());
            controller.applySnapshot({
                generation: 1,
                themes: [{ id: "user.mine", source: "user", text: userThemeText("Mine", "#aaaaaa") }],
            });
            engine.apply("user.mine");
            const reload = controller.applySnapshot({
                generation: 2,
                themes: [{ id: "user.mine", source: "user", text: "{ broken" }],
            });
            assertEqual(reload.registration?.rejected.length, 1, "the broken edit was rejected");
            assertEqual(engine.activeId, BUILTIN_DARK, "the editor fell back to the default theme");
            assertEqual(
                root.properties.get(`${TOKEN_VARIABLE_PREFIX}colors-ink`),
                "#ededed",
                "and the document is fully themed, not half-applied",
            );
        },
    },
    {
        // REGRESSION (e06b): the fallback used to resolve through a fresh `defaultMediaQueryProbe()`
        // — the GLOBAL scope — instead of the engine's own probe. That is a second source of truth:
        // it made the fallback land on whatever the HOST browser prefers rather than on what this
        // engine was told, so the same snapshot fell back to Dark on one machine and Light on
        // another — green on a Dark-preferring dev box, RED on CI's headless Chromium (which reports
        // `prefers-color-scheme: light`), which is exactly how it was found.
        //
        // This case and the NULL_PROBE one above pin the invariant from OPPOSITE directions, so the
        // pair is host-independent even though neither is alone: on a light-scheme host the sibling
        // (expects Dark) catches a regression, and on a dark-scheme host this one (expects Light)
        // does. Whichever way a future edit reaches for the global scope again, one of the two fails.
        name: "controller: the broken-theme fallback follows the ENGINE's probe, not the global scope",
        run: () => {
            const probe = probeMatching(LIGHT_SCHEME_QUERY);
            const root = new RecordingRoot();
            const engine = new ThemeEngine({ root, probe });
            assert(engine.probe === probe, "the engine exposes the very probe it was given");
            const controller = new ThemeController(engine, new FakeThemesSource());
            controller.applySnapshot({
                generation: 1,
                themes: [{ id: "user.mine", source: "user", text: userThemeText("Mine", "#aaaaaa") }],
            });
            engine.apply("user.mine");
            controller.applySnapshot({
                generation: 2,
                themes: [{ id: "user.mine", source: "user", text: "{ broken" }],
            });
            assertEqual(
                engine.activeId,
                BUILTIN_LIGHT,
                "the fallback asked THIS engine's probe (light), not globalThis.matchMedia",
            );
        },
    },
    {
        name: "controller: a reload that does not touch the active BUILT-IN theme leaves it alone",
        run: () => {
            const { engine } = engineWith(NULL_PROBE);
            const controller = new ThemeController(engine, new FakeThemesSource());
            engine.apply(BUILTIN_LIGHT);
            const reload = controller.applySnapshot({
                generation: 9,
                themes: [{ id: "user.mine", source: "user", text: userThemeText("Mine", "#aaaaaa") }],
            });
            assert(reload.reloaded && !reload.reapplied, "no needless re-apply of a built-in");
            assertEqual(engine.activeId, BUILTIN_LIGHT, "the active theme is untouched");
        },
    },
    {
        // THE e06b CI REGRESSION, pinned. `colors.panel` is a PER-THEME value (#0a0a0a Dark,
        // #ffffff Light) and the first run follows the host's `prefers-color-scheme` — correct
        // product behaviour (06 §4 / C-F22), and a landmine for the live CEF smokes, which scan the
        // composited frame for ONE hardcoded colour. A CI host has no colour-scheme preference at
        // all (no settings portal), so Chromium falls back to `light`, the editor honestly boots
        // `builtin.light`, and a smoke looking for Dark's #0a0a0a finds ZERO texels on a perfectly
        // healthy frame. It was green on a dark-mode dev box and red on both CI legs. The boot-URL
        // pin takes that ambient input out of the test; these cases are what keep it working.
        name: "bootThemeId: an explicit pin BEATS the host's prefers-color-scheme",
        run: () => {
            const known = (id: string): boolean => id === BUILTIN_DARK || id === BUILTIN_LIGHT;
            const lightHost = probeMatching(LIGHT_SCHEME_QUERY);
            assertEqual(defaultThemeId(lightHost), BUILTIN_LIGHT, "the host really does prefer light");
            assertEqual(
                bootThemeId(`?${THEME_PIN_FLAG}=${BUILTIN_DARK}`, lightHost, known),
                BUILTIN_DARK,
                "the pin wins on a light-preferring host — the CEF smoke's whole premise",
            );
            assertEqual(
                bootThemeId(`?${THEME_PIN_FLAG}=${BUILTIN_LIGHT}`, NULL_PROBE, known),
                BUILTIN_LIGHT,
                "and it wins the other way too, so the pin is not a disguised Dark default",
            );
        },
    },
    {
        name: "bootThemeId: no pin leaves the prefers-color-scheme default untouched",
        run: () => {
            const known = (): boolean => true;
            assertEqual(
                bootThemeId("", probeMatching(LIGHT_SCHEME_QUERY), known),
                BUILTIN_LIGHT,
                "a bare boot URL still follows the host (C-F22 is NOT changed by the pin)",
            );
            assertEqual(bootThemeId("", NULL_PROBE, known), BUILTIN_DARK, "undetectable -> Dark");
            assertEqual(
                bootThemeId("?ctx-smoke-palette=1", probeMatching(LIGHT_SCHEME_QUERY), known),
                BUILTIN_LIGHT,
                "an unrelated ?ctx-smoke-* flag is not a theme pin",
            );
        },
    },
    {
        // FAIL-CLOSED, and this is the case that makes it matter: `ThemeEngine.apply` refuses an
        // unknown id, and at FIRST RUN there is no previous theme for that refusal to fall back ON —
        // so honouring a typo'd pin blindly would leave the window unstyled.
        name: "bootThemeId: an UNKNOWN pin falls back to the default rather than unstyling the editor",
        run: () => {
            const known = (id: string): boolean => id === BUILTIN_DARK;
            assertEqual(
                bootThemeId(`?${THEME_PIN_FLAG}=builtin.typo`, NULL_PROBE, known),
                BUILTIN_DARK,
                "an id the registry does not hold is ignored",
            );
            const engine = new ThemeEngine({ root: new RecordingRoot(), probe: NULL_PROBE });
            const id = bootThemeId(`?${THEME_PIN_FLAG}=builtin.typo`, NULL_PROBE, (candidate) =>
                engine.registry.has(candidate),
            );
            assert(engine.apply(id).applied, "boot still lands on a real theme, so nothing is unstyled");
        },
    },
    {
        name: "parsePinnedThemeId: total against every shape a boot URL can arrive in",
        run: () => {
            assertEqual(parsePinnedThemeId(""), "", "no query string");
            assertEqual(parsePinnedThemeId("?a=1&b=2"), "", "no pin among other flags");
            assertEqual(parsePinnedThemeId(`?${THEME_PIN_FLAG}=`), "", "an empty pin is no pin");
            assertEqual(
                parsePinnedThemeId(`?ctx-smoke-arrange=1&${THEME_PIN_FLAG}=${BUILTIN_HC_DARK}`),
                BUILTIN_HC_DARK,
                "read alongside the sibling smoke flags the restore drill sets",
            );
            assertEqual(
                parsePinnedThemeId(`${THEME_PIN_FLAG}=${BUILTIN_DARK}`),
                BUILTIN_DARK,
                "a leading '?' is optional",
            );
        },
    },
];
