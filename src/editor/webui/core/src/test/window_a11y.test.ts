// T1 tests for the KEYBOARD-ONLY multi-window path (M9 e10d, R-A11Y-001, design 05 §6 / 03 §6 / 04 §2).
//
// THE STANDING LESSON THIS FILE EXISTS FOR. "Has a command registered" is NOT "is keyboard
// reachable" — so these cases do NOT assert that a command EXISTS, nor merely that a chord RESOLVES
// to an id. They DRIVE a live keystroke through the SAME `KeymapController.dispatch` path the input
// pump drives, and assert the bound action actually FIRED — which the pure keymap.test.ts cases
// could not, because that tier is synchronous and `dispatch` (registry.execute) is async (keymap.test.ts
// says so explicitly and defers execution "to the smoke"). The harness awaits each case (harness.ts),
// so the execution half is provable here without a browser.
//
// The three multi-window surfaces e10c gives a MOUSE user (tear-out, move-to-window-N, dock-zone
// move) must all be operable WITHOUT one, inside persona A's ≤4-key budget (05 §6). Each case below:
//   1. drives the DEFAULT binding's live keystroke and asserts the action fired (reachable + operable);
//   2. asserts the chord is within the ≤4-key budget (the physical keys pressed at once);
//   3. asserts the negative guards — no panel focused, or a text field focused — so the path is a
//      real when-gated binding, not a global that would fire mid-typing (03 §6 keyboard routing).

import { assert, assertEqual, type TestCase } from "./harness.js";
import { Keymap, KeymapController, keyStrokeToChord, type KeyStroke } from "../keymap.js";
import { buildCommandRegistry, type CommandOutcome, type DockDirection } from "../commands.js";
import type { WhenContext } from "../when.js";

/** persona A's simultaneous-key budget (05 §6): a chord may press at most this many keys at once. */
const PERSONA_A_KEY_BUDGET = 4;

/** A when-context with a panel focused and no text input — where the multi-window bindings are live. */
function panelFocused(): WhenContext {
    return { panelFocus: "builtin.inspector", textInputFocus: false };
}

/** A recording harness: a real command registry over spy editor actions, plus the calls it saw. */
function harness(): {
    controller: KeymapController;
    calls: string[];
} {
    const calls: string[] = [];
    const record = (tag: string): CommandOutcome => {
        calls.push(tag);
        return { ok: true, note: tag };
    };
    const registry = buildCommandRegistry({
        contractDispatch: (method) => ({ ok: true, note: method }),
        editorActions: {
            focusNextPanel: () => record("focusNext"),
            focusPreviousPanel: () => record("focusPrevious"),
            moveActivePanel: (d: DockDirection) => record(`move:${d}`),
            closeActivePanel: () => record("close"),
            toggleTheme: () => record("theme"),
            tearOutActivePanel: () => record("tearOut"),
            movePanelToPrimary: () => record("moveToPrimary"),
        },
        sessionActions: { undo: () => record("undo"), redo: () => record("redo") },
        roster: { contractMajor: 2, panels: [] },
        panelDispatch: (panelId, commandId) => record(`${panelId}/${commandId}`),
    });
    // The controller drives the DEFAULT keymap; no user override is needed (effectiveBindings starts
    // as the defaults), so dispatch resolves + executes straight off the shipped map.
    const controller = new KeymapController(new Keymap(), registry, {
        get: () => Promise.resolve({ present: false, generation: 0, text: "" }),
    });
    return { controller, calls };
}

/** The physical keys a chord presses at once — its budget cost (`Ctrl+Shift+N` = 3). */
function chordKeyCount(stroke: KeyStroke): number {
    const chord = keyStrokeToChord(stroke);
    assert(chord !== null, `expected a real chord for ${JSON.stringify(stroke)}`);
    return (chord as string).split("+").length;
}

