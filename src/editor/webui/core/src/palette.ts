// The command palette, JS side (M9 e07d, design 05 §6 / 04 / 10). R-HUX-004 ships here.
//
// D8 closes with this module: e07b built the ONE command registry, e07c bound keys to its commands,
// and this is the UI that makes every registered command reachable by name — "the palette surface ≡
// the scriptable surface" (10). Two layers with a hard boundary between them, the same split the rest
// of editor-core uses so the pure logic stays testable without a DOM:
//
//   1. THE PALETTE MODEL (`Palette`, pure TS). Holds the open state + the query, and PROJECTS the
//      registry into a ranked, when-filtered result list — a fuzzy filter over the e07b registry that
//      keeps only the commands active in the resolved when-context (05 §6). It EXECUTES a command by
//      delegating to `registry.execute`, so a palette dispatch and a keymap dispatch run the SAME
//      handler through the SAME registry — there is no second execution path to drift.
//
//   2. THE PALETTE VIEW (`PaletteView`, DOM). A thin overlay — a filter input and a results list —
//      that reflects the model and shows each command's INTROSPECTED docs (05 §6 "palette entries
//      carry the docs"). It owns no command logic; it renders the model and forwards keystrokes to it.
//      Kept DOM-only so the model's ranking/filtering is unit-testable in the browser-free tier.
//
// NO raw key SHORTCUT handlers here (05 §6, enforced by the T1 `webui-no-raw-key-handlers` lint): the
// palette is OPENED by a command (`workbench.palette.toggle`) bound in the keymap like everything else,
// and its own input handles only list navigation + activation while it is open — never a global chord.

import type { Command, CommandOutcome, CommandRegistry } from "./commands.js";
import type { WhenContext } from "./when.js";

// --------------------------------------------------------------------------- the palette-open command

/** The stable id of the command that opens/closes the palette (bound to Ctrl+Shift+P in the keymap). */
export const PALETTE_TOGGLE_COMMAND_ID = "workbench.palette.toggle";

/**
 * The action the palette-open command dispatches to.
 *
 * An INTERFACE so `paletteCommands` can register the command SURFACE (which the registry then holds,
 * the palette shows, and the keymap binds) while the app supplies the actual `Palette` to toggle — the
 * same swappable-actions discipline `EditorCommandActions` uses.
 */
export interface PaletteActions {
    toggle(): CommandOutcome | Promise<CommandOutcome>;
}

/**
 * The built-in command that opens the palette (05 §6 / 10 "open palette").
 *
 * Registered as its OWN source rather than folded into `editorCommands` so the e07b assembly counts
 * (and its tests) are untouched: `boot.ts` registers this alongside the built registry. `when: ""`
 * because the palette must be openable from anywhere — including from inside a focused text field,
 * where a user reaching for a command is exactly when they need it most.
 */
export function paletteCommands(actions: PaletteActions): readonly Command[] {
    return [
        {
            id: PALETTE_TOGGLE_COMMAND_ID,
            title: "Show Command Palette",
            category: "editor",
            when: "",
            docs: {
                summary: "Open the command palette — every command by name, keyboard-only",
                detail:
                    "workbench action; bound to Ctrl+Shift+P; the universal keyboard path to every " +
                    "registered command (R-A11Y-001 / R-CLI-001)",
            },
            handler: () => actions.toggle(),
        },
    ];
}

// --------------------------------------------------------------------------- the fuzzy filter

/** A fuzzy match of a query against a candidate string: its score plus the matched char indices. */
export interface FuzzyMatch {
    /** Higher is a better match. Only meaningful relative to other matches of the SAME query. */
    readonly score: number;
    /** The indices in the candidate that the query characters matched, ascending — for highlighting. */
    readonly positions: readonly number[];
}

/** True where a character begins a new "word" (start of string, or a lower→upper / sep→alnum edge). */
function isWordStart(text: string, index: number): boolean {
    if (index === 0) {
        return true;
    }
    const prev = text[index - 1] ?? "";
    const cur = text[index] ?? "";
    const isSep = (c: string): boolean => c === " " || c === "." || c === "-" || c === "_" || c === "/";
    if (isSep(prev)) {
        return true;
    }
    // camelCase / PascalCase boundary: a lower or digit followed by an upper letter.
    return /[a-z0-9]/.test(prev) && /[A-Z]/.test(cur);
}

/**
 * Fuzzy-match `query` against `candidate` (case-insensitive subsequence), returning `null` on no match.
 *
 * The query characters must appear IN ORDER in the candidate; the score rewards the qualities that make
 * a palette feel right — a CONSECUTIVE run of matches, matches at WORD STARTS (`clsp` → "**Cl**ose
 * **P**anel"), and an early first match. An empty query matches everything with score 0 (the "show all"
 * case the caller special-cases anyway). Deterministic and allocation-light: one left-to-right pass with
 * a greedy word-start preference, which is ample for the small command sets a palette ranks.
 */
