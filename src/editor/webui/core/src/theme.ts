// The THEME ENGINE (M9 e06b, design 06 §1-§4 / 04 §2-§5, D11/D12).
//
// e06a shipped themes as DATA — the versioned schema + validator (`@context-engine/editor-tokens`)
// and the four built-in `*.theme.json` files. This module is the RUNTIME that makes that data live:
//
//   1. APPLY      — a validated theme document is flattened into CSS custom properties written at
//                   the document root (`--ctx-<group>-<path>`), plus the DERIVED variables CSS
//                   cannot compute for itself (the Dockview chrome map and the Pulse-of-Work
//                   state→colour map). Components reference ONLY these; raw values live in themes
//                   alone (06 §1).
//   2. LIVE SWITCH — Dark / Light / HC swap with NO restart. Because every surface reads the same
//                   custom properties, a switch is one re-write of the variable set: the docking
//                   chrome, the hydrated panels and the panel iframes all re-token from it, and the
//                   350 ms cross-fade is a CSS transition keyed off `--ctx-motion-duration-theme`
//                   (app.css) rather than a JS animation nobody could inspect.
//   3. REDUCED MOTION — `prefers-reduced-motion: reduce` overrides the motion tokens
//                   UNCONDITIONALLY (06 §1), whatever the theme asked for: every duration collapses
//                   to `0s` and every Pulse-of-Work rhythm becomes `none`, while the state COLOUR
//                   and opacity are RETAINED. That is the owner-picked static fallback
//                   (mockups/TOKENS.md §5) — motion dropped, colour kept. It is NOT an aurora; the
//                   rotating conic halo was rejected outright on 2026-07-19.
//   4. WATCHED USER THEMES — `~/.context/themes/*.theme.json` hot-reload on edit. editor-core is a
//                   PURE WIRE-CLIENT (04 §1 / 08 §1): it has no filesystem and CANNOT read those
//                   files. The Shell reads/watches them and PUBLISHES the raw bytes over the e05c
//                   bridge (`themes.get`, themes_bridge.h); this module parses, SCHEMA-VALIDATES and
//                   registers them. This mirrors the e07c keybindings channel exactly, down to the
//                   GENERATION counter that makes a poll a counter compare.
//   5. PACKAGE CONTRIBUTIONS — a package's manifest `themes: […]` (04 §3, `Contribution::themes`)
//                   arrives through the SAME wire envelope with `source: "package"`, and is
//                   validated by the SAME code path. A theme that fails validation is REJECTED with
//                   a diagnostic and never applied — a bad theme can never produce a broken UI.
//   6. IFRAME DELIVERY — a third-party web panel (04 §5) lives in a sandboxed, cross-origin iframe,
//                   so tokens cannot be handed to it by touching its DOM and MUST NOT be handed to
//                   it by injecting script. They are POSTED as data (`IframeThemeChannel`) and the
//                   panel applies them itself; every switch re-posts, which is the iframe half of
//                   "re-tokened on `editor.ui.theme-changed`".
//
// ⚠ THE `editor.ui` BUS IS A LOCAL STUB. e08 owns the real editor-ui event bus; it has not landed.
// `EditorUiBus` below emits `editor.ui.theme-changed` with the envelope e08 will carry, so when the
// real bus arrives the swap is ONE constructor argument in boot.ts — no consumer changes, no
// envelope change. That is deliberate (the spec calls for it), not an accident of ordering.
//
// WHAT IS NOT HERE, on purpose: the component kit (e06c) and the Settings panel + per-user config
// (e06d). In particular the ACTIVE THEME IS NOT PERSISTED yet — first run follows
// `prefers-color-scheme` (Dark when undetectable, 06 §4 / C-F22) and the choice lives for the
// session. `~/.context/config.json` is e06d's, and the Shell is its single writer.

import { BridgeError, isRecord, type ShellBridge } from "./bridge.js";
import { validateTheme } from "../../tokens/src/index.js";

import darkTheme from "../../tokens/themes/dark.theme.json";
import lightTheme from "../../tokens/themes/light.theme.json";
import hcDarkTheme from "../../tokens/themes/high-contrast-dark.theme.json";
import hcLightTheme from "../../tokens/themes/high-contrast-light.theme.json";

// --------------------------------------------------------------------------- the wire vocabulary
// MUST match themes_bridge.h's kThemesGetMethod. Cross-checked byte-for-byte out of the BUILT bundle
// by `tools/check_webui_assets.py --panel-contract` (ctest `webui-panel-contract`), the same drift
// gate the panel / editor-state / keybindings surfaces ride: a rename on one side would leave
// editor-core calling a method the Shell no longer routes, so watched user themes would silently
// never load and NOTHING would report it.
export const THEMES_GET_METHOD = "themes.get";

/** The `editor.ui` event name a theme switch emits. e08 will carry this SAME name and envelope. */
export const THEME_CHANGED_EVENT = "editor.ui.theme-changed";

/** The CSS custom-property prefix every token variable carries (mockups/TOKENS.md: `--ctx-*`). */
export const TOKEN_VARIABLE_PREFIX = "--ctx-";

/** The media query whose `reduce` verdict overrides the motion tokens unconditionally (06 §1). */
export const REDUCED_MOTION_QUERY = "(prefers-reduced-motion: reduce)";

/** The media query that picks the FIRST-RUN default appearance (06 §4 / C-F22). */
export const LIGHT_SCHEME_QUERY = "(prefers-color-scheme: light)";

