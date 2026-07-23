// The when-context evaluator, JS side (M9 e07b, design 05 §6 / 03 §6).
//
// A `when` clause is the applicability guard a command (05 §6) — and, later, a keybinding (e07c) —
// carries: a small boolean expression over the SIX editor contexts. This module owns two things and
// nothing else:
//
//   1. THE CONTEXT MODEL. The six contexts (05 §6) — `panelFocus`, `panelType`, `viewportMode`,
//      `playState`, `textInputFocus`, `windowType` — are evaluated from `editor.ui` (the editor-local
//      focus/chrome facts, 05 §5) and the daemon session state (05 §4, D7). Each fact has a home
//      SCOPE, and the four scopes RESOLVE in a fixed precedence — text-input > focused panel > window
//      > global (03 §6) — so a more-specific scope shadows a less-specific one.
//
//      M9 e08b LANDED THE REAL `playState` SOURCE and M9 e08d WIRED IT. `DaemonSessionState` below
//      consumes the daemon's `session` topic facts (e08a — docs/editor-session-state.md) and carries
//      the DAEMON'S OWN L-51 tokens, byte-identical to `gui::playbar::state_token()`; since e08d it
//      is what `boot.ts` resolves the live editor's when-context from, fed over the Shell's
//      `session.state` relay (session.ts). The evaluation semantics did not change; only the source
//      did, exactly as the seam was built for. `editor.ui` is still behind `EditorUiSource` — the
//      real bus is e08c, and it will swap that source the same way.
//
//   2. A TOTAL, FAIL-CLOSED EXPRESSION EVALUATOR. `evaluateWhen` never throws: an empty clause is
//      "always active", and a MALFORMED clause is INACTIVE (false) rather than an exception deep in a
//      command-dispatch path. The grammar is the VS Code-style subset the design's `when` strings use
//      (`panelFocus == inspector`, `!textInputFocus`, `a && b`, `a || b`, parentheses) — deliberately
//      NOT a general expression language.
//
// NO keymap and NO palette here (those are e07c / e07d). This module is the substrate both consume.

/**
 * The L-51 play state (05 §4, D7), owned by the DAEMON.
 *
 * These three tokens are the daemon's own wire vocabulary — byte-identical to
 * `editorkernel::play_state_token()` and `gui::playbar::state_token()`, which is why a `when` clause
 * can compare against them with no translation layer to drift. `edit` (NOT "stopped") is authored
 * truth with no live session: e08b corrected the token when the real source replaced the stub.
 */
export type PlayState = "edit" | "playing" | "paused";

/** The six when-context keys (05 §6). The closed set — a clause referencing any other key is honest
 * about it (that key simply reads as absent), but nothing in the editor sets one. */
export const WHEN_CONTEXT_KEYS = [
    "panelFocus",
    "panelType",
    "viewportMode",
    "playState",
    "textInputFocus",
    "windowType",
] as const;
export type WhenContextKey = (typeof WHEN_CONTEXT_KEYS)[number];

/**
 * A resolved when-context snapshot: the flat key→value map a clause evaluates against.
 *
 * A value is a string (an id/mode/type token, or `""` for "none") or a boolean (`textInputFocus`).
 * A key that is ABSENT is distinct from one that is `""`/`false`: `panelFocus == ""` matches only
 * when the key is present-and-empty, while a bare `panelFocus` is falsy for both.
 */
export type WhenContext = Partial<Record<WhenContextKey, string | boolean>>;

// --------------------------------------------------------------------------- scopes + sources

/**
 * The four when-scope layers, LOWEST precedence first (03 §6: text-input > focused panel > window >
 * global). `resolveWhenContext` merges them in THIS order, so a later (higher) layer overrides an
 * earlier (lower) one for any key they share — the property the resolution-order matrix pins.
 */
export const WHEN_SCOPES = ["global", "window", "focusedPanel", "textInput"] as const;
export type WhenScope = (typeof WHEN_SCOPES)[number];

/** The partial context each scope may contribute. */
export type WhenContextLayers = Partial<Record<WhenScope, WhenContext>>;

/**
 * The editor-local UI facts (05 §5 `editor.ui`): what has focus and which viewport/window is active.
 *
 * An INTERFACE, not a concrete reader, so the real `editor.ui` bus wiring (mirrored across windows by
 * the Shell) swaps the source with zero change to the evaluator or the registry. Until it lands,
 * `STUB_EDITOR_UI` supplies the "nothing focused" baseline.
 */
export interface EditorUiSource {
    /** A DOM editable holds focus (03 §6 keyboard routing: such keys go to CEF, not the keymap). */
    readonly textInputFocus: boolean;
    /** The focused panel's id (`""` = none). */
    readonly panelFocus: string;
    /** The focused panel's kind/type (`""` = none). */
    readonly panelType: string;
    /** The active window's type, e.g. `"main"` (`""` = none). */
    readonly windowType: string;
    /** The active viewport mode, e.g. `"2d"` / `"3d"` (`""` = no viewport focused). */
    readonly viewportMode: string;
}

