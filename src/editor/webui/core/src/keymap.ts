// The keymap, JS side (M9 e07c, design 05 §6 / 03 §6).
//
// D8 finishes here: every editor capability is a command (e07b), and this module BINDS KEYS to those
// commands. Two layers, and a hard boundary between them:
//
//   1. THE DEFAULT KEYMAP + the resolver (this file, pure TS). Every binding targets a COMMAND ID
//      (from e07b's registry) and carries a `when` clause resolved through the SAME e07b evaluator
//      (when.ts) — there are no raw key handlers anywhere (05 §6). A keystroke resolves to a command
//      id through the when-contexts (03 §6 precedence: text-input > focused panel > window > global),
//      and the registry executes it. Session undo/redo (`session.undo` / `session.redo`,
//      undo_journal.h) finally get their real Ctrl+Z / Ctrl+Y bindings through this path.
//
//   2. THE USER OVERRIDE (`~/.context/keybindings.json`). editor-core is a PURE WIRE-CLIENT — it
//      CANNOT read that file itself (04 §1 / 08 §1). The Shell reads/watches/hot-reloads it and
//      PUBLISHES the raw bytes over the e05c bridge (keybindings_bridge.h, `keybindings.get`); this
//      module SCHEMA-VALIDATES and MERGES them. A malformed or version-incompatible override is
//      REJECTED with a diagnostic and NEVER silently applied — the previous effective map stands, so
//      a bad hand-edit can never leave the editor with no working keys. Hot reload is just re-applying
//      a new payload over the same `Keymap` (the Shell's watch drives a fresh `keybindings.get`).
//
// The `$schema`/version discipline mirrors the theme + panel-state files: an override that does not
// name this schema, or names an incompatible version, is refused rather than guessed at.

import { evaluateWhen, type WhenContext } from "./when.js";
import type { CommandOutcome, CommandRegistry } from "./commands.js";
import { PALETTE_TOGGLE_COMMAND_ID } from "./palette.js";
import { BridgeError, isRecord, type ShellBridge } from "./bridge.js";

// --------------------------------------------------------------------------- the wire vocabulary
// MUST match keybindings_bridge.h's kKeybindingsGetMethod. Cross-checked byte-for-byte out of the
// BUILT bundle by `tools/check_webui_assets.py --panel-contract` (ctest `webui-panel-contract`), the
// same drift gate the panel + editor-state surfaces ride: a rename on one side would otherwise leave
// editor-core calling a method the Shell no longer routes, so the user override would silently never
// load and NOTHING would report it.
export const KEYBINDINGS_GET_METHOD = "keybindings.get";

/** The `$schema` id a `~/.context/keybindings.json` override MUST name to be accepted. */
export const KEYBINDINGS_SCHEMA_ID = "https://schemas.context-engine.dev/m9/keybindings.schema.json";
/** The schema version this build understands. An override naming a different version is refused. */
export const KEYBINDINGS_SCHEMA_VERSION = 1;

// ------------------------------------------------------------------------------- chord canonicalization
//
// A CHORD is a canonical string form of a key combination — modifiers in a FIXED order
// (Ctrl+Alt+Shift+Meta) followed by the key, e.g. `Ctrl+Shift+Z`. Both the default map's declared
// keys and a live KeyboardEvent are reduced to this ONE form so a binding and a keystroke can be
// compared as plain strings. Without a canonical form, `ctrl+z`, `Control+z` and `Z+Ctrl` would be
// three different keys for the same chord.

/** Modifier order in a canonical chord — stable so two spellings of the same combo compare equal. */
const CHORD_MODIFIER_ORDER = ["Ctrl", "Alt", "Shift", "Meta"] as const;

/** The bare modifier `key` values a keystroke carries while a modifier is held alone (never a chord). */
const BARE_MODIFIER_KEYS = new Set(["Control", "Shift", "Alt", "Meta", "AltGraph"]);