// ------------------------------------------------------------------------------- theme documents
//
// A theme is DATA. Rather than hand-maintaining a TypeScript mirror of the schema — a second source
// of truth free to drift from `tokens/src/schema.ts` — a validated document is treated as the plain
// JSON object it is, and every read goes through the total helpers below. `validateTheme` has
// already proven the shape by the time anything here reads it.

/** A theme document that has PASSED `validateTheme`. Plain JSON, deliberately not a mirrored type. */
export type ThemeDocument = Readonly<Record<string, unknown>>;

/** Where a theme came from. Drives listing/precedence and appears in the diagnostics. */
export type ThemeSource = "builtin" | "user" | "package";

/** The seven token groups (06 §1). Everything outside them is envelope metadata, not a token. */
const TOKEN_GROUPS = [
    "colors",
    "typography",
    "shape",
    "elevation",
    "motion",
    "iconography",
    "viewport",
] as const;

/** The five Pulse-of-Work play states (mockups/TOKENS.md §5). Order is the doc's, not alphabetical. */
export const FLOURISH_STATES = ["idle", "running", "compiling", "error", "paused"] as const;

/** One registered, VALIDATED theme. */
export interface ThemeEntry {
    /** Stable id: `builtin.dark`, or `<source>.<file stem>` for a contributed one. */
    readonly id: string;
    readonly source: ThemeSource;
    /** The theme's own display name (`name` in the document). */
    readonly name: string;
    readonly appearance: "dark" | "light";
    readonly highContrast: boolean;
    readonly document: ThemeDocument;
}

/** A theme that was REFUSED, and why. Surfaced, never silently dropped (a bad theme must be visible). */
export interface ThemeRejection {
    readonly id: string;
    readonly source: ThemeSource;
    readonly diagnostic: string;
}

/** The outcome of registering a batch of contributed themes. */
export interface ThemeRegistrationResult {
    readonly accepted: readonly string[];
    readonly rejected: readonly ThemeRejection[];
}

function readString(node: unknown, key: string): string {
    if (!isRecord(node)) {
        return "";
    }
    const value = node[key];
    return typeof value === "string" ? value : "";
}

function readRecord(node: unknown, key: string): Record<string, unknown> | undefined {
    if (!isRecord(node)) {
        return undefined;
    }
    const value = node[key];
    return isRecord(value) ? value : undefined;
}

/** Walk a dotted path through a validated document, returning `undefined` at the first miss. */
function readPath(document: ThemeDocument, ...path: readonly string[]): unknown {
    let node: unknown = document;
    for (const key of path) {
        if (!isRecord(node)) {
            return undefined;
        }
        node = node[key];
    }
    return node;
}

// ------------------------------------------------------------------------------- variable naming
//
// The token variable set is DERIVED from the document rather than enumerated by hand. That is the
// whole point: a token added to the schema reaches CSS with no edit here, and no variable can go
// missing because someone forgot a line. The mapping is mechanical and total.

/** `focusRing` -> `focus-ring`, `fontUi` -> `font-ui`, `2xs` / `panel2` unchanged. */
export function kebabCase(key: string): string {
    return key.replace(/([a-z0-9])([A-Z])/g, "$1-$2").toLowerCase();
}

/** Render a JSON leaf as a CSS value. Booleans become `true`/`false` so a theme can gate a rule. */
function leafToCssValue(value: string | number | boolean): string {
    return typeof value === "string" ? value : String(value);
}

function flattenInto(
    node: unknown,
    prefix: string,
    out: Record<string, string>,
): void {
    if (typeof node === "string" || typeof node === "number" || typeof node === "boolean") {
        out[prefix] = leafToCssValue(node);
        return;
    }
    if (!isRecord(node)) {
        return; // arrays / null never appear in a validated theme; ignoring them keeps this total
    }
    for (const key of Object.keys(node)) {
        flattenInto(node[key], `${prefix}-${kebabCase(key)}`, out);
    }
}

/**
 * Parse a CSS duration (`350ms`, `2.6s`, `0s`) into milliseconds.
 *
 * TOTAL: an unparseable or negative value yields 0 rather than NaN, because this number ends up in a
 * CSS variable and in the apply report — a NaN there would surface as a silently broken transition
 * instead of an honest "no fade".
 */
export function parseDurationMs(value: string): number {
    const match = /^\s*(-?\d+(?:\.\d+)?)\s*(ms|s)\s*$/.exec(value);
    if (match === null) {
        return 0;
    }
    const magnitude = Number(match[1]);
    if (!Number.isFinite(magnitude) || magnitude < 0) {
        return 0;
    }
    return match[2] === "s" ? magnitude * 1000 : magnitude;
}

/** Render a millisecond count back into a CSS duration. Used for the reduced-motion collapse. */
function msToCss(ms: number): string {
    return `${ms}ms`;
}

/**
 * The DOCKVIEW CHROME MAP (D2 synergy, 06 §2 "Dockview chrome is skinned via its CSS variables from
 * the same tokens").
 *
 * Dockview reads its own `--dv-*` custom properties, so skinning it is a pure re-point at the
 * editor's tokens — no Dockview theme file, no fork, and the D2 fallback cost stays this one table.
 *
 * ⚠ THE FIVE BACKGROUND VARIABLES ALL RESOLVE TO `colors.panel`, DELIBERATELY. Before this task
 * app.css pointed exactly these five at the single placeholder `--editor-bg`, and the live
 * `editor-cef-smoke-shell` / `-restore` legs scan the composited frame for that ONE colour and
 * require it over a tenth of the surface. Mapping them to one token keeps the PAINTED REGION
 * byte-for-byte what it was and moves only the colour, so the smokes' coverage floor is unchanged
 * (their `kAppBackground*` constants move to the Dark theme's `colors.panel` in lockstep — see
 * src/editor/shell/cef/src/cef_shell_smoke.cpp). Giving the tab strip its own `panel2` step is a
 * COMPONENT-KIT decision and belongs to e06c, which can make it with the smokes in the same diff.
 */