/**
 * The daemon session-state facts (05 §4, D7).
 *
 * The interface the four scope layers read `playState` from. `DaemonSessionState` below is the ONE
 * implementation (M9 e08b) and, since e08d, the one the live editor actually resolves from — there
 * is deliberately no stub constant beside it any more (see `PLAY_STATE_EVENT`).
 */
export interface SessionStateSource {
    readonly playState: PlayState;
}

/**
 * The daemon's `play-state` fact discriminator (docs/editor-session-state.md).
 *
 * Exported rather than inlined into `applyFact` because the Shell's relay names the SAME token
 * (`session_bridge.h` `kSessionPlayStateEvent`) and the `webui-panel-contract` gate cross-checks the
 * two out of the built bundle. A drift makes every reply silently unrecognised — which presents
 * EXACTLY like the frozen boot baseline e08d removed, so it is mechanised rather than trusted.
 *
 * Declared HERE, not in session.ts, so this module keeps its zero-IMPORT surface (see `isRecord`).
 */
export const PLAY_STATE_EVENT = "play-state";

/** The two sources the six contexts are evaluated from. */
export interface WhenContextSources {
    readonly editorUi: EditorUiSource;
    readonly session: SessionStateSource;
}

/** The "nothing focused, no viewport, no window" editor.ui baseline (the pre-wiring stub). */
export const STUB_EDITOR_UI: EditorUiSource = {
    textInputFocus: false,
    panelFocus: "",
    panelType: "",
    windowType: "",
    viewportMode: "",
};

/**
 * THE SESSION-STATE SOURCE (M9 e08b, wired by e08d): the daemon's `session` topic, projected onto
 * the one fact the when-context model reads.
 *
 * ITS BOOT BASELINE IS `edit`, AND THAT IS NOT A FICTION — it is what the daemon itself holds at
 * boot, because play state is deliberately not persisted across a restart
 * (docs/editor-session-state.md: "restoring `playing` would be a lie about L-51 provenance"). A
 * freshly constructed instance is therefore CORRECT until the first fact arrives; what e08d added is
 * the feed that makes it stay correct afterwards. There is no separate stub constant: e08b left one
 * (`STUB_SESSION_STATE`) as the boot baseline for a caller with no subscription, `boot.ts` resolved
 * the LIVE editor's when-context from it, and the result was a `playState` frozen at `edit` for the
 * whole session — so e08d deleted it rather than leaving a placeholder that already proved it can
 * become permanent.
 *
 * It is a SINK, not a poller: something else hands it every payload (`session.ts`'s `SessionFeed`
 * over the Shell relay), exactly as the native Shell's `SessionFeed` does for the C++ panels. Two
 * properties are carried over from that C++ side deliberately, because they are the contract and not
 * an implementation detail:
 *
 *   * **`origin` echo suppression.** The daemon fans every fact out to every subscriber with no
 *     per-client filtering; a consumer APPLIES a fact whose `origin` differs from its own client id
 *     and DROPS one that matches. Set `clientId` from the attach reply. `0` means "not attached",
 *     and is ALSO the daemon's own origin, so a 0/0 match is deliberately not treated as an echo —
 *     otherwise an unattached client would silently swallow every daemon-originated fact.
 *   * **Tolerance.** A malformed payload is IGNORED, never thrown: this feeds command-palette
 *     filtering, and a `when` evaluation that throws deep in a dispatch path is strictly worse than
 *     one that keeps the last known state.
 */
export class DaemonSessionState implements SessionStateSource {
    /** This connection's echo-suppression identity (`clientId` from the attach reply). */
    clientId = 0;

    #playState: PlayState = "edit";
    #applied = 0;
    #echoesDropped = 0;

    get playState(): PlayState {
        return this.#playState;
    }

    /** How many facts actually moved the state, and how many were dropped as our own echo. */
    get applied(): number {
        return this.#applied;
    }
    get echoesDropped(): number {
        return this.#echoesDropped;
    }

    /**
     * Apply one `session` topic payload. Returns true only when the state actually changed, so a
     * caller can skip a palette re-filter it does not need.
     *
     * Recognises `play-state` and ignores the rest (`selection-changed` / `camera-changed` carry no
     * when-context key — the six keys are a closed set, 05 §6).
     */
    applyFact(payload: unknown): boolean {
        if (!isRecord(payload)) {
            return false;
        }
        const origin = typeof payload["origin"] === "number" ? payload["origin"] : 0;
        if (this.clientId !== 0 && origin === this.clientId) {
            this.#echoesDropped += 1;
            return false;
        }
        if (payload["event"] !== PLAY_STATE_EVENT) {
            return false;
        }
        const state = toPlayState(payload["state"]);
        if (state === null || state === this.#playState) {
            return false;
        }
        this.#playState = state;
        this.#applied += 1;
        return true;
    }