/** Canonicalize the non-modifier key token: a single printable char upper-cases; named keys are kept. */
function canonicalizeKeyToken(token: string): string {
    // A single character (`z`, `/`, `1`) is compared case-insensitively — Shift is carried as a
    // MODIFIER, so `Z` and `z` are the same key. Named keys (`ArrowLeft`, `Enter`, `F2`, `Escape`)
    // keep their DOM `KeyboardEvent.key` casing, which is already canonical.
    return token.length === 1 ? token.toUpperCase() : token;
}

/** Map a modifier spelling to its canonical token, or `null` when the token is not a modifier. */
function modifierToken(token: string): string | null {
    switch (token.toLowerCase()) {
        case "ctrl":
        case "control":
            return "Ctrl";
        case "alt":
        case "option":
            return "Alt";
        case "shift":
            return "Shift";
        case "meta":
        case "cmd":
        case "command":
        case "win":
        case "super":
            return "Meta";
        default:
            return null;
    }
}

/**
 * Canonicalize a written chord (`"ctrl+shift+z"`, `"Alt+ArrowLeft"`) into its stable form.
 *
 * Returns `null` for a chord with no non-modifier key, with two non-modifier keys, or with an empty
 * segment — a binding the resolver could never match, so it is refused at parse rather than kept as
 * dead configuration.
 */
export function canonicalizeChord(raw: string): string | null {
    const parts = raw
        .split("+")
        .map((part) => part.trim())
        .filter((part) => part.length > 0);
    if (parts.length === 0) {
        return null;
    }
    const modifiers = new Set<string>();
    let key: string | null = null;
    for (const part of parts) {
        const modifier = modifierToken(part);
        if (modifier !== null) {
            modifiers.add(modifier);
            continue;
        }
        if (key !== null) {
            return null; // two non-modifier keys is not a chord this resolver models
        }
        key = canonicalizeKeyToken(part);
    }
    if (key === null) {
        return null; // modifiers with no key never fire
    }
    const ordered = CHORD_MODIFIER_ORDER.filter((modifier) => modifiers.has(modifier));
    return [...ordered, key].join("+");
}

/** The subset of a DOM `KeyboardEvent` the chord form reads — so the input path can pass one directly. */
export interface KeyStroke {
    readonly key: string;
    readonly ctrlKey?: boolean;
    readonly altKey?: boolean;
    readonly shiftKey?: boolean;
    readonly metaKey?: boolean;
}

/**
 * Reduce a live keystroke to its canonical chord.
 *
 * `null` for a bare modifier press (`Control` alone) or an empty key — neither is a chord, and
 * returning `null` keeps a held modifier from resolving to a command every repeat.
 */
export function keyStrokeToChord(stroke: KeyStroke): string | null {
    if (stroke.key === "" || BARE_MODIFIER_KEYS.has(stroke.key)) {
        return null;
    }
    const modifiers: string[] = [];
    if (stroke.ctrlKey === true) {
        modifiers.push("Ctrl");
    }
    if (stroke.altKey === true) {
        modifiers.push("Alt");
    }
    if (stroke.shiftKey === true) {
        modifiers.push("Shift");
    }
    if (stroke.metaKey === true) {
        modifiers.push("Meta");
    }
    // `modifiers` is already built in CHORD_MODIFIER_ORDER, so the form matches canonicalizeChord's.
    return [...modifiers, canonicalizeKeyToken(stroke.key)].join("+");
}

// ------------------------------------------------------------------------------- bindings + the default map

/**
 * One key→command binding.
 *
 * `key` is a CANONICAL chord (canonicalizeChord'd at construction), `command` is a registry command
 * id, and `when` is a when-clause (`""` = always). An EMPTY `command` is a deliberate UNBIND: a user
 * override can mask a default binding by rebinding its chord to `""`.
 */
export interface Keybinding {
    readonly key: string;
    readonly command: string;
    readonly when: string;
}