const DOCKVIEW_CHROME: readonly (readonly [string, readonly string[]])[] = [
    ["--dv-background-color", ["colors", "panel"]],
    ["--dv-group-view-background-color", ["colors", "panel"]],
    ["--dv-tabs-and-actions-container-background-color", ["colors", "panel"]],
    ["--dv-activegroup-visiblepanel-tab-background-color", ["colors", "panel"]],
    ["--dv-inactivegroup-visiblepanel-tab-background-color", ["colors", "panel"]],
    ["--dv-activegroup-visiblepanel-tab-color", ["colors", "ink"]],
    ["--dv-activegroup-hiddenpanel-tab-color", ["colors", "muted"]],
    ["--dv-inactivegroup-visiblepanel-tab-color", ["colors", "muted"]],
    ["--dv-inactivegroup-hiddenpanel-tab-color", ["colors", "muted2"]],
    ["--dv-tab-divider-color", ["colors", "line"]],
    ["--dv-separator-border", ["colors", "line"]],
    ["--dv-paneview-header-border-color", ["colors", "line"]],
    ["--dv-paneview-active-outline-color", ["colors", "accent"]],
];

/**
 * Flatten a validated theme into its CSS custom properties.
 *
 * PURE — no DOM, no globals, no reduced-motion policy (that is `applyMotionPreference`'s job, kept
 * separate so the two are independently testable and the override is provably unconditional).
 *
 * Three layers, in this order:
 *   1. every leaf of the seven token groups, mechanically named `--ctx-<group>-<path…>`;
 *   2. the Dockview chrome map above, RESOLVED to concrete values (not `var()` indirection, so a
 *      consumer that reads the computed value gets a colour rather than a reference);
 *   3. the Pulse-of-Work derived set — `--ctx-flourish-<state>-{color,opacity,duration,animation}`.
 *      The document names a HUE by its reserved semantic (`good` / `warn` / …), which is what keeps
 *      the flourish at ZERO new colour tokens; CSS cannot dereference that indirection itself, so
 *      the resolution happens here.
 */
export function themeCssVariables(theme: ThemeDocument): Record<string, string> {
    const out: Record<string, string> = {};
    for (const group of TOKEN_GROUPS) {
        flattenInto(readPath(theme, group), `${TOKEN_VARIABLE_PREFIX}${group}`, out);
    }

    for (const [variable, path] of DOCKVIEW_CHROME) {
        const value = readPath(theme, ...path);
        if (typeof value === "string") {
            out[variable] = value;
        }
    }

    const semantic = readRecord(readPath(theme, "colors"), "semantic");
    for (const state of FLOURISH_STATES) {
        const spec = readPath(theme, "motion", "flourish", "states", state);
        if (!isRecord(spec)) {
            continue;
        }
        const hue = readString(spec, "hue");
        const colour = semantic === undefined ? undefined : semantic[hue];
        out[`${TOKEN_VARIABLE_PREFIX}flourish-${state}-color`] =
            typeof colour === "string" ? colour : "transparent";
        const opacity = spec["opacity"];
        out[`${TOKEN_VARIABLE_PREFIX}flourish-${state}-opacity`] =
            typeof opacity === "number" ? String(opacity) : "1";
        const duration = readString(spec, "duration");
        out[`${TOKEN_VARIABLE_PREFIX}flourish-${state}-duration`] = duration === "" ? "0s" : duration;
        // The keyframes name app.css defines for each rhythm. `none` is a legal `animation-name`, so
        // the paused state (and, below, every state under reduced motion) needs no separate rule.
        const rhythm = readString(spec, "rhythm");
        out[`${TOKEN_VARIABLE_PREFIX}flourish-${state}-animation`] =
            rhythm === "breathe" ? "ctx-state-breathe" : rhythm === "pulse" ? "ctx-state-pulse" : "none";
    }

    // The cross-fade duration, named once so app.css transitions and the apply report agree on it.
    out[`${TOKEN_VARIABLE_PREFIX}theme-fade-duration`] = readString(
        readRecord(readRecord(theme, "motion") ?? {}, "duration") ?? {},
        "theme",
    );
    return out;
}

/**
 * Apply the reduced-motion policy to an already-flattened variable set, IN PLACE-free (returns a new
 * map so the un-reduced set stays inspectable).
 *
 * UNCONDITIONAL when `reduced` (06 §1): every `--ctx-motion-duration-*` and every flourish duration
 * collapses to `0ms`, every flourish `animation` becomes `none`, spring pills are disabled, and the
 * theme cross-fade is instant. The flourish COLOUR and OPACITY are untouched — that is the
 * owner-picked static fallback (mockups/TOKENS.md §5: "drops all animation to a static state, colour
 * retained"), and it is why this cannot be implemented by simply switching the flourish off.
 */
