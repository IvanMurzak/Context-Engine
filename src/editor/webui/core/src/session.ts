// The DAEMON SESSION feed, browser side (M9 e08d, design 05 §4 / §6, D7 tier 1).
//
// WHAT THIS CLOSES. e08a put the semantic human state — selection, cameras, PLAY STATE — in the
// daemon; e08b landed `DaemonSessionState` (when.ts), the sink that projects the daemon's `session`
// topic onto the one fact the when-context model reads. What e08b could NOT land was the wiring, so
// `boot.ts` kept resolving its when-context from a frozen `edit` baseline and every `playState`
// clause in the live editor was wrong. THIS module is that wiring: it is the only thing between the
// daemon's play state and a `when` clause in the browser.
//
// WHY IT POLLS, WHEN THE SINK IS EXPLICITLY "NOT A POLLER". Both are true, and the split is the
// point. editor-core is a pure wire-client of the Shell (04 §1 / 08 §1) and the e05c bridge accepts
// NO persistent queries by construction (`cef_shell.cpp` completes every query inside `OnQuery`), so
// there is no push channel to the renderer at all — nothing can subscribe from here. The Shell,
// which IS a real daemon subscriber (e08b `SessionFeed`), relays its last-known state over
// `session.state`, and this feed READS it and hands the reply to the sink. `DaemonSessionState`
// itself still never polls anything; it is fed, exactly as the C++ panels' one is. The same shape
// `themes.get` / `keybindings.get` already use, including the GENERATION compare that makes an idle
// poll one integer comparison and one served call.
//
// THE REPLY IS THE DAEMON'S OWN FACT SHAPE, so it is handed to `applyFact` VERBATIM — no adapter
// that could drift between the two languages (see session_bridge.h for the C++ half's reasoning).
//
// NEVER FATAL, like every other boot feed. A Shell that does not serve the method (an older build)
// refuses with `bridge.unknown_method`; that leaves the sink on its boot baseline — which is exactly
// what such a build can honestly know — and STOPS the poll, so an unserved method costs one refusal
// rather than one per tick forever.

import { BridgeError, ShellBridge, isRecord } from "./bridge.js";
import { DaemonSessionState, toPlayState, type PlayState } from "./when.js";

/**
 * The Shell method that relays the daemon's session state.
 *
 * MUST match `kSessionStateMethod` in src/editor/shell/include/context/editor/shell/session_bridge.h
 * — the `webui-panel-contract` gate re-reads this value out of the BUILT bundle and compares it to
 * that constant, the same cross-language discipline `keybindings.get` / `themes.get` / `config.*`
 * use. A rename on either side leaves editor-core calling a method the Shell no longer routes, so
 * `playState` silently freezes at `edit` again with NOTHING reporting it.
 */
export const SESSION_STATE_METHOD = "session.state";

/**
 * The Shell method the play-bar strip's transport writes through (editor-window-chrome d1).
 *
 * MUST match `kSessionControlMethod` in session_bridge.h — the `webui-panel-contract` gate
 * cross-checks it exactly as it does `SESSION_STATE_METHOD` above. The Shell relays the verb to its
 * `SessionFeed` writer (the proven e08b chain: `editor.play|pause|stop|step` over the Shell's own
 * client, with its `origin` echo suppression), so the strip, the palette's `play.*` commands and
 * the docked playbar drive ONE implementation.
 */
export const SESSION_CONTROL_METHOD = "session.control";

/**
 * The `verb` vocabulary `session.control` accepts — MUST match `kSessionControlVerb*` in
 * session_bridge.h (same gate). A drifted verb is refused as `session.bad_verb` on every press of a
 * button that looks perfectly wired, which is why the tokens are pinned rather than trusted.
 */
export const SESSION_CONTROL_VERB_PLAY = "play";
export const SESSION_CONTROL_VERB_PAUSE = "pause";
export const SESSION_CONTROL_VERB_STOP = "stop";
export const SESSION_CONTROL_VERB_STEP = "step";