/** Build a binding, canonicalizing its chord. Throws on an unparseable chord — a coding error in a
 * DEFAULT binding, caught by the T1 tests; user-override chords are validated (not thrown) at parse. */
function binding(key: string, command: string, when = ""): Keybinding {
    const canonical = canonicalizeChord(key);
    if (canonical === null) {
        throw new Error(`invalid default keybinding chord: ${key}`);
    }
    return { key: canonical, command, when };
}

/**
 * The default keymap shipped in editor-core (05 §6 "default map ships in editor-core").
 *
 * Every entry targets a real command id from e07b's registry. The undo/redo pair is this task's
 * headline binding (design 05 §6 / 04): `session.undo` / `session.redo` reach Ctrl+Z / Ctrl+Y (and
 * Ctrl+Shift+Z, the conventional second redo) — guarded on `!textInputFocus`, because when a DOM
 * editable has focus those keys belong to it (03 §6: such keys go to CEF, not the keymap). The panel
 * nav/dock bindings target the e07b editor commands, whose own `when` clauses the resolver honours.
 */
export const DEFAULT_KEYBINDINGS: readonly Keybinding[] = [
    binding("Ctrl+Z", "session.undo", "!textInputFocus"),
    binding("Ctrl+Y", "session.redo", "!textInputFocus"),
    binding("Ctrl+Shift+Z", "session.redo", "!textInputFocus"),
    // Dock navigation — reachable by keyboard alone (R-CLI-001), guarded like their commands.
    binding("Ctrl+K", "view.panel.focusNext", "!textInputFocus"),
    binding("Ctrl+Shift+K", "view.panel.focusPrevious", "!textInputFocus"),
    binding("Alt+ArrowLeft", "view.panel.move.left", "panelFocus && !textInputFocus"),
    binding("Alt+ArrowRight", "view.panel.move.right", "panelFocus && !textInputFocus"),
    binding("Alt+ArrowUp", "view.panel.move.up", "panelFocus && !textInputFocus"),
    binding("Alt+ArrowDown", "view.panel.move.down", "panelFocus && !textInputFocus"),
    binding("Ctrl+W", "view.panel.close", "panelFocus && !textInputFocus"),
    binding("Ctrl+T", "view.theme.toggle"),
    // The command palette (e07d): openable from ANYWHERE — including from inside a text field — so it
    // carries no `when` guard. This is the universal keyboard path to every command (R-CLI-001).
    binding("Ctrl+Shift+P", PALETTE_TOGGLE_COMMAND_ID),
];

// ------------------------------------------------------------------------------- the user override schema

/** A parsed, VALID user override (already canonicalized). */
export interface KeybindingsOverride {
    readonly bindings: readonly Keybinding[];
}

/** The result of parsing the override text: either a valid override or a diagnostic (never both). */
export type KeybindingsParseResult =
    | { readonly ok: true; readonly override: KeybindingsOverride }
    | { readonly ok: false; readonly diagnostic: string };

/**
 * Parse + schema-validate a `~/.context/keybindings.json` payload.
 *
 * TOTAL and FAIL-CLOSED: any structural problem — bad JSON, wrong `$schema`, an incompatible
 * `version`, a non-array `bindings`, a malformed entry, an unparseable chord — returns a DIAGNOSTIC
 * rather than a partial map. A file that cannot be fully understood is never partially applied (design
 * 05 §6: "REJECTED with a diagnostic, never silently applied"). Never throws.
 */