export function applyMotionPreference(
    variables: Readonly<Record<string, string>>,
    reduced: boolean,
): Record<string, string> {
    const out: Record<string, string> = { ...variables };
    if (!reduced) {
        return out;
    }
    for (const key of Object.keys(out)) {
        if (key.startsWith(`${TOKEN_VARIABLE_PREFIX}motion-duration-`)) {
            out[key] = msToCss(0);
        } else if (key.endsWith("-duration") && key.startsWith(`${TOKEN_VARIABLE_PREFIX}flourish-`)) {
            out[key] = msToCss(0);
        } else if (key.endsWith("-animation") && key.startsWith(`${TOKEN_VARIABLE_PREFIX}flourish-`)) {
            out[key] = "none";
        }
    }
    out[`${TOKEN_VARIABLE_PREFIX}motion-enable-spring-pills`] = "false";
    out[`${TOKEN_VARIABLE_PREFIX}theme-fade-duration`] = msToCss(0);
    // A single switch app.css (and any kit component) can branch on without re-deriving the policy.
    out[`${TOKEN_VARIABLE_PREFIX}motion-reduced`] = "1";
    return out;
}

// ------------------------------------------------------------------------------- the editor.ui bus (e08 STUB)

/** The `editor.ui.theme-changed` payload. e08's real bus carries this SAME shape (see the header). */
export interface ThemeChangedPayload {
    readonly themeId: string;
    readonly name: string;
    readonly appearance: "dark" | "light";
    readonly highContrast: boolean;
    readonly reducedMotion: boolean;
    /** The full applied variable set — what a panel iframe needs to re-token itself. */
    readonly variables: Readonly<Record<string, string>>;
}

/** An `editor.ui` event envelope: a name plus its payload. Deliberately e08's shape, not ours. */
export interface EditorUiEvent {
    readonly type: string;
    readonly payload: ThemeChangedPayload;
}

/** Detaches a subscription. Mirrors Dockview's disposable convention so callers see one idiom. */
export interface EditorUiSubscription {
    dispose(): void;
}

/**
 * The LOCAL STUB of e08's `editor.ui` bus.
 *
 * Exists so the theme engine can publish `editor.ui.theme-changed` today with the envelope e08 will
 * carry tomorrow. When the real bus lands, `ThemeEngine` takes it as its `bus` argument and NOTHING
 * else changes — that zero-engine-change swap is the reason this is a bus at all rather than a
 * callback list inlined into the engine.
 *
 * Total by design: a throwing subscriber is isolated so one bad panel cannot break the theme switch
 * for every other subscriber.
 */
export class EditorUiBus {
    readonly #listeners = new Map<string, Set<(event: EditorUiEvent) => void>>();

    subscribe(type: string, listener: (event: EditorUiEvent) => void): EditorUiSubscription {
        const existing = this.#listeners.get(type);
        const set = existing ?? new Set<(event: EditorUiEvent) => void>();
        if (existing === undefined) {
            this.#listeners.set(type, set);
        }
        set.add(listener);
        return {
            dispose: (): void => {
                set.delete(listener);
            },
        };
    }

    emit(event: EditorUiEvent): void {
        const set = this.#listeners.get(event.type);
        if (set === undefined) {
            return;
        }
        for (const listener of [...set]) {
            try {
                listener(event);
            } catch {
                // A subscriber's failure is its own; the switch must still reach everyone else.
            }
        }
    }
}

// ------------------------------------------------------------------------------- iframe delivery (04 §5)

/** The one method this module needs from an iframe's `contentWindow`. Keeps the tests DOM-free. */
export interface IframeMessageTarget {
    postMessage(message: unknown, targetOrigin: string): void;
}

/**
 * CSP-SAFE token delivery into panel iframes.
 *
 * A third-party web panel runs in a SANDBOXED, cross-origin iframe (04 §5), which forces the design:
 *
 *   * we CANNOT reach into its document to set variables — a sandbox without `allow-same-origin`
 *     gives it an opaque origin, so `contentDocument` is null;
 *   * we MUST NOT hand it script or a stylesheet string to inject — the trusted zone's whole CSP
 *     posture (`script-src 'self'`, no `unsafe-inline`) exists so authored/third-party content can
 *     never execute or style its way out, and an injection helper would be exactly the hole it is
 *     there to close.
 *
 * So tokens travel as DATA: a structured-clone `postMessage` carrying the same
 * `editor.ui.theme-changed` envelope the bus emits, which the panel applies to its OWN root. Every
 * registered frame is re-posted on every switch — the iframe half of "re-tokened on theme change" —
 * and a frame registered mid-session is posted the CURRENT theme immediately, so it never renders
 * one frame unthemed.
 *
 * `targetOrigin` is `"*"` ON PURPOSE: a sandboxed frame's origin is opaque, so no specific origin
 * could ever match and a stricter value would silently deliver nothing. That is safe here because
 * the payload is design tokens — public presentation data with no capability and no secret.
 */
export const IFRAME_TARGET_ORIGIN = "*";

export class IframeThemeChannel {
    readonly #targets = new Set<IframeMessageTarget>();
    #last: EditorUiEvent | undefined;

    /** Register a panel frame. It is posted the current theme immediately when one has been applied. */
    register(target: IframeMessageTarget): void {
        this.#targets.add(target);
        if (this.#last !== undefined) {
            this.#post(target, this.#last);
        }
    }

    unregister(target: IframeMessageTarget): void {
        this.#targets.delete(target);
    }

    get size(): number {
        return this.#targets.size;
    }