export function fuzzyMatch(query: string, candidate: string): FuzzyMatch | null {
    const q = query.toLowerCase();
    const c = candidate.toLowerCase();
    if (q === "") {
        return { score: 0, positions: [] };
    }
    const positions: number[] = [];
    let score = 0;
    let ci = 0;
    let prevMatch = -2; // so the first match is never counted as "consecutive"
    for (let qi = 0; qi < q.length; qi += 1) {
        const target = q[qi] ?? "";
        // Greedy scan for the next occurrence, PREFERRING a word-start occurrence within the remaining
        // run so "clsp" locks onto the capitals rather than the first stray letter.
        let found = -1;
        let firstAny = -1;
        for (let k = ci; k < c.length; k += 1) {
            if (c[k] !== target) {
                continue;
            }
            if (firstAny === -1) {
                firstAny = k;
            }
            if (isWordStart(candidate, k)) {
                found = k;
                break;
            }
        }
        if (found === -1) {
            found = firstAny;
        }
        if (found === -1) {
            return null; // a query char never appears in the rest of the candidate — no match
        }
        positions.push(found);
        // Score contributions.
        if (found === prevMatch + 1) {
            score += 5; // consecutive run — the strongest signal
        }
        if (isWordStart(candidate, found)) {
            score += 3; // matched at a word boundary
        }
        score += found === 0 ? 2 : 0; // matched the very first char
        prevMatch = found;
        ci = found + 1;
    }
    // Shorter candidates that a query covers more of rank higher (a tie-break toward the tightest match).
    score += Math.max(0, 10 - (candidate.length - q.length) / 4);
    return { score, positions };
}

// --------------------------------------------------------------------------- the palette model

/** One ranked result the palette exposes: the command, its match score, and the matched indices. */
export interface PaletteEntry {
    readonly command: Command;
    readonly score: number;
    /** The indices in the command title that the query matched (empty for an empty query). */
    readonly positions: readonly number[];
}

/**
 * The command palette MODEL — pure logic over the e07b registry, no DOM.
 *
 * Owns two pieces of state: whether it is OPEN and the current filter QUERY. Everything else is derived
 * on demand from the registry + a resolved when-context, so there is no cached command list to drift out
 * of sync with a registry that changed (a panel installed a command, a contract verb appeared).
 */
export class Palette {
    readonly #registry: CommandRegistry;
    #open = false;
    #query = "";

    constructor(registry: CommandRegistry) {
        this.#registry = registry;
    }

    get isOpen(): boolean {
        return this.#open;
    }

    get query(): string {
        return this.#query;
    }

    /** Open the palette. Resets the query so it always opens on the full list, VS Code-style. */
    open(): void {
        this.#open = true;
        this.#query = "";
    }

    /** Close the palette (and clear the query, so a reopen starts fresh). */
    close(): void {
        this.#open = false;
        this.#query = "";
    }

    /** Open when closed, close when open — the `workbench.palette.toggle` command's effect. */
    toggle(): void {
        if (this.#open) {
            this.close();
        } else {
            this.open();
        }
    }

    /** Set the filter query (typed into the palette input). */
    setQuery(query: string): void {
        this.#query = query;
    }

    /**
     * The ranked, when-filtered results for the current query in the given context.
     *
     * Two filters compose: the registry's when-context filter (only commands ACTIVE in `context`
     * survive — a move-panel command is hidden with nothing focused) and the fuzzy query filter. An
     * EMPTY query returns every active command in the registry's own order (contract → editor → panel),
     * so the palette opens on the whole surface; a non-empty query keeps only the fuzzy matches, ranked
     * best-first with the title as a stable tie-break.
     */
    results(context: WhenContext): readonly PaletteEntry[] {
        const active = this.#registry.query({ context });
        const query = this.#query.trim();
        if (query === "") {
            return active.map((command) => ({ command, score: 0, positions: [] }));
        }
        const matched: PaletteEntry[] = [];
        for (const command of active) {
            // Match against the TITLE (what the user sees) primarily, and fall back to the id so a
            // user who types an id fragment (`view.panel.close`) still finds it (`??` evaluates the id
            // match only when the title did not match).
            const titleMatch = fuzzyMatch(query, command.title);
            const match = titleMatch ?? fuzzyMatch(query, command.id);
            if (match === null) {
                continue;
            }
            matched.push({
                command,
                score: match.score,
                // Highlight positions only apply when the TITLE matched; an id-only match has none.
                positions: titleMatch !== null ? titleMatch.positions : [],
            });
        }
        matched.sort(
            (a, b) => b.score - a.score || a.command.title.localeCompare(b.command.title),
        );
        return matched;
    }

    /**
     * Close the palette, then execute a command by id through the SHARED registry.
     *
     * The palette closes FIRST — SYNCHRONOUSLY, the moment a command is chosen (VS Code's behaviour):
     * the picker's job is done, and a command whose handler runs async (an RPC verb) must not leave the
     * overlay lingering over the result. Then it delegates to `registry.execute` — the SAME path the
     * keymap dispatches through — so a palette run and a keymap run of a command are byte-for-byte the
     * same execution. An unknown id is the registry's ordinary `ok: false` outcome (a stale entry),
     * never a throw.
     */
    async execute(id: string): Promise<CommandOutcome> {
        this.close();
        return await this.#registry.execute(id);
    }
}