export function parseUserKeybindings(text: string): KeybindingsParseResult {
    let parsed: unknown;
    try {
        parsed = JSON.parse(text);
    } catch (error) {
        const detail = error instanceof Error ? error.message : String(error);
        return { ok: false, diagnostic: `keybindings.json is not valid JSON: ${detail}` };
    }
    if (!isRecord(parsed)) {
        return { ok: false, diagnostic: "keybindings.json must be a JSON object" };
    }
    if (parsed["$schema"] !== KEYBINDINGS_SCHEMA_ID) {
        return {
            ok: false,
            diagnostic: `keybindings.json must declare "$schema": "${KEYBINDINGS_SCHEMA_ID}"`,
        };
    }
    if (parsed["version"] !== KEYBINDINGS_SCHEMA_VERSION) {
        return {
            ok: false,
            diagnostic:
                `keybindings.json "version" must be ${KEYBINDINGS_SCHEMA_VERSION}, ` +
                `got ${JSON.stringify(parsed["version"])}`,
        };
    }
    const rawBindings = parsed["bindings"];
    if (!Array.isArray(rawBindings)) {
        return { ok: false, diagnostic: 'keybindings.json "bindings" must be an array' };
    }
    const bindings: Keybinding[] = [];
    for (let index = 0; index < rawBindings.length; index += 1) {
        const entry = rawBindings[index];
        if (!isRecord(entry)) {
            return { ok: false, diagnostic: `bindings[${index}] must be an object` };
        }
        const key = entry["key"];
        if (typeof key !== "string" || key.trim() === "") {
            return { ok: false, diagnostic: `bindings[${index}].key must be a non-empty string` };
        }
        const canonical = canonicalizeChord(key);
        if (canonical === null) {
            return { ok: false, diagnostic: `bindings[${index}].key is not a valid chord: ${key}` };
        }
        // `command` may be "" — an explicit UNBIND of the chord (masks a default). A missing or
        // non-string command is malformed.
        const command = entry["command"];
        if (typeof command !== "string") {
            return {
                ok: false,
                diagnostic: `bindings[${index}].command must be a string ("" unbinds the chord)`,
            };
        }
        // `when` is optional; when present it must be a string. The clause itself is not validated
        // here — evaluateWhen is fail-closed, so a malformed clause simply makes the binding inactive.
        const when = entry["when"];
        if (when !== undefined && typeof when !== "string") {
            return { ok: false, diagnostic: `bindings[${index}].when must be a string when present` };
        }
        bindings.push({ key: canonical, command, when: typeof when === "string" ? when : "" });
    }
    return { ok: true, override: { bindings } };
}

// ------------------------------------------------------------------------------- the Keymap

/** What an `applyUserOverride` call did — returned so the caller (and a test) can assert on it. */
export interface KeymapApplyResult {
    /** True when the payload became the effective override; false when it was rejected (previous kept). */
    readonly applied: boolean;
    /** True when the override was CLEARED (a null payload) back to the defaults. */
    readonly cleared: boolean;
    /** `""` on success; the rejection reason on failure. */
    readonly diagnostic: string;
    /** The user bindings now in effect (0 when defaults-only). */
    readonly userBindingCount: number;
}

/**
 * The resolved keymap: the default bindings plus the current user override, with a resolver.
 *
 * RESOLUTION PRECEDENCE. The effective list is `[...defaults, ...user]`, and `resolveCommand` scans it
 * from the END, so the LAST matching active binding wins — a user binding outranks a default, and a
 * later user binding outranks an earlier one. A winner whose command is `""` is an explicit UNBIND
 * and resolves to `undefined` (the chord is masked). A binding is a candidate only when its chord
 * matches AND its `when` is active in the resolved context — the same evaluateWhen the registry uses,
 * so a key and a command share ONE applicability model.
 */
export class Keymap {
    readonly #defaults: readonly Keybinding[];
    #user: readonly Keybinding[] = [];

    constructor(defaults: readonly Keybinding[] = DEFAULT_KEYBINDINGS) {
        this.#defaults = defaults;
    }