    /** Broadcast a theme change to every registered frame, and remember it for late registrations. */
    broadcast(event: EditorUiEvent): void {
        this.#last = event;
        for (const target of [...this.#targets]) {
            this.#post(target, event);
        }
    }

    #post(target: IframeMessageTarget, event: EditorUiEvent): void {
        try {
            target.postMessage(event, IFRAME_TARGET_ORIGIN);
        } catch {
            // A detached / navigated frame throws; that is its problem, not the theme switch's.
        }
    }
}

// ------------------------------------------------------------------------------- the registry

/** The built-in theme ids. Stable — a user config (e06d) will name one of these. */
export const BUILTIN_DARK = "builtin.dark";
export const BUILTIN_LIGHT = "builtin.light";
export const BUILTIN_HC_DARK = "builtin.high-contrast-dark";
export const BUILTIN_HC_LIGHT = "builtin.high-contrast-light";

const BUILTIN_DOCUMENTS: readonly (readonly [string, unknown])[] = [
    [BUILTIN_DARK, darkTheme],
    [BUILTIN_LIGHT, lightTheme],
    [BUILTIN_HC_DARK, hcDarkTheme],
    [BUILTIN_HC_LIGHT, hcLightTheme],
];

function entryFrom(id: string, source: ThemeSource, document: ThemeDocument): ThemeEntry {
    const appearance = readString(document, "appearance") === "light" ? "light" : "dark";
    return {
        id,
        source,
        name: readString(document, "name"),
        appearance,
        highContrast: document["highContrast"] === true,
        document,
    };
}

/** A contributed theme as it arrives on the wire: an id and the raw, UNVALIDATED file bytes. */
export interface ThemeContribution {
    readonly id: string;
    readonly source: ThemeSource;
    readonly text: string;
}

/**
 * The set of themes this editor can switch to.
 *
 * Built-ins are registered at construction and are IMMOVABLE: a contributed theme that claims a
 * built-in id is rejected rather than shadowing it, so a stray file in `~/.context/themes/` can
 * never replace Dark with something broken. Contributed themes are replaced WHOLESALE per source on
 * every reload, which is what makes a deleted file actually disappear.
 */
export class ThemeRegistry {
    readonly #entries = new Map<string, ThemeEntry>();

    constructor() {
        for (const [id, document] of BUILTIN_DOCUMENTS) {
            // The built-ins ship inside the bundle and are covered by the e06a T1 suite, so they are
            // registered without re-validating: a failure here could not be acted on at runtime
            // anyway, and the drift gate that would catch it lives in the tests, not in the product.
            this.#entries.set(id, entryFrom(id, "builtin", document as ThemeDocument));
        }
    }

    list(): readonly ThemeEntry[] {
        return [...this.#entries.values()];
    }

    get(id: string): ThemeEntry | undefined {
        return this.#entries.get(id);
    }

    has(id: string): boolean {
        return this.#entries.has(id);
    }

    /** The ids currently contributed by `source` (built-ins are never in this set). */
    idsFrom(source: ThemeSource): readonly string[] {
        return this.list()
            .filter((entry) => entry.source === source)
            .map((entry) => entry.id);
    }

    /**
     * Replace every theme from the named sources with `contributions`, validating each.
     *
     * FAIL-CLOSED PER THEME, not per batch: a malformed file is rejected with a diagnostic and the
     * REST of the batch still loads. That is the difference between one bad hand-edit costing you
     * that theme and it costing you every theme you have.
     */
    replaceContributed(
        sources: readonly ThemeSource[],
        contributions: readonly ThemeContribution[],
    ): ThemeRegistrationResult {
        for (const [id, entry] of [...this.#entries]) {
            if (entry.source !== "builtin" && sources.includes(entry.source)) {
                this.#entries.delete(id);
            }
        }
        const accepted: string[] = [];
        const rejected: ThemeRejection[] = [];
        for (const contribution of contributions) {
            const outcome = this.#register(contribution);
            if (outcome === "") {
                accepted.push(contribution.id);
            } else {
                rejected.push({
                    id: contribution.id,
                    source: contribution.source,
                    diagnostic: outcome,
                });
            }
        }
        return { accepted, rejected };
    }

    /** Register one contribution. Returns `""` on success, or the rejection diagnostic. */
    #register(contribution: ThemeContribution): string {
        if (contribution.source === "builtin") {
            return "a contribution may not claim the builtin source";
        }
        if (contribution.id === "" ) {
            return "a contributed theme must carry a non-empty id";
        }
        const existing = this.#entries.get(contribution.id);
        if (existing !== undefined) {
            return existing.source === "builtin"
                ? `"${contribution.id}" is a built-in theme id and cannot be replaced`
                : `"${contribution.id}" is already contributed by ${existing.source}`;
        }
        let parsed: unknown;
        try {
            parsed = JSON.parse(contribution.text);
        } catch (error) {
            return `not valid JSON: ${error instanceof Error ? error.message : String(error)}`;
        }
        const result = validateTheme(parsed);
        if (!result.valid) {
            // Every error, not just the first — a hand-authored theme is usually wrong in more than
            // one place, and one-at-a-time diagnostics turn that into a guessing loop.
            return `does not match the theme schema: ${result.errors.join("; ")}`;
        }
        this.#entries.set(
            contribution.id,
            entryFrom(contribution.id, contribution.source, parsed as ThemeDocument),
        );
        return "";
    }
}

// ------------------------------------------------------------------------------- the engine