    /** Reset to the boot baseline — what a reconnect means (a restarted daemon holds no session). */
    reset(): void {
        this.#playState = "edit";
    }
}

/** A plain-object type guard, local so this module keeps its zero-import surface. */
function isRecord(value: unknown): value is Record<string, unknown> {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}

/**
 * A wire `state` token -> PlayState. `null` for anything else, so an unknown token from a NEWER
 * daemon leaves the last known state alone rather than silently reading as `edit` — claiming "no live
 * session" on a token we simply do not understand would be a confident lie.
 */
function toPlayState(token: unknown): PlayState | null {
    return token === "edit" || token === "playing" || token === "paused" ? token : null;
}

/**
 * Map the two sources onto the four scope layers, placing each fact in its home scope: `playState`
 * is global, window chrome (`windowType`/`viewportMode`) is the window scope, panel focus
 * (`panelFocus`/`panelType`) the focused-panel scope, and `textInputFocus` the text-input scope.
 */
export function sourcesToLayers(sources: WhenContextSources): WhenContextLayers {
    const ui = sources.editorUi;
    return {
        global: { playState: sources.session.playState },
        window: { windowType: ui.windowType, viewportMode: ui.viewportMode },
        focusedPanel: { panelFocus: ui.panelFocus, panelType: ui.panelType },
        textInput: { textInputFocus: ui.textInputFocus },
    };
}

/**
 * Merge scope layers into one resolved snapshot, applying the 03 §6 precedence (text-input >
 * focused panel > window > global). `WHEN_SCOPES` is ordered lowest-to-highest, so iterating it and
 * overwriting yields "the highest scope that sets a key wins".
 */
export function resolveWhenContext(layers: WhenContextLayers): WhenContext {
    const out: WhenContext = {};
    for (const scope of WHEN_SCOPES) {
        const layer = layers[scope];
        if (layer === undefined) {
            continue;
        }
        for (const key of WHEN_CONTEXT_KEYS) {
            const value = layer[key];
            if (value !== undefined) {
                out[key] = value;
            }
        }
    }
    return out;
}

/** Resolve the six contexts from their two sources, honouring the scope precedence. */
export function resolveContext(sources: WhenContextSources): WhenContext {
    return resolveWhenContext(sourcesToLayers(sources));
}

// ------------------------------------------------------------------------- the clause evaluator

/** Render a context value as the string a `==` / `!=` comparison compares against. */
function valueToken(value: string | boolean | undefined): string {
    if (value === undefined) {
        return "";
    }
    return typeof value === "boolean" ? (value ? "true" : "false") : value;
}

/** Is a context value truthy for a BARE key reference (`panelFocus`, `textInputFocus`)? */
function isTruthy(value: string | boolean | undefined): boolean {
    if (typeof value === "boolean") {
        return value;
    }
    // A non-empty string is truthy; `""`/absent are falsy. "false" is a STRING and thus truthy as a
    // bare reference — which is why booleans belong to boolean keys, never string ones.
    return typeof value === "string" && value.length > 0;
}

type Token =
    | { readonly kind: "id"; readonly text: string }
    | { readonly kind: "str"; readonly text: string }
    | { readonly kind: "op"; readonly text: "==" | "!=" | "&&" | "||" | "!" | "(" | ")" };

/** The identifier char class (context keys + unquoted values): letters, digits, `_`, `.`, `-`. */
const ID_CHAR = /[A-Za-z0-9_.\-]/;

/** Tokenize a clause. Returns `null` on any unrecognised character (the fail-closed path). */
function tokenize(clause: string): Token[] | null {
    const tokens: Token[] = [];
    let i = 0;
    const n = clause.length;
    while (i < n) {
        const c = clause[i] ?? "";
        if (c === " " || c === "\t" || c === "\n" || c === "\r") {
            i += 1;
            continue;
        }
        if (c === "(" || c === ")") {
            tokens.push({ kind: "op", text: c });
            i += 1;
            continue;
        }
        if (c === "'" || c === '"') {
            // A quoted literal — read to the matching quote; an unterminated quote fails closed.
            let j = i + 1;
            while (j < n && clause[j] !== c) {
                j += 1;
            }
            if (j >= n) {
                return null;
            }
            tokens.push({ kind: "str", text: clause.slice(i + 1, j) });
            i = j + 1;
            continue;
        }
        if (c === "=" || c === "!") {
            if (clause[i + 1] === "=") {
                tokens.push({ kind: "op", text: c === "=" ? "==" : "!=" });
                i += 2;
                continue;
            }
            if (c === "!") {
                tokens.push({ kind: "op", text: "!" });
                i += 1;
                continue;
            }
            return null; // a lone `=` is not valid
        }
        if (c === "&" || c === "|") {
            if (clause[i + 1] === c) {
                tokens.push({ kind: "op", text: c === "&" ? "&&" : "||" });
                i += 2;
                continue;
            }
            return null; // a lone `&`/`|` is not valid
        }
        if (ID_CHAR.test(c)) {
            let j = i;
            while (j < n && ID_CHAR.test(clause[j] ?? "")) {
                j += 1;
            }
            tokens.push({ kind: "id", text: clause.slice(i, j) });
            i = j;
            continue;
        }
        return null; // an unrecognised character fails the whole clause closed
    }
    return tokens;
}