    /**
     * Apply (or clear) the user override — the hot-reload entry point.
     *
     * `null` clears the override back to the defaults. A string is schema-validated; on success it
     * becomes the effective override (a HOT RELOAD is just this call with fresh text), and on failure
     * the PREVIOUS override is kept and the diagnostic is returned — a malformed file never leaves the
     * editor with a broken or empty keymap.
     */
    applyUserOverride(text: string | null): KeymapApplyResult {
        if (text === null) {
            this.#user = [];
            return { applied: true, cleared: true, diagnostic: "", userBindingCount: 0 };
        }
        const result = parseUserKeybindings(text);
        if (!result.ok) {
            return {
                applied: false,
                cleared: false,
                diagnostic: result.diagnostic,
                userBindingCount: this.#user.length,
            };
        }
        this.#user = result.override.bindings;
        return {
            applied: true,
            cleared: false,
            diagnostic: "",
            userBindingCount: this.#user.length,
        };
    }

    /** The effective bindings, defaults-first then user (i.e. ascending precedence). */
    effectiveBindings(): readonly Keybinding[] {
        return [...this.#defaults, ...this.#user];
    }

    /**
     * Resolve a canonical chord to a command id in the given context, or `undefined`.
     *
     * `undefined` means "nothing is bound here for this context" — the input path then falls the key
     * through to CEF (03 §6). An explicit unbind (`command: ""`) also resolves to `undefined`.
     */
    resolveCommand(chord: string, context: WhenContext): string | undefined {
        const bindings = this.effectiveBindings();
        for (let index = bindings.length - 1; index >= 0; index -= 1) {
            const candidate = bindings[index];
            if (candidate === undefined || candidate.key !== chord) {
                continue;
            }
            if (!evaluateWhen(candidate.when, context)) {
                continue;
            }
            return candidate.command === "" ? undefined : candidate.command;
        }
        return undefined;
    }

    /** Resolve a live keystroke to a command id, or `undefined`. A non-chord keystroke resolves to `undefined`. */
    resolveStroke(stroke: KeyStroke, context: WhenContext): string | undefined {
        const chord = keyStrokeToChord(stroke);
        return chord === null ? undefined : this.resolveCommand(chord, context);
    }
}

// ------------------------------------------------------------------------------- the Shell bridge feed
//
// The Shell publishes the user override over the e05c bridge. editor-core PULLS it: the Shell owns the
// read/watch/hot-reload of the file (keybindings_bridge.h), and hands back the raw text plus a
// GENERATION counter it bumps on every observed change. editor-core re-applies only when the
// generation moves — so a poll is cheap (a counter compare) and the expensive file IO happens once
// per change, Shell-side.

/** The `keybindings.get` result the Shell serves. `text` is meaningful only when `present`. */
export interface KeybindingsSnapshot {
    /** Whether `~/.context/keybindings.json` exists. When false, defaults stand and `text` is `""`. */
    readonly present: boolean;
    /** Bumped by the Shell's watch on every observed change — the hot-reload trigger. */
    readonly generation: number;
    /** The raw file bytes (unvalidated — editor-core owns the schema). `""` when absent. */
    readonly text: string;
}

/** Parse a `keybindings.get` result, total against a malformed or partial envelope. */
export function parseKeybindingsSnapshot(value: unknown): KeybindingsSnapshot {
    if (!isRecord(value)) {
        return { present: false, generation: 0, text: "" };
    }
    const present = value["present"] === true;
    const generation = typeof value["generation"] === "number" ? value["generation"] : 0;
    const text = typeof value["text"] === "string" ? value["text"] : "";
    return { present, generation, text };
}

/** A source of override snapshots — the seam the controller depends on, so a test injects a fake. */
export interface KeybindingsSource {
    get(): Promise<KeybindingsSnapshot>;
}

/**
 * The thin typed client over the `keybindings.get` bridge method.
 *
 * Holds no state — the override file lives in the Shell (D18), so a cache here would be a second
 * source of truth free to disagree with it. A bridge refusal is returned as an ABSENT snapshot rather
 * than thrown: a Shell that does not serve the method (an older build, or the smoke's minimal router)
 * must degrade to the defaults, not break editor-core's boot.
 */
export class KeybindingsClient implements KeybindingsSource {
    readonly #bridge: ShellBridge;

