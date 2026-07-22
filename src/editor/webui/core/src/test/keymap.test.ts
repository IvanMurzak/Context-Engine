// T1 unit tests for the keymap (M9 e07c, design 05 §6 / 03 §6). The DoD properties this tier pins:
// the DEFAULT keymap resolves its bindings (incl. the headline undo/redo pair); a USER-FILE OVERRIDE
// is schema-validated and merged; HOT RELOAD is re-applying a fresh payload; a malformed override is
// REJECTED with a diagnostic and never silently applied; and WHEN-CONTEXT binding resolution honours
// the same evaluator commands use (a guarded binding drops when its context is inactive).
//
// All synchronous, per the shared harness (harness.ts runs each case with no await). The keymap's
// pure logic — chord canonicalization, schema validation, merge/precedence, the resolver, and the
// controller's generation-gated hot-reload DECISION (applySnapshot) — is fully covered here; the
// async bridge fetch (KeybindingsClient) and command execution are the thin wrappers the CEF smoke
// exercises end to end.

import { assert, assertEqual, assertNull, type TestCase } from "./harness.js";
import {
    canonicalizeChord,
    DEFAULT_KEYBINDINGS,
    KEYBINDINGS_GET_METHOD,
    KEYBINDINGS_SCHEMA_ID,
    KEYBINDINGS_SCHEMA_VERSION,
    Keymap,
    KeymapController,
    keyStrokeToChord,
    parseKeybindingsSnapshot,
    parseUserKeybindings,
} from "../keymap.js";
import type { KeybindingsSnapshot } from "../keymap.js";
import { buildCommandRegistry, CommandRegistry } from "../commands.js";
import type { WhenContext } from "../when.js";

// A context with nothing focused and no text input — the baseline the default undo/redo resolve in.
const NO_TEXT: WhenContext = { textInputFocus: false };

/** Build a valid override document naming the current schema. */
function overrideDoc(bindings: readonly Record<string, unknown>[]): string {
    return JSON.stringify({
        $schema: KEYBINDINGS_SCHEMA_ID,
        version: KEYBINDINGS_SCHEMA_VERSION,
        bindings,
    });
}

/** A snapshot as the Shell's `keybindings.get` serves it. */
function snapshot(present: boolean, generation: number, text: string): KeybindingsSnapshot {
    return { present, generation, text };
}