/** The minimum an element must offer to be themed. `HTMLElement` satisfies it structurally. */
export interface ThemeRoot {
    readonly style: { setProperty(property: string, value: string): void };
    setAttribute(qualifiedName: string, value: string): void;
}

/** A `MediaQueryList`-shaped probe result. Only `matches` is read. */
export interface MediaQueryLike {
    readonly matches: boolean;
}

/** Evaluates a media query, or returns `null` where the environment cannot (a bare harness). */
export type MediaQueryProbe = (query: string) => MediaQueryLike | null;

/** The default probe: `globalThis.matchMedia`, or a null probe when it does not exist. */
export function defaultMediaQueryProbe(scope: unknown = globalThis): MediaQueryProbe {
    if (!isRecord(scope) || typeof scope["matchMedia"] !== "function") {
        return (): null => null;
    }
    const matchMedia = scope["matchMedia"] as (query: string) => MediaQueryLike;
    return (query: string): MediaQueryLike | null => {
        try {
            return matchMedia.call(scope, query);
        } catch {
            return null;
        }
    };
}

/**
 * Does the user want reduced motion?
 *
 * An UNDETECTABLE preference reads as `false` — the honest default (we were not told to reduce), and
 * the one that keeps a harness with no `matchMedia` from silently claiming a preference the user
 * never expressed.
 */
export function prefersReducedMotion(probe: MediaQueryProbe): boolean {
    return probe(REDUCED_MOTION_QUERY)?.matches === true;
}

/** The FIRST-RUN theme: follow `prefers-color-scheme`, Dark when undetectable (06 §4 / C-F22). */
export function defaultThemeId(probe: MediaQueryProbe): string {
    return probe(LIGHT_SCHEME_QUERY)?.matches === true ? BUILTIN_LIGHT : BUILTIN_DARK;
}

/** What an `apply` did — returned so a caller (and a test) can assert on it without reading CSS. */
export interface ThemeApplyReport {
    readonly applied: boolean;
    readonly themeId: string;
    readonly appearance: "dark" | "light";
    readonly highContrast: boolean;
    readonly reducedMotion: boolean;
    /** The cross-fade actually used, in ms: 350 from the built-ins, 0 under reduced motion. */
    readonly fadeDurationMs: number;
    readonly variableCount: number;
    /** `""` on success; why the apply was refused otherwise (the previous theme stands). */
    readonly diagnostic: string;
}

/** Runs `callback` after `delayMs`. Injected so the cross-fade window is testable without waiting. */
export type SchedulerFn = (callback: () => void, delayMs: number) => void;

/** The default scheduler: `setTimeout`, degrading to "immediately" where there is none. */
export function defaultScheduler(callback: () => void, delayMs: number): void {
    if (typeof setTimeout === "function") {
        setTimeout(callback, delayMs);
        return;
    }
    callback();
}

/**
 * The root attribute that ARMS the cross-fade, set for exactly the fade's duration.
 *
 * The transition CANNOT simply be declared on the themed surfaces: `--ctx-theme-fade-duration` stays
 * at 350 ms after a switch finishes, so a permanently-declared `transition: background-color
 * var(--ctx-theme-fade-duration)` would also slow every hover and selection change for the rest of
 * the session. Arming it only while a switch is in flight keeps the cross-fade a property of the
 * SWITCH rather than of the editor.
 */
export const THEME_TRANSITION_ATTRIBUTE = "data-theme-transition";

export interface ThemeEngineOptions {
    readonly root: ThemeRoot;
    readonly registry?: ThemeRegistry;
    readonly bus?: EditorUiBus;
    readonly iframes?: IframeThemeChannel;
    readonly probe?: MediaQueryProbe;
    readonly schedule?: SchedulerFn;
}

/**
 * The theme engine: owns the active theme, writes the variables, and announces the change.
 *
 * NEVER THROWS. An unknown theme id, or a registry that somehow lost the entry, is a REFUSAL that
 * leaves the previous theme standing and reports a diagnostic — the same fail-closed contract the
 * keymap's user override follows, and for the same reason: whatever else goes wrong, the editor must
 * never be left unstyled.
 */
export class ThemeEngine {
    readonly #root: ThemeRoot;
    readonly #registry: ThemeRegistry;
    readonly #bus: EditorUiBus;
    readonly #iframes: IframeThemeChannel;
    readonly #schedule: SchedulerFn;
    #probe: MediaQueryProbe;
    #activeId = "";
    #lastReport: ThemeApplyReport | undefined;

    constructor(options: ThemeEngineOptions) {
        this.#root = options.root;
        this.#registry = options.registry ?? new ThemeRegistry();
        this.#bus = options.bus ?? new EditorUiBus();
        this.#iframes = options.iframes ?? new IframeThemeChannel();
        this.#probe = options.probe ?? defaultMediaQueryProbe();
        this.#schedule = options.schedule ?? defaultScheduler;
    }

    get registry(): ThemeRegistry {
        return this.#registry;
    }

    get bus(): EditorUiBus {
        return this.#bus;
    }

    get iframes(): IframeThemeChannel {
        return this.#iframes;
    }

    /** The active theme id, or `""` before the first successful apply. */
    get activeId(): string {
        return this.#activeId;
    }

    get lastReport(): ThemeApplyReport | undefined {
        return this.#lastReport;
    }

    /** Swap the media-query probe (the live `prefers-reduced-motion` listener re-applies through it). */
    setProbe(probe: MediaQueryProbe): void {
        this.#probe = probe;
    }