export type SessionControlVerb =
    | typeof SESSION_CONTROL_VERB_PLAY
    | typeof SESSION_CONTROL_VERB_PAUSE
    | typeof SESSION_CONTROL_VERB_STOP
    | typeof SESSION_CONTROL_VERB_STEP;

/**
 * How often the feed re-reads the relay, in milliseconds.
 *
 * A human-perceptible-but-idle-cheap cadence: the read is one bridge round trip answering a struct
 * the Shell already holds (no daemon call, no IO), and an unchanged reply moves nothing — `applyFact`
 * returns false and no consumer is touched. Play state changes at human speed, so a sub-second
 * ceiling on staleness is well inside the R-HUX latency budget for "the palette shows the right
 * commands" without turning the bridge into a busy loop.
 */
export const SESSION_POLL_INTERVAL_MS = 500;

/** What a session read produced — reported so a boot can name WHY it sees the state it sees. */
export interface SessionReadReport {
    /** Did the Shell serve the method at all? False means an older Shell (or none). */
    readonly served: boolean;
    /** Is the Shell attached to a live daemon behind that state? */
    readonly attached: boolean;
    /** The state the sink holds AFTER applying this read. */
    readonly playState: PlayState;
    /** The simTick the sink holds AFTER applying this read (d1 — the strip's `t+` source). */
    readonly simTick: number;
    /** Did this read actually move the sink? */
    readonly changed: boolean;
    /** `""` on a served read; the refusal reason otherwise. */
    readonly diagnostic: string;
}

/** A minimal scheduler seam so the T1 tier can drive the poll without a real timer. */
export interface SessionScheduler {
    setInterval(callback: () => void, delayMs: number): number;
    clearInterval(handle: number): void;
}

/** The default scheduler: the global timers, degrading to "never ticks" where there are none. */
export function defaultSessionScheduler(): SessionScheduler | undefined {
    const scope = globalThis as {
        setInterval?: (callback: () => void, delayMs: number) => unknown;
        clearInterval?: (handle: unknown) => void;
    };
    if (typeof scope.setInterval !== "function" || typeof scope.clearInterval !== "function") {
        return undefined;
    }
    return {
        setInterval: (callback: () => void, delayMs: number): number =>
            scope.setInterval?.(callback, delayMs) as unknown as number,
        clearInterval: (handle: number): void => {
            scope.clearInterval?.(handle);
        },
    };
}

/**
 * The typed client over `session.state`.
 *
 * Returns the raw reply record rather than a parsed struct, because the reply IS the daemon's fact
 * and the parser that owns it is `DaemonSessionState.applyFact` (when.ts) — re-parsing it here would
 * be a second, drifting reader of one wire shape. `null` means the Shell refused or answered
 * something that is not a record.
 */
export class SessionClient {
    readonly #bridge: ShellBridge;

    constructor(bridge: ShellBridge) {
        this.#bridge = bridge;
    }

    /** One read. Rejects only through `BridgeError`, which callers here convert into a report. */
    async get(): Promise<Record<string, unknown> | null> {
        const result = await this.#bridge.call(SESSION_STATE_METHOD);
        return isRecord(result) ? result : null;
    }
}

/** What one `session.control` write produced (editor-window-chrome d1). TOTAL — never a throw. */
export interface SessionControlReport {
    /** Did the Shell serve the method? False = an older Shell, or a transport fault. */
    readonly served: boolean;
    /** Did something actually move (the daemon's `changed`)? */
    readonly changed: boolean;
    /**
     * The state AFTER, as the daemon reports it — `null` when the reply's token is one this build
     * cannot name (the `toPlayState` rule: keep the last known state, never invent `edit`).
     */
    readonly playState: PlayState | null;
    /** The raw wire token behind `playState` — relayed to the sink verbatim, never re-spelled. */
    readonly stateToken: string;
    /** The running session's simTick, as the daemon reports it. */
    readonly simTick: number;
    /** The reserved `play.*` catalog code on a daemon refusal; `""` otherwise. */
    readonly errorCode: string;
    /** `""` on a served write; the refusal reason otherwise. */
    readonly diagnostic: string;
}

