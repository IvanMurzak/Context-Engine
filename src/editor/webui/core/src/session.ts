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
import { DaemonSessionState, type PlayState } from "./when.js";

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
    #handle: number | null = null;
    #reads = 0;
    #generation = -1;

    constructor(
        bridge: ShellBridge,
        state: DaemonSessionState,
        scheduler: SessionScheduler | undefined = defaultSessionScheduler(),
    ) {
        this.#client = new SessionClient(bridge);
        this.#state = state;
        this.#scheduler = scheduler;
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
            return {
                served: false,
                attached: false,
                playState: this.#state.playState,
                changed: false,
                diagnostic:
                    error instanceof BridgeError
                        ? `${error.reason}: ${error.message}`
                        : error instanceof Error
                          ? error.message
                          : String(error),
            };
        }
        if (reply === null) {
            return {
                served: true,
                attached: false,
                playState: this.#state.playState,
                changed: false,
                diagnostic: "the Shell answered session.state with a non-record",
            };
        }
        const generation =
            typeof reply["generation"] === "number" ? reply["generation"] : Number.NaN;
        const fresh = Number.isNaN(generation) || generation !== this.#generation;
        const changed = fresh ? this.#state.applyFact(reply) : false;
        if (!Number.isNaN(generation)) {
            this.#generation = generation;
        }
        return {
            served: true,
            attached: reply["attached"] === true,
            playState: this.#state.playState,
            changed,
            diagnostic: "",
        };
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