/** Drive one keystroke and return which spy tag (if any) it executed. */
async function driveAndTag(
    controller: KeymapController,
    calls: string[],
    stroke: KeyStroke,
    context: WhenContext,
): Promise<string | undefined> {
    const before = calls.length;
    const outcome = await controller.dispatch(stroke, context);
    if (outcome === undefined) {
        return undefined; // nothing bound in this context — the key falls through to CEF (03 §6)
    }
    assert(calls.length === before + 1, "a dispatched command executed exactly one action");
    return calls[calls.length - 1];
}

export const windowA11yTests: readonly TestCase[] = [
    {
        name: "keyboard-only: tear-out fires from a live Ctrl+Shift+N within the ≤4-key budget",
        run: async () => {
            const { controller, calls } = harness();
            const stroke: KeyStroke = { key: "N", ctrlKey: true, shiftKey: true };
            assert(chordKeyCount(stroke) <= PERSONA_A_KEY_BUDGET, "Ctrl+Shift+N is within budget");
            // DRIVEN, not resolved: the tear-out ACTION must actually run from the keystroke.
            const tag = await driveAndTag(controller, calls, stroke, panelFocused());
            assertEqual(tag, "tearOut", "Ctrl+Shift+N executes the tear-out action");
        },
    },
    {
        name: "keyboard-only: move-to-window-N (primary) fires from Ctrl+Shift+M within budget",
        run: async () => {
            const { controller, calls } = harness();
            const stroke: KeyStroke = { key: "M", ctrlKey: true, shiftKey: true };
            assert(chordKeyCount(stroke) <= PERSONA_A_KEY_BUDGET, "Ctrl+Shift+M is within budget");
            const tag = await driveAndTag(controller, calls, stroke, panelFocused());
            assertEqual(tag, "moveToPrimary", "Ctrl+Shift+M executes the move-to-window-N action");
        },
    },
    {
        name: "keyboard-only: every dock-zone move fires from its Alt+Arrow chord within budget",
        run: async () => {
            const cases: readonly { stroke: KeyStroke; direction: DockDirection }[] = [
                { stroke: { key: "ArrowLeft", altKey: true }, direction: "left" },
                { stroke: { key: "ArrowRight", altKey: true }, direction: "right" },
                { stroke: { key: "ArrowUp", altKey: true }, direction: "up" },
                { stroke: { key: "ArrowDown", altKey: true }, direction: "down" },
            ];
            for (const { stroke, direction } of cases) {
                const { controller, calls } = harness();
                assert(
                    chordKeyCount(stroke) <= PERSONA_A_KEY_BUDGET,
                    `Alt+${direction} arrow is within budget`,
                );
                const tag = await driveAndTag(controller, calls, stroke, panelFocused());
                assertEqual(tag, `move:${direction}`, `the ${direction} dock-zone move fires from keys`);
            }
        },
    },
    {
        name: "keyboard-only: the guards are real — no panel / a text field suppresses the bindings",
        run: async () => {
            const { controller, calls } = harness();
            // With NO panel focused, the panelFocus-guarded bindings must NOT fire (a real when-gate,
            // not a global) — this is what a single "the command is registered" check would miss.
            assertEqual(
                await driveAndTag(controller, calls, { key: "N", ctrlKey: true, shiftKey: true }, {
                    panelFocus: "",
                    textInputFocus: false,
                }),
                undefined,
                "tear-out does not fire with no panel focused",
            );
            assertEqual(
                await driveAndTag(controller, calls, { key: "ArrowLeft", altKey: true }, {
                    panelFocus: "",
                    textInputFocus: false,
                }),
                undefined,
                "a dock-zone move does not fire with no panel focused",
            );
            // While typing in a DOM editable the same keys belong to CEF (03 §6): they must not fire.
            assertEqual(
                await driveAndTag(controller, calls, { key: "M", ctrlKey: true, shiftKey: true }, {
                    panelFocus: "builtin.inspector",
                    textInputFocus: true,
                }),
                undefined,
                "move-to-window-N does not fire while a text field has focus",
            );
            assert(calls.length === 0, "no guarded action fired in any suppressed context");
        },
    },
];
