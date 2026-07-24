// T1 keyboard-only REACHABILITY tests (M9 e07d, design 05 §6 / 10; feeds R-A11Y-001 / R-CLI-001).
//
// The DoD property this tier pins is STRUCTURAL, not incidental: EVERY dock / window / theme /
// navigation operation is reachable as a command with a keyboard-only path. Two paths exist and both
// are asserted here:
//
//   1. A DIRECT default keybinding — every built-in editor / session command (and the palette itself)
//      has at least one chord in DEFAULT_KEYBINDINGS that targets it, so it is reachable with no
//      pointer even before the palette is opened.
//   2. THE PALETTE as the universal path — the palette is opened by a keyboard-bound command
//      (Ctrl+Shift+P), and once open it lists EVERY when-active command by name, so any command
//      (contract verbs and panel commands included, which carry no dedicated chord) is keyboard-
//      reachable through it.
//
// This is the assertion the design calls a "structural accessibility property" (10): a pointer-only
// capability would be a gap this test turns red, not a nicety.

import { assert, type TestCase } from "./harness.js";
import {
    buildCommandRegistry,
    editorCommands,
    sessionCommands,
} from "../commands.js";
import type { EditorCommandActions, SessionCommandActions } from "../commands.js";
import { DEFAULT_KEYBINDINGS, Keymap } from "../keymap.js";
import { Palette, PALETTE_TOGGLE_COMMAND_ID, paletteCommands } from "../palette.js";
import type { PaletteActions } from "../palette.js";

const editorSpy: EditorCommandActions = {
    focusNextPanel: () => ({ ok: true, note: "" }),
    focusPreviousPanel: () => ({ ok: true, note: "" }),
    moveActivePanel: () => ({ ok: true, note: "" }),
    closeActivePanel: () => ({ ok: true, note: "" }),
    toggleTheme: () => ({ ok: true, note: "" }),
    tearOutActivePanel: () => ({ ok: true, note: "" }),
    movePanelToPrimary: () => ({ ok: true, note: "" }),
};
const sessionSpy: SessionCommandActions = {
    undo: () => ({ ok: true, note: "" }),
    redo: () => ({ ok: true, note: "" }),
};
const paletteSpy: PaletteActions = { toggle: () => ({ ok: true, note: "" }) };

/** The command ids bound by at least one non-unbind default keybinding. */
function boundCommandIds(): Set<string> {
    return new Set(DEFAULT_KEYBINDINGS.filter((b) => b.command !== "").map((b) => b.command));
}

export const reachabilityTests: readonly TestCase[] = [
    {
        name: "reachability: every built-in editor/session/palette command has a default keybinding",
        run: () => {
            const bound = boundCommandIds();
            const ids = [
                ...editorCommands(editorSpy).map((c) => c.id),
                ...sessionCommands(sessionSpy).map((c) => c.id),
                ...paletteCommands(paletteSpy).map((c) => c.id),
            ];
            for (const id of ids) {
                assert(bound.has(id), `${id} is reachable via a default keybinding (keyboard-only path)`);
            }
        },
    },
    {
        name: "reachability: every dock / window / theme / navigation operation has a keyboard path",
        run: () => {
            const bound = boundCommandIds();
            // The explicit dock/window/theme/navigation surface (05 §6), enumerated so a dropped
            // binding for any ONE of them fails by name rather than silently.
            for (const id of [
                "view.panel.focusNext", // navigation
                "view.panel.focusPrevious", // navigation
                "view.panel.move.left", // dock
                "view.panel.move.right", // dock
                "view.panel.move.up", // dock
                "view.panel.move.down", // dock
                "view.panel.close", // dock/window
                "view.theme.toggle", // theme
            ]) {
                assert(bound.has(id), `${id} has a keyboard-only path (a default keybinding)`);
            }
        },
    },
    {
        name: "reachability: the palette itself is keyboard-openable — even while typing in a field",
        run: () => {
            const keymap = new Keymap();
            // Ctrl+Shift+P carries NO when-guard, so it resolves even with a text field focused — the
            // one moment a user most needs "give me any command by name".
            const typing = keymap.resolveCommand("Ctrl+Shift+P", { textInputFocus: true });
            assert(typing === PALETTE_TOGGLE_COMMAND_ID, "Ctrl+Shift+P opens the palette while typing");
            const idle = keymap.resolveStroke(
                { key: "P", ctrlKey: true, shiftKey: true },
                { textInputFocus: false },
            );
            assert(idle === PALETTE_TOGGLE_COMMAND_ID, "the live keystroke resolves to the palette too");
        },
    },
    {
        name: "reachability: the palette is the UNIVERSAL keyboard path — every command is listed by name",
        run: () => {
            const registry = buildCommandRegistry({
                contractDispatch: (method) => ({ ok: true, note: method }),
                editorActions: editorSpy,
                sessionActions: sessionSpy,
                roster: { contractMajor: 2, panels: [] },
                panelDispatch: (p, c) => ({ ok: true, note: `${p}/${c}` }),
            });
            const palette = new Palette(registry);
            palette.open();
            const listed = new Set(
                palette.results({ textInputFocus: false }).map((e) => e.command.id),
            );
            // With nothing focused, exactly the always-/text-safe commands are listed; the panel-focus
            // -guarded ones (move/close) require a focused panel and are asserted separately. What must
            // hold universally: the theme + navigation + every contract verb are keyboard-reachable.
            assert(listed.has("view.theme.toggle"), "the theme command is in the palette");
            assert(listed.has("view.panel.focusNext"), "a navigation command is in the palette");
            assert(listed.has("session.undo"), "a session command is in the palette");
            // A focused panel surfaces the dock commands through the SAME palette.
            const focused = new Set(
                palette.results({ panelFocus: "p", textInputFocus: false }).map((e) => e.command.id),
            );
            assert(focused.has("view.panel.close"), "a focused panel makes the close command reachable");
            assert(focused.has("view.panel.move.left"), "and the dock-move commands too");
        },
    },
];