    /**
     * Apply a theme by id. THE live-switch entry point — Dark -> Light -> HC is exactly this call,
     * with no restart and no reload, because every surface reads the variables it rewrites.
     */
    apply(themeId: string): ThemeApplyReport {
        const entry = this.#registry.get(themeId);
        if (entry === undefined) {
            const report: ThemeApplyReport = {
                applied: false,
                themeId: this.#activeId,
                appearance: this.#lastReport?.appearance ?? "dark",
                highContrast: this.#lastReport?.highContrast ?? false,
                reducedMotion: this.#lastReport?.reducedMotion ?? false,
                fadeDurationMs: 0,
                variableCount: 0,
                diagnostic: `no such theme: "${themeId}"`,
            };
            this.#lastReport = report;
            return report;
        }
        const reduced = prefersReducedMotion(this.#probe);
        const variables = applyMotionPreference(themeCssVariables(entry.document), reduced);
        // NO CROSS-FADE ON THE FIRST APPLY. There is nothing to fade FROM at boot, so fading would
        // only mean the editor's first 350 ms are spent sliding out of the pre-theme placeholder
        // colours — visible as a flash, and it would make the live CEF smoke's per-pixel background
        // assertion race a transition instead of observing a settled frame. The 350 ms cross-fade is
        // a property of a SWITCH.
        const fadeDurationMs =
            this.#activeId === ""
                ? 0
                : parseDurationMs(variables[`${TOKEN_VARIABLE_PREFIX}theme-fade-duration`] ?? "");
        variables[`${TOKEN_VARIABLE_PREFIX}theme-fade-duration`] = msToCss(fadeDurationMs);
        for (const key of Object.keys(variables)) {
            const value = variables[key];
            if (value !== undefined) {
                this.#root.style.setProperty(key, value);
            }
        }
        // Attributes, not just variables: a stylesheet needs to branch on appearance / contrast /
        // reduced-motion, and a `--var` cannot drive a selector. They are also what makes the applied
        // state legible from DevTools and from the `--dump-dom` local repro.
        this.#root.setAttribute("data-theme", entry.id);
        this.#root.setAttribute("data-appearance", entry.appearance);
        this.#root.setAttribute("data-high-contrast", entry.highContrast ? "true" : "false");
        this.#root.setAttribute("data-reduced-motion", reduced ? "true" : "false");
        // Arm the cross-fade for exactly its own duration, then disarm (see THEME_TRANSITION_ATTRIBUTE).
        if (fadeDurationMs > 0) {
            this.#root.setAttribute(THEME_TRANSITION_ATTRIBUTE, "true");
            this.#schedule(() => {
                this.#root.setAttribute(THEME_TRANSITION_ATTRIBUTE, "false");
            }, fadeDurationMs);
        } else {
            this.#root.setAttribute(THEME_TRANSITION_ATTRIBUTE, "false");
        }

        this.#activeId = entry.id;
        const report: ThemeApplyReport = {
            applied: true,
            themeId: entry.id,
            appearance: entry.appearance,
            highContrast: entry.highContrast,
            reducedMotion: reduced,
            fadeDurationMs,
            variableCount: Object.keys(variables).length,
            diagnostic: "",
        };
        this.#lastReport = report;

        const event: EditorUiEvent = {
            type: THEME_CHANGED_EVENT,
            payload: {
                themeId: entry.id,
                name: entry.name,
                appearance: entry.appearance,
                highContrast: entry.highContrast,
                reducedMotion: reduced,
                variables,
            },
        };
        // The iframes BEFORE the bus: a panel frame is the slowest consumer (a message hop), and a
        // bus subscriber that re-enters `apply` would otherwise reorder the two.
        this.#iframes.broadcast(event);
        this.#bus.emit(event);
        return report;
    }

    /** Re-apply the active theme — how a live `prefers-reduced-motion` change takes effect. */
    reapply(): ThemeApplyReport | undefined {
        return this.#activeId === "" ? undefined : this.apply(this.#activeId);
    }

    /**
     * The `view.theme.toggle` command's behaviour: flip the active theme's APPEARANCE, preserving
     * whether high contrast is on.
     *
     * Preserving contrast is the accessible choice — a user who needs HC needs it in both
     * appearances, and a toggle that silently dropped it would be a regression they cannot see
     * coming. Falls back to the first theme of the wanted appearance so a toggle from a USER theme
     * still does something sensible.
     */
    toggleAppearance(): ThemeApplyReport {
        const active = this.#registry.get(this.#activeId);
        const wantLight = active?.appearance !== "light";
        const wantHc = active?.highContrast === true;
        const match = this.#registry
            .list()
            .find(
                (entry) =>
                    entry.source === "builtin" &&
                    entry.appearance === (wantLight ? "light" : "dark") &&
                    entry.highContrast === wantHc,
            );
        return this.apply(match?.id ?? (wantLight ? BUILTIN_LIGHT : BUILTIN_DARK));
    }
}

// ------------------------------------------------------------------------------- the Shell feed
//
// The Shell owns the watch of `~/.context/themes/*.theme.json` and publishes a GENERATION counter
// (themes_bridge.h). editor-core re-registers only when the generation moves, so an idle poll costs
// one integer compare — the identical model e07c established for keybindings.

/** One theme as the Shell serves it: an id, its source, and the raw file bytes (unvalidated). */
export interface ThemesSnapshotEntry {
    readonly id: string;
    readonly source: ThemeSource;
    readonly text: string;
}