/**
 * The seam the play actions write through — what `SessionControlClient` implements, named so the T1
 * tier can drive `makePlayActions` (playbar.ts) with a scripted sender instead of a wired bridge.
 */
export interface SessionControlSender {
    send(verb: SessionControlVerb): Promise<SessionControlReport>;
}

/**
 * The typed client over `session.control` (editor-window-chrome d1) — the strip's ONE write path.
 *
 * TOTAL like `SessionFeed`: a refusal (an older Shell that does not route the method, a transport
 * fault) is a REPORT, never a throw — a transport button sits in a click handler nobody awaits, so
 * an escaping rejection would be an unhandled rejection in a renderer nobody watches. `changed:
 * false` with an empty `errorCode` is the Shell's honest "nothing to drive / nothing to do" (a
 * benign no-op, or no daemon link — the Shell deliberately does not distinguish them; see
 * session_bridge.h `SessionControlOutcome`).
 */
export class SessionControlClient implements SessionControlSender {
    readonly #bridge: ShellBridge;

    constructor(bridge: ShellBridge) {
        this.#bridge = bridge;
    }

    async send(verb: SessionControlVerb): Promise<SessionControlReport> {
        let reply: unknown;
        try {
            reply = await this.#bridge.call(SESSION_CONTROL_METHOD, { verb });
        } catch (error) {
            return {
                served: false,
                changed: false,
                playState: null,
                stateToken: "",
                simTick: 0,
                errorCode: "",
                diagnostic:
                    error instanceof BridgeError
                        ? `${error.reason}: ${error.message}`
                        : error instanceof Error
                          ? error.message
                          : String(error),
            };
        }
        if (!isRecord(reply)) {
            return {
                served: true,
                changed: false,
                playState: null,
                stateToken: "",
                simTick: 0,
                errorCode: "",
                diagnostic: "the Shell answered session.control with a non-record",
            };
        }
        const stateToken = typeof reply["state"] === "string" ? reply["state"] : "";
        const simTick = reply["simTick"];
        return {
            served: true,
            changed: reply["changed"] === true,
            playState: toPlayState(stateToken),
            stateToken,
            simTick:
                typeof simTick === "number" && Number.isInteger(simTick) && simTick >= 0
                    ? simTick
                    : 0,
            errorCode: typeof reply["errorCode"] === "string" ? reply["errorCode"] : "",
            diagnostic: "",
        };
    }
}

/**
 * The live feed: read the relay, hand the reply to the sink, repeat.
 *
 * TOTAL — no method throws or rejects. A refusal, a malformed reply, or a missing scheduler each
 * leave the sink on its last known (and at boot, correct) state and are REPORTED, never thrown: this
 * sits on the boot path of a renderer whose only diagnostic channel is an attribute on `<html>`.
 */
export class SessionFeed {
    readonly #client: SessionClient;
    readonly #state: DaemonSessionState;
    readonly #scheduler: SessionScheduler | undefined;
    readonly #onRead: ((report: SessionReadReport) => void) | undefined;
    #handle: number | null = null;
    #reads = 0;
    #generation = -1;

    constructor(
        bridge: ShellBridge,
        state: DaemonSessionState,
        scheduler: SessionScheduler | undefined = defaultSessionScheduler(),
        // Called after EVERY completed read (served or refused) with the report — the d1 strip's
        // update channel: boot hands it a callback that re-renders the play bar, so the strip is
        // FED exactly as the sink is, never a second poller. Total like the feed itself: a throwing
        // callback is contained (a strip that cannot paint must not stop the when-context feed).
        onRead?: (report: SessionReadReport) => void,
    ) {
        this.#client = new SessionClient(bridge);
        this.#state = state;
        this.#scheduler = scheduler;
        this.#onRead = onRead;
    }

