// T1 unit tests for the when-context evaluator (M9 e07b, design 05 §6 / 03 §6). Two properties this
// tier pins: (1) the SIX contexts resolve from their two sources with the fixed scope precedence
// (text-input > focused panel > window > global) — the resolution-order matrix; and (2) the clause
// evaluator is TOTAL and FAIL-CLOSED (empty = always, malformed = inactive, never a throw).
//
// M9 e08b adds the third: the REAL daemon-backed `DaemonSessionState` — its `origin` echo
// suppression, its tolerance, and the fact that a daemon `play-state` fact reaches a `when` clause
// with ZERO change to the evaluator or the registry (the seam's whole purpose).

import { assert, assertEqual, type TestCase } from "./harness.js";
import {
    DaemonSessionState,
    evaluateWhen,
    resolveContext,
    resolveWhenContext,
    sourcesToLayers,
    STUB_EDITOR_UI,
    WHEN_CONTEXT_KEYS,
} from "../when.js";
import type { EditorUiSource, SessionStateSource, WhenContext } from "../when.js";

// e08d DELETED the frozen stub session constant: `boot.ts` resolved the LIVE editor's when-context
// from it and froze `playState` at `edit` for the whole session, so the placeholder was removed
// rather than left where it had already proved it can become permanent. A FRESHLY CONSTRUCTED
// `DaemonSessionState` IS the boot baseline (`edit`, no live session — see its doc comment), so the
// cases below assert exactly what they asserted before, against the real source at rest.
const BASELINE_SESSION = (): SessionStateSource => new DaemonSessionState();

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
        name: "the session source is the ONLY one e08 swapped (zero change to the other contexts)",
        run: () => {
            const base = resolveContext({ editorUi: FULL_UI, session: BASELINE_SESSION() });
            // e08b corrected the token: `edit` is the daemon's own L-51 vocabulary, not "stopped".
            assertEqual(base.playState, "edit", "the boot baseline reads the daemon's `edit`");

            // Swapping ONLY the session source (what e08b did) changes ONLY playState.
            const playing: SessionStateSource = { playState: "playing" };
            const swapped = resolveContext({ editorUi: FULL_UI, session: playing });
            assertEqual(swapped.playState, "playing", "the swapped source drives playState");
            assertEqual(swapped.panelFocus, base.panelFocus, "panelFocus unchanged by the swap");
            assertEqual(swapped.windowType, base.windowType, "windowType unchanged by the swap");
            assertEqual(
                swapped.textInputFocus,
                base.textInputFocus,
                "textInputFocus unchanged by the swap",
            );
        },
    },

    // ------------------------------------------- e08b: the REAL daemon-backed session source
    //
    // These would all pass trivially against the old stub, which is exactly why each asserts
    // something the stub CANNOT do: move on a daemon fact, drop an echo, and reject a token.
    {
        name: "DaemonSessionState: a `play-state` fact from another client drives playState",
        run: () => {
            const session = new DaemonSessionState();
            session.clientId = 4;
            assertEqual(session.playState, "edit", "boot is `edit` — play state is never persisted");

            assert(
                session.applyFact({ event: "play-state", origin: 9, state: "playing", simTick: 3 }),
                "a fact from another client is applied",
            );
            assertEqual(session.playState, "playing", "the daemon's state is now the source");
            assertEqual(session.applied, 1, "one fact moved the state");

            // ...and it reaches a `when` clause with no change to the evaluator or the registry.
            const ctx = resolveContext({ editorUi: FULL_UI, session });
            assertEqual(
                evaluateWhen("playState == playing", ctx),
                true,
                "a play-guarded command is active while the DAEMON says playing",
            );
            assertEqual(evaluateWhen("playState == edit", ctx), false, "and `edit` is not");
        },
    },
    {
        name: "DaemonSessionState: our OWN echo is dropped (the e08a `origin` contract)",
        run: () => {
            const session = new DaemonSessionState();
            session.clientId = 4;
            assert(
                !session.applyFact({ event: "play-state", origin: 4, state: "playing" }),
                "a fact stamped with our own client id is an echo",
            );
            assertEqual(session.playState, "edit", "an echo never moves the state");
            assertEqual(session.echoesDropped, 1, "and it is counted, not silently ignored");
        },
    },
    {
        name: "DaemonSessionState: an UNATTACHED source (clientId 0) is a plain subscriber",
        run: () => {
            // 0 is also the DAEMON's own origin, so a naive `origin === clientId` test would make an
            // unattached client swallow every daemon-originated fact in silence.
            const session = new DaemonSessionState();
            assertEqual(session.clientId, 0, "not attached");
            assert(
                session.applyFact({ event: "play-state", origin: 0, state: "paused" }),
                "a daemon-originated fact is applied, not read as our echo",
            );
            assertEqual(session.playState, "paused", "the state moved");
            assertEqual(session.echoesDropped, 0, "nothing was dropped");
        },
    },
    {
        name: "DaemonSessionState: malformed / foreign / unknown-token payloads leave the state alone",
        run: () => {
            const session = new DaemonSessionState();
            assert(session.applyFact({ event: "play-state", origin: 9, state: "playing" }), "setup");
            assertEqual(session.playState, "playing", "setup");

            // Tolerance: none of these throws, and none of them moves the state.
            for (const payload of [
                null,
                undefined,
                "play-state",
                [],
                {},
                { event: "selection-changed", origin: 9, ids: ["a"] },
                { event: "camera-changed", origin: 9, viewportId: "main" },
                { event: "play-state", origin: 9 },
                { event: "play-state", origin: 9, state: 7 },
                // An unknown token from a NEWER daemon: keep the last known state rather than
                // claiming `edit` (a confident lie about whether a session is live).
                { event: "play-state", origin: 9, state: "rewinding" },
            ]) {
                assert(!session.applyFact(payload), "no state change");
            }
            assertEqual(session.playState, "playing", "the last KNOWN state survived every one");
            assertEqual(session.applied, 1, "only the real fact counted");
        },
    },
    {
        name: "DaemonSessionState: a restated fact is idempotent; reset returns to the boot baseline",
        run: () => {
            const session = new DaemonSessionState();
            assert(session.applyFact({ event: "play-state", origin: 9, state: "playing" }), "setup");
            assert(
                !session.applyFact({ event: "play-state", origin: 9, state: "playing" }),
                "a restatement (a `sinceSeq: 0` ring replay can deliver one twice) changes nothing",
            );
            assertEqual(session.applied, 1, "and is not counted twice");

            // A reconnect means a restarted daemon, which holds no live session.
            session.reset();
            assertEqual(session.playState, "edit", "reset returns to the boot baseline");
        },
    },
    {
        name: "the stub editor.ui baseline is the 'nothing focused' context",
        run: () => {
            const resolved = resolveContext({
                editorUi: STUB_EDITOR_UI,
                session: BASELINE_SESSION(),
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