/** The `themes.get` result. `generation` moves whenever the Shell observes a content change. */
export interface ThemesSnapshot {
    readonly generation: number;
    readonly themes: readonly ThemesSnapshotEntry[];
}

/** Parse a `themes.get` result, total against a malformed or partial envelope. */
export function parseThemesSnapshot(value: unknown): ThemesSnapshot {
    if (!isRecord(value)) {
        return { generation: 0, themes: [] };
    }
    const generation = typeof value["generation"] === "number" ? value["generation"] : 0;
    const raw = value["themes"];
    if (!Array.isArray(raw)) {
        return { generation, themes: [] };
    }
    const themes: ThemesSnapshotEntry[] = [];
    for (const item of raw) {
        if (!isRecord(item)) {
            continue;
        }
        const id = typeof item["id"] === "string" ? item["id"] : "";
        const text = typeof item["text"] === "string" ? item["text"] : "";
        // An unrecognised source is coerced to "user" rather than dropped: the Shell is trusted, and
        // silently discarding a theme because a future source token was added would be worse than
        // treating it as the ordinary contributed case.
        const rawSource = item["source"];
        const source: ThemeSource = rawSource === "package" ? "package" : "user";
        if (id !== "") {
            themes.push({ id, source, text });
        }
    }
    return { generation, themes };
}

/** A source of theme snapshots — the seam the controller depends on, so a test injects a fake. */
export interface ThemesSource {
    get(): Promise<ThemesSnapshot>;
}

/**
 * The thin typed client over the `themes.get` bridge method.
 *
 * Holds no state: the files live in the Shell (D18), so a cache here would be a second source of
 * truth free to disagree. A bridge refusal reads as an EMPTY snapshot rather than throwing — a Shell
 * that does not serve the method (an older build, or a smoke's minimal router) must leave the
 * built-ins standing, not break editor-core's boot.
 */
export class ThemesClient implements ThemesSource {
    readonly #bridge: ShellBridge;

    constructor(bridge: ShellBridge) {
        this.#bridge = bridge;
    }

    async get(): Promise<ThemesSnapshot> {
        try {
            return parseThemesSnapshot(await this.#bridge.call(THEMES_GET_METHOD));
        } catch (error) {
            if (error instanceof BridgeError) {
                return { generation: 0, themes: [] };
            }
            throw error;
        }
    }
}

/** What a controller refresh did. */
export interface ThemeReloadResult {
    /** True when the generation moved and the contributed set was re-registered (a hot reload). */
    readonly reloaded: boolean;
    readonly registration: ThemeRegistrationResult | undefined;
    /** True when the ACTIVE theme was itself reloaded and re-applied — the live-editing feedback. */
    readonly reapplied: boolean;
    readonly generation: number;
}

/** The wire sources a snapshot may carry — everything the controller replaces on each reload. */
const CONTRIBUTED_SOURCES: readonly ThemeSource[] = ["user", "package"];

/**
 * Ties the registry + engine to the Shell feed: re-register on a generation move, and re-apply when
 * the theme currently on screen is one of the files that changed.
 *
 * THAT RE-APPLY IS THE HOT-RELOAD FEATURE, not a side effect: editing `~/.context/themes/mine.theme.json`
 * repaints the editor within one poll, which is the "live-editing themes with instant feedback"
 * design 06 §4 asks for. If the edited file is now INVALID it is rejected, the active theme is gone
 * from the registry, and the engine falls back to the first-run default rather than leaving a
 * half-applied or unstyled window — a bad theme is never a broken UI.
 */
export class ThemeController {
    readonly #engine: ThemeEngine;
    readonly #source: ThemesSource;
    // -1 is never a real generation, so the FIRST snapshot always applies — even an empty one, which
    // establishes the "no user themes" baseline rather than assuming 0 means "nothing yet".
    #lastGeneration = -1;

    constructor(engine: ThemeEngine, source: ThemesSource) {
        this.#engine = engine;
        this.#source = source;
    }

    get generation(): number {
        return this.#lastGeneration;
    }

    /** Apply a snapshot, re-registering only when its generation moved. Synchronous and total. */
    applySnapshot(snapshot: ThemesSnapshot): ThemeReloadResult {
        if (snapshot.generation === this.#lastGeneration) {
            return {
                reloaded: false,
                registration: undefined,
                reapplied: false,
                generation: this.#lastGeneration,
            };
        }
        const activeBefore = this.#engine.activeId;
        const activeWasContributed =
            this.#engine.registry.get(activeBefore)?.source !== "builtin" && activeBefore !== "";
        const registration = this.#engine.registry.replaceContributed(
            CONTRIBUTED_SOURCES,
            snapshot.themes,
        );
        this.#lastGeneration = snapshot.generation;

        let reapplied = false;
        if (activeWasContributed) {
            // The active theme's bytes may have just changed (or vanished). Re-apply it when it is
            // still there; otherwise fall back, so the window is never left showing a theme that no
            // longer exists.
            reapplied = true;
            if (this.#engine.registry.has(activeBefore)) {
                this.#engine.apply(activeBefore);
            } else {
                this.#engine.apply(defaultThemeId(defaultMediaQueryProbe()));
            }
        }
        return { reloaded: true, registration, reapplied, generation: this.#lastGeneration };
    }

    /** Fetch the current snapshot and re-register when its generation moved. */
    async refresh(): Promise<ThemeReloadResult> {
        return this.applySnapshot(await this.#source.get());
    }
}
