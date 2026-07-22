// T1 unit tests for the when-context evaluator (M9 e07b, design 05 §6 / 03 §6). Two properties this
// tier pins: (1) the SIX contexts resolve from their two sources with the fixed scope precedence
// (text-input > focused panel > window > global) — the resolution-order matrix; and (2) the clause
// evaluator is TOTAL and FAIL-CLOSED (empty = always, malformed = inactive, never a throw). It also
// pins the e08 seam: swapping the session-state stub changes ONLY `playState`.

import { assert, assertEqual, type TestCase } from "./harness.js";
import {
    evaluateWhen,
    resolveContext,
    resolveWhenContext,
    sourcesToLayers,
    STUB_EDITOR_UI,
    STUB_SESSION_STATE,
    WHEN_CONTEXT_KEYS,
} from "../when.js";
import type { EditorUiSource, SessionStateSource, WhenContext } from "../when.js";

const FULL_UI: EditorUiSource = {
    textInputFocus: true,
    panelFocus: "builtin.inspector",
    panelType: "inspector",
    windowType: "main",
    viewportMode: "3d",
};

export const whenTests: readonly TestCase[] = [
    // ------------------------------------------------------------- the clause evaluator
    {
        name: "evaluateWhen: an empty or whitespace clause is always active",
        run: () => {
            assertEqual(evaluateWhen("", {}), true, "empty clause");
            assertEqual(evaluateWhen("   ", {}), true, "whitespace clause");
        },
    },
    {
        name: "evaluateWhen: a bare key is truthy for a non-empty string / true, falsy otherwise",
        run: () => {
            assertEqual(evaluateWhen("panelFocus", { panelFocus: "x" }), true, "non-empty string");
            assertEqual(evaluateWhen("panelFocus", { panelFocus: "" }), false, "empty string");
            assertEqual(evaluateWhen("panelFocus", {}), false, "absent key");
            assertEqual(evaluateWhen("textInputFocus", { textInputFocus: true }), true, "true");
            assertEqual(evaluateWhen("textInputFocus", { textInputFocus: false }), false, "false");
        },
    },
    {
        name: "evaluateWhen: negation flips a bare key",
        run: () => {
            assertEqual(evaluateWhen("!textInputFocus", { textInputFocus: false }), true, "!false");
            assertEqual(evaluateWhen("!textInputFocus", { textInputFocus: true }), false, "!true");
            assertEqual(evaluateWhen("!panelFocus", {}), true, "!absent");
        },
    },
    {
        name: "evaluateWhen: == / != compare the context value as a token (all six contexts)",
        run: () => {
            assertEqual(
                evaluateWhen("panelFocus == builtin.inspector", { panelFocus: "builtin.inspector" }),
                true,
                "panelFocus equality (dotted value)",
            );
            assertEqual(
                evaluateWhen("panelType == inspector", { panelType: "scene-tree" }),
                false,
                "panelType inequality",
            );
            assertEqual(
                evaluateWhen("viewportMode == 3d", { viewportMode: "3d" }),
                true,
                "viewportMode",
            );
            assertEqual(
                evaluateWhen("playState == playing", { playState: "playing" }),
                true,
                "playState",
            );
            assertEqual(
                evaluateWhen("windowType != main", { windowType: "welcome" }),
                true,
                "windowType inequality",
            );
            assertEqual(
                evaluateWhen("textInputFocus == true", { textInputFocus: true }),
                true,
                "a boolean context compares against its 'true'/'false' token",
            );
            assertEqual(
                evaluateWhen("panelFocus == 'quoted value'", { panelFocus: "quoted value" }),
                true,
                "a quoted RHS carries spaces",
            );
            assertEqual(
                evaluateWhen("panelFocus == inspector", {}),
                false,
                "an absent key compares as the empty token, never a match",
            );
        },
    },
    {
        name: "evaluateWhen: && / || / parentheses compose, with && binding tighter than ||",
        run: () => {
            const ctx: WhenContext = { panelFocus: "builtin.inspector", textInputFocus: false };
            assertEqual(
                evaluateWhen("panelFocus && !textInputFocus", ctx),
                true,
                "the canonical panel command guard",
            );
            assertEqual(
                evaluateWhen("panelFocus && textInputFocus", ctx),
                false,
                "&& short of the second operand",
            );
            assertEqual(
                evaluateWhen("textInputFocus || panelFocus", ctx),
                true,
                "|| true on the second operand",
            );
            // && binds tighter: `false && false || true` is `(false && false) || true` = true.
            assertEqual(
                evaluateWhen("textInputFocus && textInputFocus || panelFocus", ctx),
                true,
                "&& precedence over ||",
            );
            assertEqual(
                evaluateWhen("textInputFocus && (textInputFocus || panelFocus)", ctx),
                false,
                "parentheses override precedence",
            );
        },
    },
    {
        name: "evaluateWhen: a malformed clause is INACTIVE (fail-closed), never a throw",
        run: () => {
            assertEqual(evaluateWhen("panelFocus ==", {}), false, "dangling comparison");
            assertEqual(evaluateWhen("(panelFocus", {}), false, "unbalanced paren");
            assertEqual(evaluateWhen("panelFocus &&", {}), false, "dangling &&");
            assertEqual(evaluateWhen("panelFocus % inspector", {}), false, "unknown operator char");
            assertEqual(evaluateWhen("== inspector", {}), false, "no left operand");
            assertEqual(evaluateWhen("panelFocus 'unterminated", {}), false, "unterminated quote");
        },
    },

    // ------------------------------------------------------------- the resolution-order matrix
    {
        name: "resolveWhenContext: text-input > focused panel > window > global (the matrix)",
        run: () => {
            const all = resolveWhenContext({
                global: { panelFocus: "g" },
                window: { panelFocus: "w" },
                focusedPanel: { panelFocus: "fp" },
                textInput: { panelFocus: "ti" },
            });
            assertEqual(all.panelFocus, "ti", "text-input wins over all lower scopes");

            const noTextInput = resolveWhenContext({
                global: { panelFocus: "g" },
                window: { panelFocus: "w" },
                focusedPanel: { panelFocus: "fp" },
            });
            assertEqual(noTextInput.panelFocus, "fp", "focused panel wins when text-input is absent");

            const windowOverGlobal = resolveWhenContext({
                global: { panelFocus: "g" },
                window: { panelFocus: "w" },
            });
            assertEqual(windowOverGlobal.panelFocus, "w", "window wins over global");

            const globalOnly = resolveWhenContext({ global: { panelFocus: "g" } });
            assertEqual(globalOnly.panelFocus, "g", "global stands alone");
        },
    },
    {
        name: "resolveWhenContext: a key set only in a low scope survives when no higher scope sets it",
        run: () => {
            const resolved = resolveWhenContext({
                global: { playState: "playing" },
                textInput: { textInputFocus: true },
            });
            assertEqual(resolved.playState, "playing", "the low-scope key is not shadowed away");
            assertEqual(resolved.textInputFocus, true, "the high-scope key coexists");
        },
    },

    // ------------------------------------------------------------- the source → context wiring
    {
        name: "resolveContext: the six contexts map from editor.ui + session state into their home scopes",
        run: () => {
            const resolved = resolveContext({
                editorUi: FULL_UI,
                session: { playState: "paused" },
            });
            assertEqual(resolved.panelFocus, "builtin.inspector", "panelFocus (focused-panel scope)");
            assertEqual(resolved.panelType, "inspector", "panelType (focused-panel scope)");
            assertEqual(resolved.windowType, "main", "windowType (window scope)");
            assertEqual(resolved.viewportMode, "3d", "viewportMode (window scope)");
            assertEqual(resolved.textInputFocus, true, "textInputFocus (text-input scope)");
            assertEqual(resolved.playState, "paused", "playState (global/session scope)");
            // The wiring places EVERY one of the six keys — none is dropped.
            for (const key of WHEN_CONTEXT_KEYS) {
                assert(resolved[key] !== undefined, `context ${key} is present`);
            }
        },
    },
    {
        name: "sourcesToLayers: each fact lands in its documented scope layer",
        run: () => {
            const layers = sourcesToLayers({ editorUi: FULL_UI, session: { playState: "playing" } });
            assertEqual(layers.global?.playState, "playing", "playState -> global");
            assertEqual(layers.window?.windowType, "main", "windowType -> window");
            assertEqual(layers.window?.viewportMode, "3d", "viewportMode -> window");
            assertEqual(layers.focusedPanel?.panelFocus, "builtin.inspector", "panelFocus -> focusedPanel");
            assertEqual(layers.textInput?.textInputFocus, true, "textInputFocus -> textInput");
        },
    },

    // ------------------------------------------------------------- the e08 (session state) seam
    {
        name: "the session-state stub is the ONLY source e08 swaps (zero change to the other contexts)",
        run: () => {
            const stopped = resolveContext({ editorUi: FULL_UI, session: STUB_SESSION_STATE });
            assertEqual(stopped.playState, "stopped", "the stub reads stopped");

            // Swapping ONLY the session source (what e08 does) changes ONLY playState.
            const playing: SessionStateSource = { playState: "playing" };
            const swapped = resolveContext({ editorUi: FULL_UI, session: playing });
            assertEqual(swapped.playState, "playing", "the swapped source drives playState");
            assertEqual(swapped.panelFocus, stopped.panelFocus, "panelFocus unchanged by the swap");
            assertEqual(swapped.windowType, stopped.windowType, "windowType unchanged by the swap");
            assertEqual(
                swapped.textInputFocus,
                stopped.textInputFocus,
                "textInputFocus unchanged by the swap",
            );
        },
    },
    {
        name: "the stub editor.ui baseline is the 'nothing focused' context",
        run: () => {
            const resolved = resolveContext({
                editorUi: STUB_EDITOR_UI,
                session: STUB_SESSION_STATE,
            });
            assertEqual(resolved.panelFocus, "", "no panel focused");
            assertEqual(resolved.textInputFocus, false, "no text input");
            // A guard that requires a focused panel is inactive on the empty baseline.
            assertEqual(
                evaluateWhen("panelFocus && !textInputFocus", resolved),
                false,
                "a panel-scoped command is inactive with nothing focused",
            );
        },
    },
];