    constructor(bridge: ShellBridge) {
        this.#bridge = bridge;
    }

    /** Read the current override snapshot. An absent snapshot on any bridge refusal (defaults stand). */
    async get(): Promise<KeybindingsSnapshot> {
        try {
            return parseKeybindingsSnapshot(await this.#bridge.call(KEYBINDINGS_GET_METHOD));
        } catch (error) {
            if (error instanceof BridgeError) {
                return { present: false, generation: 0, text: "" };
            }
            throw error;
        }
    }
}

/** What a `KeymapController` refresh did. */
export interface KeymapRefreshResult {
    /** True when the override changed since the last refresh and was re-applied (a hot reload). */
    readonly reloaded: boolean;
    /** The apply outcome when a reload happened; `undefined` when the generation was unchanged. */
    readonly apply: KeymapApplyResult | undefined;
    /** The generation the controller is now tracking. */
    readonly generation: number;
}

/**
 * Ties the keymap to the registry and the Shell feed: fetch the override, hot-reload on a generation
 * change, and dispatch a keystroke to its command.
 *
 * This is what boot.ts wires and the input path drives. The GENERATION is the hot-reload pivot: the
 * Shell's watch bumps it, `refresh()` re-applies only when it moves, and a re-applied malformed file
 * is refused (the previous map stands) — the same fail-closed contract `applyUserOverride` gives,
 * surfaced per refresh.
 */
export class KeymapController {
    readonly #keymap: Keymap;
    readonly #registry: CommandRegistry;
    readonly #source: KeybindingsSource;
    // -1 (never a real generation) forces the FIRST snapshot to apply, even for an absent override, so
    // the controller starts from a known baseline rather than assuming generation 0 == "nothing yet".
    #lastGeneration = -1;

    constructor(keymap: Keymap, registry: CommandRegistry, source: KeybindingsSource) {
        this.#keymap = keymap;
        this.#registry = registry;
        this.#source = source;
    }

    /** The generation last applied (`-1` before the first refresh). */
    get generation(): number {
        return this.#lastGeneration;
    }

    /**
     * Apply a snapshot, re-applying the override only when its generation moved (the hot-reload pivot).
     *
     * SYNCHRONOUS and pure over the snapshot so the generation-gating is directly testable. The FIRST
     * call always applies (from the `-1` baseline); later calls are a cheap counter compare and
     * re-apply only on a real change. An absent snapshot clears the override to the defaults.
     */
    applySnapshot(snapshot: KeybindingsSnapshot): KeymapRefreshResult {
        if (snapshot.generation === this.#lastGeneration) {
            return { reloaded: false, apply: undefined, generation: this.#lastGeneration };
        }
        const apply = this.#keymap.applyUserOverride(snapshot.present ? snapshot.text : null);
        this.#lastGeneration = snapshot.generation;
        return { reloaded: true, apply, generation: this.#lastGeneration };
    }

    /** Fetch the current override and re-apply it when its generation moved (async wrapper over applySnapshot). */
    async refresh(): Promise<KeymapRefreshResult> {
        return this.applySnapshot(await this.#source.get());
    }

    /**
     * Resolve a keystroke and execute the bound command, or return `undefined` when nothing is bound.
     *
     * `undefined` is the "fall the key through to CEF" signal (03 §6). A bound command's outcome is
     * returned verbatim — including an `ok: false` refusal, which is an ordinary result, not an error.
     */
    async dispatch(stroke: KeyStroke, context: WhenContext): Promise<CommandOutcome | undefined> {
        const command = this.#keymap.resolveStroke(stroke, context);
        if (command === undefined) {
            return undefined;
        }
        return this.#registry.execute(command);
    }
}