    /** How many reads have completed (served or refused) — the T1 tier's poll evidence. */
    get reads(): number {
        return this.#reads;
    }

    /** Is the repeating poll running? False before `start()` and after a refusal stopped it. */
    get polling(): boolean {
        return this.#handle !== null;
    }

    /** The last `generation` the Shell reported, or -1 before the first served read. */
    get generation(): number {
        return this.#generation;
    }

    /**
     * Read once and apply.
     *
     * The GENERATION short-circuit is deliberate and one-directional: an unchanged generation skips
     * `applyFact`, but a MISSING/unparseable generation does NOT skip it — a Shell that cannot count
     * must still be believed about the state itself, or a relay bug would present as a freeze.
     */
    async refresh(): Promise<SessionReadReport> {
        this.#reads += 1;
        let reply: Record<string, unknown> | null = null;
        try {
            reply = await this.#client.get();
        } catch (error) {
            return this.#report({
                served: false,
                attached: false,
                playState: this.#state.playState,
                simTick: this.#state.simTick,
                changed: false,
                diagnostic:
                    error instanceof BridgeError
                        ? `${error.reason}: ${error.message}`
                        : error instanceof Error
                          ? error.message
                          : String(error),
            });
        }
        if (reply === null) {
            return this.#report({
                served: true,
                attached: false,
                playState: this.#state.playState,
                simTick: this.#state.simTick,
                changed: false,
                diagnostic: "the Shell answered session.state with a non-record",
            });
        }
        const generation =
            typeof reply["generation"] === "number" ? reply["generation"] : Number.NaN;
        const fresh = Number.isNaN(generation) || generation !== this.#generation;
        const changed = fresh ? this.#state.applyFact(reply) : false;
        if (!Number.isNaN(generation)) {
            this.#generation = generation;
        }
        return this.#report({
            served: true,
            attached: reply["attached"] === true,
            playState: this.#state.playState,
            simTick: this.#state.simTick,
            changed,
            diagnostic: "",
        });
    }

    /** Hand the report to the d1 strip callback (contained), then return it to the caller. */
    #report(report: SessionReadReport): SessionReadReport {
        try {
            this.#onRead?.(report);
        } catch {
            // A strip that cannot paint must not stop the when-context feed.
        }
        return report;
    }

    /**
     * Start the repeating poll. A no-op when there is no scheduler or one is already running.
     *
     * Each tick is fire-and-forget by design: `refresh` is total, and awaiting a tick from a timer
     * callback would only give a place for an unhandled rejection to appear.
     */
    start(intervalMs: number = SESSION_POLL_INTERVAL_MS): void {
        if (this.#scheduler === undefined || this.#handle !== null) {
            return;
        }
        this.#handle = this.#scheduler.setInterval((): void => {
            void this.refresh().then((report) => {
                // An older Shell refuses every tick identically. Stop rather than accumulate one
                // refusal per tick for the life of the window — a bounded cost for a permanent state.
                if (!report.served) {
                    this.stop();
                }
            });
        }, intervalMs);
    }

    /** Stop the poll. Idempotent, and safe to call when it never started. */
    stop(): void {
        if (this.#scheduler === undefined || this.#handle === null) {
            return;
        }
        this.#scheduler.clearInterval(this.#handle);
        this.#handle = null;
    }
}

/** Render a read report as the `<html data-editor-session>` diagnostic string. */
export function describeSessionRead(report: SessionReadReport): string {
    if (!report.served) {
        return `session feed unavailable: ${report.diagnostic}; playState "${report.playState}"`;
    }
    const link = report.attached ? "daemon attached" : "no daemon link";
    return `playState "${report.playState}" (${link})`;
}
