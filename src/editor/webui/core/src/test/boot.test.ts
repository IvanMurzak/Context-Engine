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

import { assert, assertEqual, waitFor, type TestCase } from "./harness.js";
import { ShellBridge, type BridgeQuery, type BridgeQueryFunction } from "../bridge.js";
import {
    CHROME_ATTRIBUTE,
    PANEL_DAEMON_CALL_METHOD,
    SESSION_ATTRIBUTE,
    bootEditorCore,
    makePackageDaemonCall,
} from "../boot.js";
import { SESSION_CONTROL_METHOD, SESSION_STATE_METHOD } from "../session.js";
import { CHROME_STATE_METHOD, WINDOW_SET_APPEARANCE_METHOD } from "../window.js";

/** A mutable answer table so a case can change what the Shell serves mid-run. */
type Answers = Record<string, unknown>;

/**
 * A mock Shell. Anything absent from `answers` is refused exactly as the real router's
 * deny-by-default does (`bridge.unknown_method`), which is what every boot feed degrades on.
 * EXPORTED: chrome.test.ts's boot cases drive the same envelope logic, so there is ONE copy of the
 * refusal shape to keep in lockstep with the real router. `onRequest` lets a case observe a call's
 * params (e.g. record what `editor.regions.publish` carried) without rebuilding the shell.
 */