export const keymapTests: readonly TestCase[] = [
    // ------------------------------------------------------------- chord canonicalization
    {
        name: "canonicalizeChord: normalizes case, modifier order, and rejects non-chords",
        run: () => {
            assertEqual(canonicalizeChord("ctrl+z"), "Ctrl+Z", "lowercase modifier + key");
            assertEqual(canonicalizeChord("Control+Z"), "Ctrl+Z", "Control alias, key already upper");
            assertEqual(canonicalizeChord("shift+ctrl+z"), "Ctrl+Shift+Z", "modifier order is fixed");
            assertEqual(canonicalizeChord("Alt+ArrowLeft"), "Alt+ArrowLeft", "named key kept verbatim");
            assertEqual(canonicalizeChord("cmd+k"), "Meta+K", "cmd is the Meta alias");
            assertNull(canonicalizeChord("ctrl"), "a modifier with no key is not a chord");
            assertNull(canonicalizeChord(""), "an empty chord is rejected");
            assertNull(canonicalizeChord("a+b"), "two non-modifier keys is rejected");
        },
    },
    {
        name: "keyStrokeToChord: reduces a live KeyboardEvent to the same canonical form",
        run: () => {
            assertEqual(keyStrokeToChord({ key: "z", ctrlKey: true }), "Ctrl+Z", "ctrl+z");
            assertEqual(
                keyStrokeToChord({ key: "Z", ctrlKey: true, shiftKey: true }),
                "Ctrl+Shift+Z",
                "ctrl+shift+z (shift both mod and upper-case)",
            );
            assertEqual(
                keyStrokeToChord({ key: "ArrowLeft", altKey: true }),
                "Alt+ArrowLeft",
                "a named key with a modifier",
            );
            assertNull(
                keyStrokeToChord({ key: "Control", ctrlKey: true }),
                "a bare modifier press is not a chord",
            );
            assertNull(keyStrokeToChord({ key: "" }), "an empty key is not a chord");
        },
    },

    // ------------------------------------------------------------- the default keymap (undo/redo)
    {
        name: "default keymap: Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z bind session.undo / session.redo",
        run: () => {
            const keymap = new Keymap();
            assertEqual(keymap.resolveCommand("Ctrl+Z", NO_TEXT), "session.undo", "Ctrl+Z -> undo");
            assertEqual(keymap.resolveCommand("Ctrl+Y", NO_TEXT), "session.redo", "Ctrl+Y -> redo");
            assertEqual(
                keymap.resolveCommand("Ctrl+Shift+Z", NO_TEXT),
                "session.redo",
                "Ctrl+Shift+Z -> redo (the conventional second redo)",
            );
            // And through a live keystroke, the path the input pump actually drives.
            assertEqual(
                keymap.resolveStroke({ key: "z", ctrlKey: true }, NO_TEXT),
                "session.undo",
                "a live Ctrl+Z keystroke resolves to session.undo",
            );
            // The default map really does declare the undo/redo pair (guards against a silent drop).
            const ids = DEFAULT_KEYBINDINGS.map((b) => b.command);
            assert(ids.includes("session.undo") && ids.includes("session.redo"), "both are shipped");
        },
    },

    // ------------------------------------------------------------- when-context binding resolution
    {
        name: "when-context resolution: a guard drops a binding when its context is inactive",
        run: () => {
            const keymap = new Keymap();
            // Ctrl+Z is guarded `!textInputFocus`: while a DOM editable has focus it does NOT fire
            // (03 §6 — the key belongs to CEF), so the session undo must not resolve.
            assertNullish(
                keymap.resolveCommand("Ctrl+Z", { textInputFocus: true }),
                "Ctrl+Z does not resolve while typing in a text field",
            );
            assertEqual(
                keymap.resolveCommand("Ctrl+Z", { textInputFocus: false }),
                "session.undo",
                "Ctrl+Z resolves when no text input has focus",
            );
            // Alt+ArrowLeft is guarded `panelFocus && !textInputFocus`: it needs a focused panel.
            assertEqual(
                keymap.resolveCommand("Alt+ArrowLeft", {
                    panelFocus: "builtin.problems",
                    textInputFocus: false,
                }),
                "view.panel.move.left",
                "the move binding resolves with a panel focused",
            );
            assertNullish(
                keymap.resolveCommand("Alt+ArrowLeft", { panelFocus: "", textInputFocus: false }),
                "the move binding drops with no panel focused",
            );
        },
    },

    // ------------------------------------------------------------- user override: parse + validate
    {
        name: "parseUserKeybindings: a valid override parses + canonicalizes its chords",
        run: () => {
            const result = parseUserKeybindings(
                overrideDoc([{ key: "ctrl+b", command: "view.theme.toggle" }]),
            );
            assert(result.ok, "a well-formed override parses");
            if (result.ok) {
                assertEqual(result.override.bindings.length, 1, "one binding");
                assertEqual(result.override.bindings[0]?.key, "Ctrl+B", "the chord is canonicalized");
                assertEqual(result.override.bindings[0]?.when, "", "an absent when defaults to empty");
            }
        },
    },
    {
        name: "parseUserKeybindings: schema rejection — bad JSON, wrong $schema/version, bad shape",
        run: () => {
            assertRejected(parseUserKeybindings("{ not json"), "not valid JSON", "malformed JSON");
            assertRejected(
                parseUserKeybindings(
                    JSON.stringify({ version: KEYBINDINGS_SCHEMA_VERSION, bindings: [] }),
                ),
                "$schema",
                "a missing $schema",
            );
            assertRejected(
                parseUserKeybindings(
                    JSON.stringify({ $schema: KEYBINDINGS_SCHEMA_ID, version: 99, bindings: [] }),
                ),
                "version",
                "an incompatible version",
            );
            assertRejected(
                parseUserKeybindings(
                    JSON.stringify({
                        $schema: KEYBINDINGS_SCHEMA_ID,
                        version: KEYBINDINGS_SCHEMA_VERSION,
                        bindings: {},
                    }),
                ),
                "must be an array",
                "a non-array bindings",
            );
            assertRejected(
                parseUserKeybindings(overrideDoc([{ key: "", command: "view.theme.toggle" }])),
                "non-empty string",
                "an empty key",
            );
            assertRejected(
                parseUserKeybindings(overrideDoc([{ key: "ctrl", command: "view.theme.toggle" }])),
                "not a valid chord",
                "a modifier-only chord",
            );
            assertRejected(
                parseUserKeybindings(overrideDoc([{ key: "Ctrl+B", command: 42 }])),
                "must be a string",
                "a non-string command",
            );
        },
    },

    // ------------------------------------------------------------- override merge + precedence
    {
        name: "Keymap.applyUserOverride: a user binding overrides a default, and '' unbinds",
        run: () => {
            const keymap = new Keymap();
            // A user binding for Ctrl+Z outranks the default session.undo.
            const applied = keymap.applyUserOverride(
                overrideDoc([{ key: "Ctrl+Z", command: "view.theme.toggle" }]),
            );
            assert(applied.applied, "the override applied");
            assertEqual(
                keymap.resolveCommand("Ctrl+Z", NO_TEXT),
                "view.theme.toggle",
                "the user binding outranks the default undo",
            );
            // Ctrl+Y is untouched by the override — the default redo still resolves.
            assertEqual(keymap.resolveCommand("Ctrl+Y", NO_TEXT), "session.redo", "defaults persist");
            // An explicit unbind ("") masks the default.
            keymap.applyUserOverride(overrideDoc([{ key: "Ctrl+Y", command: "" }]));
            assertNullish(
                keymap.resolveCommand("Ctrl+Y", NO_TEXT),
                "an empty-command override unbinds the chord",
            );
        },
    },
    {
        name: "Keymap.applyUserOverride: hot reload — re-applying a fresh payload swaps the map; null clears",
        run: () => {
            const keymap = new Keymap();
            keymap.applyUserOverride(overrideDoc([{ key: "Ctrl+Z", command: "view.theme.toggle" }]));
            assertEqual(keymap.resolveCommand("Ctrl+Z", NO_TEXT), "view.theme.toggle", "first payload");
            // A HOT RELOAD is exactly this: re-apply a fresh payload over the same Keymap.
            keymap.applyUserOverride(overrideDoc([{ key: "Ctrl+Z", command: "view.panel.close" }]));
            assertEqual(
                keymap.resolveCommand("Ctrl+Z", { panelFocus: "p", textInputFocus: false }),
                "view.panel.close",
                "the second payload replaces the first (hot reload)",
            );
            // Clearing (null) reverts to the defaults.
            const cleared = keymap.applyUserOverride(null);
            assert(cleared.cleared, "a null payload clears the override");
            assertEqual(keymap.resolveCommand("Ctrl+Z", NO_TEXT), "session.undo", "back to defaults");
        },
    },
    {
        name: "Keymap.applyUserOverride: a REJECTED reload keeps the previous map (never silently applied)",
        run: () => {
            const keymap = new Keymap();
            keymap.applyUserOverride(overrideDoc([{ key: "Ctrl+Z", command: "view.theme.toggle" }]));
            const rejected = keymap.applyUserOverride("{ not json");
            assert(!rejected.applied, "the malformed reload was rejected");
            assert(rejected.diagnostic.length > 0, "a diagnostic is returned");
            assertEqual(
                keymap.resolveCommand("Ctrl+Z", NO_TEXT),
                "view.theme.toggle",
                "the PREVIOUS override stands — a bad file never breaks the keymap",
            );
        },
    },

    // ------------------------------------------------------------- the Shell snapshot + controller
    {
        name: "parseKeybindingsSnapshot: total against a malformed or partial envelope",
        run: () => {
            const ok = parseKeybindingsSnapshot({ present: true, generation: 3, text: "x" });
            assertEqual(ok.present, true, "present");
            assertEqual(ok.generation, 3, "generation");
            assertEqual(ok.text, "x", "text");
            const empty = parseKeybindingsSnapshot(null);
            assertEqual(empty.present, false, "a non-object reads absent");
            assertEqual(empty.generation, 0, "generation defaults to 0");
            assertEqual(empty.text, "", "text defaults to empty");
            // The wire method name mirrors the C++ constant (webui-panel-contract gate).
            assertEqual(KEYBINDINGS_GET_METHOD, "keybindings.get", "the wire method name is stable");
        },
    },
    {
        name: "KeymapController.applySnapshot: re-applies only when the generation moves (hot-reload gate)",
        run: () => {
            const keymap = new Keymap();
            const registry = new CommandRegistry();
            // A stub source; applySnapshot is driven directly, so the source is never called here.
            const controller = new KeymapController(keymap, registry, {
                get: () => Promise.resolve(snapshot(false, 0, "")),
            });

            const first = controller.applySnapshot(
                snapshot(true, 1, overrideDoc([{ key: "Ctrl+Z", command: "view.theme.toggle" }])),
            );
            assert(first.reloaded, "the first snapshot applies (from the -1 baseline)");
            assertEqual(controller.generation, 1, "the controller tracks generation 1");
            assertEqual(
                keymap.resolveCommand("Ctrl+Z", NO_TEXT),
                "view.theme.toggle",
                "the override took effect",
            );

            // Same generation -> a cheap no-op, no re-apply.
            const same = controller.applySnapshot(snapshot(true, 1, overrideDoc([])));
            assert(!same.reloaded, "an unchanged generation does not reload");
            assertEqual(
                keymap.resolveCommand("Ctrl+Z", NO_TEXT),
                "view.theme.toggle",
                "the map is untouched when the generation is unchanged",
            );

            // A bumped generation -> a hot reload to the new payload.
            const next = controller.applySnapshot(
                snapshot(true, 2, overrideDoc([{ key: "Ctrl+Z", command: "view.panel.close" }])),
            );
            assert(next.reloaded, "a bumped generation reloads");
            assertEqual(
                keymap.resolveCommand("Ctrl+Z", { panelFocus: "p", textInputFocus: false }),
                "view.panel.close",
                "the new payload is live",
            );

            // An absent file at a new generation clears the override to the defaults.
            controller.applySnapshot(snapshot(false, 3, ""));
            assertEqual(keymap.resolveCommand("Ctrl+Z", NO_TEXT), "session.undo", "cleared to defaults");
        },
    },
    {
        name: "KeymapController: a real registry executes the command a keystroke resolves to",
        run: () => {
            const calls: string[] = [];
            const record = (tag: string) => {
                calls.push(tag);
                return { ok: true, note: tag };
            };
            const registry = buildCommandRegistry({
                contractDispatch: (method) => ({ ok: true, note: method }),
                editorActions: {
                    focusNextPanel: () => record("focusNext"),
                    focusPreviousPanel: () => record("focusPrevious"),
                    moveActivePanel: (d) => record(`move:${d}`),
                    closeActivePanel: () => record("close"),
                    toggleTheme: () => record("theme"),
                },
                sessionActions: { undo: () => record("undo"), redo: () => record("redo") },
                roster: { contractMajor: 2, panels: [] },
                panelDispatch: (panelId, commandId) => record(`${panelId}/${commandId}`),
            });
            const keymap = new Keymap();
            // Resolution is synchronous and is the load-bearing half; assert it directly (dispatch's
            // execute() is async, so the sync harness cannot await it — the smoke covers execution).
            assertEqual(
                keymap.resolveStroke({ key: "z", ctrlKey: true }, NO_TEXT),
                "session.undo",
                "a Ctrl+Z keystroke resolves to the registered session.undo command",
            );
            assert(registry.has("session.undo"), "the resolved id is a real registry command");
            assertNullish(
                keymap.resolveStroke({ key: "q", ctrlKey: true }, NO_TEXT),
                "an unbound keystroke resolves to nothing (falls through to CEF)",
            );
        },
    },
];

// --------------------------------------------------------------------------- local assert helpers

/** Assert a value is `undefined` (the resolver's "nothing bound" signal). */
function assertNullish(actual: unknown, message: string): void {
    assert(actual === undefined, `${message}: expected undefined, got ${JSON.stringify(actual)}`);
}

/** Assert a parse result is a rejection whose diagnostic mentions `needle`. */
function assertRejected(
    result: ReturnType<typeof parseUserKeybindings>,
    needle: string,
    message: string,
): void {
    assert(!result.ok, `${message}: expected a rejection`);
    if (!result.ok) {
        assert(
            result.diagnostic.includes(needle),
            `${message}: diagnostic ${JSON.stringify(result.diagnostic)} should mention ${JSON.stringify(needle)}`,
        );
    }
}