/**
 * A recursive-descent parser+evaluator over the token stream. It evaluates against `ctx` as it
 * parses (the clauses are tiny; a separate AST would be ceremony). Precedence: `||` < `&&` < unary
 * `!` < primary. Any structural error throws `ClauseError`, which `evaluateWhen` converts to `false`.
 */
class ClauseError extends Error {}

class ClauseEvaluator {
    readonly #tokens: readonly Token[];
    readonly #ctx: WhenContext;
    #pos = 0;

    constructor(tokens: readonly Token[], ctx: WhenContext) {
        this.#tokens = tokens;
        this.#ctx = ctx;
    }

    evaluate(): boolean {
        const value = this.#or();
        if (this.#pos !== this.#tokens.length) {
            throw new ClauseError("trailing tokens");
        }
        return value;
    }

    #peek(): Token | undefined {
        return this.#tokens[this.#pos];
    }

    #or(): boolean {
        let value = this.#and();
        for (;;) {
            const token = this.#peek();
            if (token?.kind === "op" && token.text === "||") {
                this.#pos += 1;
                // NO short-circuit: the right side is a pure context read with no side effects, and
                // evaluating it keeps a malformed right operand an honest parse error rather than one
                // hidden behind a truthy left.
                const right = this.#and();
                value = value || right;
            } else {
                return value;
            }
        }
    }

    #and(): boolean {
        let value = this.#unary();
        for (;;) {
            const token = this.#peek();
            if (token?.kind === "op" && token.text === "&&") {
                this.#pos += 1;
                const right = this.#unary();
                value = value && right;
            } else {
                return value;
            }
        }
    }

    #unary(): boolean {
        const token = this.#peek();
        if (token?.kind === "op" && token.text === "!") {
            this.#pos += 1;
            return !this.#unary();
        }
        return this.#primary();
    }

    #primary(): boolean {
        const token = this.#peek();
        if (token === undefined) {
            throw new ClauseError("unexpected end of clause");
        }
        if (token.kind === "op" && token.text === "(") {
            this.#pos += 1;
            const value = this.#or();
            const close = this.#peek();
            if (close?.kind !== "op" || close.text !== ")") {
                throw new ClauseError("expected )");
            }
            this.#pos += 1;
            return value;
        }
        if (token.kind !== "id") {
            throw new ClauseError("expected a context key");
        }
        // A key, optionally followed by a comparison. `token.text` is the context key name.
        this.#pos += 1;
        const key = token.text;
        const next = this.#peek();
        if (next?.kind === "op" && (next.text === "==" || next.text === "!=")) {
            this.#pos += 1;
            const rhs = this.#peek();
            if (rhs === undefined || (rhs.kind !== "id" && rhs.kind !== "str")) {
                throw new ClauseError("expected a value after a comparison");
            }
            this.#pos += 1;
            const actual = valueToken(this.#read(key));
            return next.text === "==" ? actual === rhs.text : actual !== rhs.text;
        }
        return isTruthy(this.#read(key));
    }

    #read(key: string): string | boolean | undefined {
        // A clause may reference any of the six keys; an unknown key reads as absent (never an error).
        return (this.#ctx as Record<string, string | boolean | undefined>)[key];
    }
}

/**
 * Evaluate a `when` clause against a resolved context.
 *
 * TOTAL and FAIL-CLOSED: an empty (or whitespace-only) clause is `true` ("always active"), and a
 * clause that does not tokenize/parse is `false` — a command whose guard cannot be understood must
 * not silently become active. Never throws.
 */
export function evaluateWhen(clause: string, ctx: WhenContext): boolean {
    if (clause.trim() === "") {
        return true;
    }
    const tokens = tokenize(clause);
    if (tokens === null || tokens.length === 0) {
        return false;
    }
    try {
        return new ClauseEvaluator(tokens, ctx).evaluate();
    } catch {
        return false;
    }
}