export function mockShell(
    answers: Answers,
    onRequest?: (method: string, params: unknown) => void,
): { query: BridgeQueryFunction; methods: string[] } {
    const methods: string[] = [];
    const query: BridgeQueryFunction = (request: BridgeQuery): number => {
        const parsed = JSON.parse(request.request) as {
            id: number;
            method: string;
            params?: unknown;
        };
        methods.push(parsed.method);
        onRequest?.(parsed.method, parsed.params);
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
 * EXPORTED for the same reason `mockShell` is: chrome.test.ts's boot cases spread-and-override it.
 */
export function baseAnswers(): Answers {
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

/** The value `data-editor-chrome` was left at, or `""` when boot never reported one (a1). */
function chromeAttribute(): string {
    return document.documentElement.getAttribute(CHROME_ATTRIBUTE) ?? "";
}

function clearChromeAttribute(): void {
    document.documentElement.removeAttribute(CHROME_ATTRIBUTE);
}

export const bootTests: readonly TestCase[] = [
    {
        // ⚠ THIS CASE EXISTS BECAUSE A PLANT FOUND ITS ABSENCE (M9 e13c-1). `panelverbs.test.ts`
        // proves the `bridge.call` HANDLER passes no package id — but it drives a fixture stand-in for
        // `daemonCall`, so it says nothing about the function that actually BUILDS the Shell payload.
        // A plant that made `makePackageDaemonCall` read `packageId` off the panel-controlled params
        // therefore stayed GREEN across the whole suite: the closed-over identity was asserted one
        // layer above the place it could be broken. That is a hole in the TEST TABLE, not in the gate.
        //
        // ⚠ PLANT: `packageId: params?.packageId ?? packageId` in `makePackageDaemonCall` — RED here.
        name: "boot: makePackageDaemonCall closes over the package — a panel cannot name another's session",
        run: async () => {
            const forwarded: Record<string, unknown>[] = [];
            const query: BridgeQueryFunction = (request: BridgeQuery): number => {
                const parsed = JSON.parse(request.request) as {
                    id: number;
                    method: string;
                    params: Record<string, unknown>;
                };
                forwarded.push({ method: parsed.method, params: parsed.params });
                request.onSuccess(
                    JSON.stringify({ jsonrpc: "2.0", id: parsed.id, result: { rows: [] } }),
                );
                return forwarded.length;
            };
            const call = makePackageDaemonCall(new ShellBridge(query), "ext.hello");

            // THE FORGERY, in the one place a panel actually controls: the daemon verb's own params,
            // which travel through this function verbatim.
            const outcome = await call("query", { packageId: "ext.evil", q: "*" });
            assertEqual(outcome.ok, true, "the forward succeeded");
            assertEqual(forwarded.length, 1, "exactly one Shell call was made");
            assertEqual(
                (forwarded[0]?.params as Record<string, unknown>)["packageId"],
                "ext.hello",
                "the Shell is told THIS panel's package — the one closed over at binding time — and " +
                    "never the one the panel put in its own params",
            );
            assertEqual(
                (forwarded[0]?.params as Record<string, unknown>)["method"],
                "query",
                "…with the method verbatim (the panel-callable allowlist is the SHELL's)",
            );
            assertEqual(
                forwarded[0]?.method,
                PANEL_DAEMON_CALL_METHOD,
                "…over the e13c-1 fan-in route, which mirrors C++ kPanelDaemonCallMethod",
            );

            // A REFUSAL IS A VALUE, NEVER A REJECTION — `bridge.call`'s handler must be able to turn
            // every outcome into a PanelVerbRefusal, and an escaping rejection would reach the generic
            // host-fault path and tell the package nothing, `scope.denied` least of all.
            const refusing: BridgeQueryFunction = (request: BridgeQuery): number => {
                const parsed = JSON.parse(request.request) as { id: number };
                request.onSuccess(
                    JSON.stringify({
                        jsonrpc: "2.0",
                        id: parsed.id,
                        error: {
                            code: -32603,
                            message: "the daemon refused 'set' for package 'ext.hello'",
                            data: { reason: "scope.denied" },
                        },
                    }),
                );
                return 1;
            };
            const denied = await makePackageDaemonCall(new ShellBridge(refusing), "ext.hello")(
                "set",
                {},
            );
            assertEqual(denied.ok, false, "the refusal is a VALUE, not a rejection");
            assertEqual(
                denied.code,
                "scope.denied",
                "…carrying the DAEMON's own catalog code, which is the whole observable of this task",
            );
        },
    },
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
        // editor-window-chrome d1 — THE WIRING CLAIM, end to end in a REAL boot: the strip mounts
        // into the a2 slot from the SERVED session state (simTick included), and a strip button
        // press travels strip -> live registry -> play action -> `session.control` -> reply adopted
        // -> strip re-rendered. Every link below is one a stub could sever with every unit tier
        // still green (noop playActions handed to the registry; a strip fed from a copy of the
        // sink; a mount that never happens), which is why this case drives `bootEditorCore` itself.
        name: "boot d1: the play-bar strip mounts from the served session and a press round-trips session.control",
        run: async () => {
            clearSessionAttribute();
            // The a2 strip fixture + the dock root (the harness loads the real dockview engine, so
            // an EMPTY roster brings the command layer up without mounting any panel).
            const titlebar = document.createElement("header");
            titlebar.id = "editor-titlebar";
            const playbar = document.createElement("div");
            playbar.id = "editor-playbar";
            const statusbar = document.createElement("footer");
            statusbar.id = "editor-statusbar";
            const root = document.createElement("main");
            root.id = "editor-root";
            document.body.append(titlebar, playbar, statusbar, root);

            const controlVerbs: unknown[] = [];
            const answers = baseAnswers();
            answers["panel.list"] = { contractMajor: 2, panels: [] };
            // The persistence restore READS this at boot (LayoutPersistence.restore); an empty
            // persisted state is the fresh-project answer. Without it the restore's BridgeError
            // lands in startPanels' silent degrade and the command layer never comes up.
            answers["editor.state.get"] = { layout: null, panels: {} };
            answers[SESSION_STATE_METHOD] = {
                event: "play-state",
                state: "playing",
                origin: 0,
                attached: true,
                generation: 1,
                simTick: 41,
            };
            answers[SESSION_CONTROL_METHOD] = {
                changed: true,
                state: "edit",
                simTick: 0,
                errorCode: "",
            };
            const shell = mockShell(answers, (method: string, params: unknown): void => {
                if (method === SESSION_CONTROL_METHOD) {
                    controlVerbs.push((params as { verb?: unknown } | undefined)?.verb);
                }
            });
            try {
                const report = await bootEditorCore(new ShellBridge(shell.query));
                assert(report.ready, "the editor boots");

                // The strip mounted into the a2 slot, seeded from the SERVED state — tick included.
                const stop = playbar.querySelector<HTMLButtonElement>(
                    '[data-playbar-control="stop"]',
                );
                assert(stop !== null, "the strip's transport mounted into #editor-playbar");
                assertEqual(
                    document.documentElement.getAttribute("data-editor-playbar"),
                    "state playing; simTick 41",
                    "the strip renders the served session truth, not a boot baseline",
                );
                assertEqual(
                    playbar
                        .querySelector('[data-playbar-control="play"]')
                        ?.getAttribute("data-play-state"),
                    "running",
                    "the first data-play-state writer wrote the honest mapping",
                );

                // THE PRESS: stop is enabled while playing; the click must flow through the LIVE
                // registry (the same holder the palette uses) to `session.control` and back.
                assert(stop !== null && !stop.disabled, "stop is enabled while playing");
                stop?.click();
                await waitFor(
                    "the reply to be adopted and the strip re-rendered edit/t+0",
                    () =>
                        document.documentElement.getAttribute("data-editor-playbar") ===
                        "state edit; simTick 0",
                    5_000,
                    () =>
                        `verbs ${JSON.stringify(controlVerbs)}; playbar ${
                            document.documentElement.getAttribute("data-editor-playbar") ??
                            "(absent)"
                        }; commands ${
                            document.documentElement.getAttribute("data-editor-commands") ??
                            "(absent)"
                        }; bootError: ${report.error}`,
                );
                assertEqual(controlVerbs, ["stop"], "the press dispatched session.control {stop}");
                assertEqual(
                    playbar
                        .querySelector('[data-playbar-control="play"]')
                        ?.getAttribute("data-play-state"),
                    "idle",
                    "the flourish followed the daemon's answer",
                );
            } finally {
                // Withdraw the relay so the 500ms poll startSession started self-stops on its next
                // refused tick (the same cleanup the sibling cases use), then drop the fixture.
                delete answers[SESSION_STATE_METHOD];
                titlebar.remove();
                playbar.remove();
                statusbar.remove();
                root.remove();
            }
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
    {
        // editor-window-chrome a1: the REAL bootEditorCore fetches the chrome contract and reports
        // it — served values a default could not have, so a boot that stopped fetching (or a parser
        // that stopped reading a field) reds on that field's absence from the attribute.
        name: "boot: chrome.state is fetched at boot and reported on data-editor-chrome",
        run: async () => {
            clearChromeAttribute();
            const answers = baseAnswers();
            answers[CHROME_STATE_METHOD] = {
                mode: "hybrid",
                controlsInset: { left: 72, right: 4 },
                maximized: true,
                focused: true,
                window: "secondary",
            };
            const shell = mockShell(answers);
            const report = await bootEditorCore(new ShellBridge(shell.query));
            assert(report.ready, "the editor boots");
            assert(shell.methods.includes(CHROME_STATE_METHOD), "chrome.state was called at boot");
            assertEqual(
                chromeAttribute(),
                "mode=hybrid inset=72,4 maximized=true focused=true window=secondary",
                "the fetched state is reported verbatim",
            );
        },
    },
    {
        name: "boot: an older Shell with no chrome surface degrades to the honest system default",
        run: async () => {
            clearChromeAttribute();
            // No `chrome.state` — the deny-by-default refusal a pre-a1 Shell gives. Boot must not
            // break, and the report must be the stock-OS-frame default, not silence.
            const shell = mockShell(baseAnswers());
            const report = await bootEditorCore(new ShellBridge(shell.query));
            assert(report.ready, "the editor still boots");
            assertEqual(
                chromeAttribute(),
                "mode=system inset=0,0 maximized=false focused=false window=primary",
                "the refusal degrades to the default and is still REPORTED",
            );
        },
    },
    {
        // editor-window-chrome b1: the REAL bootEditorCore reports the applied theme's appearance
        // over `window.set-appearance` (the DWM dark-mode tint's feed, 02 §3). Asserted AGAINST THE
        // APPLIED TRUTH — the `data-appearance` attribute ThemeEngine.apply stamps — rather than a
        // hard-coded "dark", because with no persisted choice the boot theme follows the HOST's
        // prefers-color-scheme, an ambient input a fixed expectation would be coupled to (the
        // theme.ts smoke-pin rationale). The Shell serving the method is enough for the report to
        // be accepted; the refusal path is the previous case's (boot survives an older Shell).
        name: "boot: the applied theme's appearance is reported over window.set-appearance",
        run: async () => {
            const reported: unknown[] = [];
            const answers = baseAnswers();
            answers[WINDOW_SET_APPEARANCE_METHOD] = { accepted: true };
            const shell = mockShell(answers, (method, params) => {
                if (method === WINDOW_SET_APPEARANCE_METHOD && params !== null) {
                    reported.push((params as Record<string, unknown>)["appearance"]);
                }
            });
            const report = await bootEditorCore(new ShellBridge(shell.query));
            assert(report.ready, "the editor boots");
            assert(reported.length >= 1, "boot reported the appearance at least once");
            const applied = document.documentElement.getAttribute("data-appearance") ?? "";
            assert(applied === "dark" || applied === "light", `a theme was applied (${applied})`);
            assertEqual(
                reported[reported.length - 1],
                applied,
                "the report carries the APPLIED theme's appearance, not a guess",
            );
        },
    },
];
