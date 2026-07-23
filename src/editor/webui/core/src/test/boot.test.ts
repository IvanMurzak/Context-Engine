// T1 for the BOOT WIRING of the when-context's session source (M9 e08d, design 05 §4 / §6).
//
// THE ONE PROPERTY THIS FILE EXISTS FOR: the LIVE editor's when-context must read the daemon's play
// state, and this test must FAIL if that source is reverted to a frozen stub. That is the task's own
// hard line, and it is worth stating why the obvious cheaper tests do NOT satisfy it:
//
//   * asserting `DaemonSessionState` is constructed would pass with `boot.ts` still resolving from a
//     stub — which is EXACTLY the state e08b shipped (the source landed; the wiring did not);
//   * asserting a standalone provider factory reflects its argument would pass if the CALL SITE
//     handed it a stub.
//
// So this drives the REAL `bootEditorCore` against a mock Shell that reports `playing`, and asserts
// on `data-editor-session`, which `startSession` computes from THE SAME provider closure it hands
// the palette (boot.ts § startSession: one provider, one session object). A regression that re-froze
// the source cannot show `playing` here while serving `edit` to the palette — there is only one
// object to freeze.
//
// WHY THIS TIER AND NOT THE LIVE SMOKE. `editor-cef-smoke-shell*` runs the real thing but is CI-only
// and cannot script a daemon into `playing` without a real play session; this tier runs in a real
// browser locally in ~2s and can. The smokes' contribution is the complementary one: they prove the
// `session.state` surface is SERVED (their strict `bridge.refused() == 0` invariant fails otherwise).

import { assert, assertEqual, type TestCase } from "./harness.js";
import { ShellBridge, type BridgeQuery, type BridgeQueryFunction } from "../bridge.js";
import { SESSION_ATTRIBUTE, bootEditorCore } from "../boot.js";
import { SESSION_STATE_METHOD } from "../session.js";

/** A mutable answer table so a case can change what the Shell serves mid-run. */
type Answers = Record<string, unknown>;

/**
 * A mock Shell. Anything absent from `answers` is refused exactly as the real router's
 * deny-by-default does (`bridge.unknown_method`), which is what every boot feed degrades on.
 */
function mockShell(answers: Answers): { query: BridgeQueryFunction; methods: string[] } {
    const methods: string[] = [];
    const query: BridgeQueryFunction = (request: BridgeQuery): number => {
        const parsed = JSON.parse(request.request) as { id: number; method: string };
        methods.push(parsed.method);
        const has = Object.prototype.hasOwnProperty.call(answers, parsed.method);
        const envelope = has
            ? { jsonrpc: "2.0", id: parsed.id, result: answers[parsed.method] }
            : {
                  jsonrpc: "2.0",
                  id: parsed.id,
                  error: {
                      code: -32601,
                      message: "unknown method",
                      data: { reason: "bridge.unknown_method" },
                  },
              };
        request.onSuccess(JSON.stringify(envelope));
        return methods.length;
    };
    return { query, methods };
}

/**
 * The minimum a boot needs to reach the session wiring: the three-leg handshake, plus a welcome
 * surface reporting the PROJECT mode so boot takes the editor path rather than the front door.
 *
 * `panel.list` is deliberately ABSENT. With no roster PanelHost never creates a docking root, so no
 * Dockview instance and no palette overlay are left in the shared test document — and the property
 * under test is upstream of the panels anyway (`startSession` runs before `startPanels`).
 */
function baseAnswers(): Answers {
    return {
        "shell.hello": { nonce: "e08d-nonce" },
        "shell.ready": {},
        "welcome.state": { mode: "project" },
    };
}

/** The value `data-editor-session` was left at, or `""` when boot never reported one. */
function sessionAttribute(): string {
    return document.documentElement.getAttribute(SESSION_ATTRIBUTE) ?? "";
}

function clearSessionAttribute(): void {
    document.documentElement.removeAttribute(SESSION_ATTRIBUTE);
}

export const bootTests: readonly TestCase[] = [
    {
        name: "boot: the live when-context reads the DAEMON's play state (fails on a frozen stub)",
        run: async () => {
            clearSessionAttribute();
            const answers = baseAnswers();
            answers[SESSION_STATE_METHOD] = {
                event: "play-state",
                state: "playing",
                origin: 0,
                attached: true,
                generation: 1,
            };
            const shell = mockShell(answers);
            const report = await bootEditorCore(new ShellBridge(shell.query));

            assert(report.attached, "the mock bridge was detected");
            assert(report.ready, "the three-leg handshake completed");
            assert(
                shell.methods.includes(SESSION_STATE_METHOD),
                "boot READ the session relay (a boot that never asks cannot be tracking anything)",
            );
            const detail = sessionAttribute();
            assert(
                detail.includes('when playState "playing"'),
                `the RESOLVED when-context tracks the daemon's play state, got: ${detail}`,
            );
            assert(detail.includes("daemon attached"), `the link state is reported, got: ${detail}`);

            // Stop the poll `startSession` started: the feed self-stops on its first refused tick,
            // so withdrawing the method is how this case cleans up after itself without reaching
            // into boot's internals (and it re-exercises that self-stop on the REAL boot path).
            delete answers[SESSION_STATE_METHOD];
        },
    },
    {
        name: "boot: a paused daemon resolves to `paused`, not the boot baseline",
        run: async () => {
            clearSessionAttribute();
            const answers = baseAnswers();
            answers[SESSION_STATE_METHOD] = {
                event: "play-state",
                state: "paused",
                origin: 0,
                attached: true,
                generation: 5,
            };
            const shell = mockShell(answers);
            await bootEditorCore(new ShellBridge(shell.query));
            assert(
                sessionAttribute().includes('when playState "paused"'),
                `a second distinct state also reaches the when-context, got: ${sessionAttribute()}`,
            );
            delete answers[SESSION_STATE_METHOD];
        },
    },
    {
        name: "boot: an older Shell with no session relay degrades honestly to the boot baseline",
        run: async () => {
            clearSessionAttribute();
            // No `session.state` at all — the deny-by-default refusal a pre-e08d Shell gives.
            const shell = mockShell(baseAnswers());
            const report = await bootEditorCore(new ShellBridge(shell.query));
            assert(report.ready, "the editor still boots");
            const detail = sessionAttribute();
            assert(
                detail.includes("session feed unavailable"),
                `the unavailability is NAMED rather than implied, got: ${detail}`,
            );
            assert(
                detail.includes('playState "edit"'),
                `and the baseline it fell back to is stated, got: ${detail}`,
            );
        },
    },
    {
        name: "boot: outside the Shell there is no bridge, and boot says so instead of throwing",
        run: async () => {
            const report = await bootEditorCore(undefined);
            assert(!report.attached, "a bundle loaded outside the Shell reports detached");
            assertEqual(report.error, "", "which is honest, not an error");
        },
    },
];
