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
//      ⚠ e08 (session state) HAS NOT LANDED. `playState` (and, later, selection) is therefore read
//      from a LOCAL STUB behind `SessionStateSource`, so e08 swaps only that source with ZERO change
//      to the registry or this evaluator. `editor.ui` is behind `EditorUiSource` for the same reason —
//      the real bus wiring swaps the source, not the model.
//
//   2. A TOTAL, FAIL-CLOSED EXPRESSION EVALUATOR. `evaluateWhen` never throws: an empty clause is
//      "always active", and a MALFORMED clause is INACTIVE (false) rather than an exception deep in a
//      command-dispatch path. The grammar is the VS Code-style subset the design's `when` strings use
//      (`panelFocus == inspector`, `!textInputFocus`, `a && b`, `a || b`, parentheses) — deliberately
//      NOT a general expression language.
//
// NO keymap and NO palette here (those are e07c / e07d). This module is the substrate both consume.

/** The play state (05 §4, D7). Owned by e08 (session state); read from a stub until it lands. */
export type PlayState = "stopped" | "playing" | "paused";

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
 * The daemon session-state facts (05 §4, D7) — OWNED BY e08 (session state), NOT LANDED.
 *
 * Read from `STUB_SESSION_STATE` behind this interface so e08 swaps ONLY this source (with zero
 * registry change), exactly as the spec requires.
 */
export interface SessionStateSource {
    readonly playState: PlayState;
}

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

/** The stopped-session baseline (the e08 stub — the ONE source e08 replaces). */
export const STUB_SESSION_STATE: SessionStateSource = {
    playState: "stopped",
};

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
