// The `window.*` bridge surface, JS side (M9 e10b, design 03 §1 / §7, 04 §2).
//
// This is the TS mirror of `src/editor/shell/include/context/editor/shell/window_bridge.h`, and it is
// editor-core's ONLY way to move a panel between windows: the browser side has no window registry (04
// §1 / 08 §1), so tear-out, "move to window N", and rehome-on-close all travel through here.
//
// THE ONE MECHANISM (D6). A move is: SERIALIZE the panel (`panel.state.get`, panels.ts), hand the
// blob to the Shell, and let the TARGET window's editor-core RECREATE the panel and restore the blob
// (`PanelHost.open` + `panel.state.set`). There is no second recreate path. The only difference
// between the moves is WHEN the target reads its seed:
//   * TEAR-OUT   → a NEW window created by `tearOut`, which reads `seed()` once at boot;
//   * MOVE-TO-N  → an EXISTING window, which drains `rehomed()` on its cheap poll;
//   * REHOME     → a closing window's editor-core `moveTo`s every open panel to window 0, delivered
//                  to window 0 the same way as move-to-N. "Never silently lost" IS that relay.
//
// TWO FACTS ARE LOAD-BEARING, exactly as panels.ts / session.ts state:
//
//   1. THE VOCABULARY IS CROSS-LANGUAGE AND GATED. The method names below are byte-compared against
//      the C++ constants by `tools/check_webui_assets.py` (ctest `webui-panel-contract`), reading the
//      values out of the BUILT bundle. A rename on either side would otherwise unbind the window
//      surface SILENTLY — a tear-out would refuse with `unknown_method` and the panel would be lost
//      with no build error, the exact thing 03 §7's loud-degradation contract forbids.
//
//   2. EVERY PARSER IS TOTAL. The Shell is trusted, but a response is still validated structurally,
//      and a refusal is RETURNED, not thrown: a build with no window factory answers `created:false`
//      (03 §7's loud degrade), which is an ordinary result the caller acts on, not an exception.

import type { ShellBridge } from "./bridge.js";
import { BridgeError, isRecord } from "./bridge.js";

// --------------------------------------------------------------------------- the wire vocabulary
// MUST match window_bridge.h's kWindow*Method. See fact 1 above.

export const WINDOW_LIST_METHOD = "window.list";
export const WINDOW_TEAR_OUT_METHOD = "window.tear-out";
export const WINDOW_MOVE_TO_METHOD = "window.move-to";
export const WINDOW_SEED_METHOD = "window.seed";
export const WINDOW_REHOMED_METHOD = "window.rehomed";
export const WINDOW_CLOSE_METHOD = "window.close";

// -------------------------------------------------------------------------------- the wire shapes

/** The live window set as `window.list` reports it (this window's id + every peer's). */
export interface WindowList {
    readonly windowId: number;
    readonly windows: readonly number[];
}

/**
 * The outcome of a `window.tear-out` (03 §7). LOUD by construction: `created:false` carries the
 * `WindowCreateOutcome` token (`factory_failed`, `limit_reached`, …) and a human reason, so the
 * source window's editor-core can degrade to a floating group AND say WHY.
 */
export interface TearOutResult {
    readonly created: boolean;
    readonly windowId: number;
    readonly outcome: string;
    readonly error: string;
}

/** The outcome of a `window.move-to` (a move to an already-open window, delivered on its poll). */
export interface MoveResult {
    readonly moved: boolean;
    readonly windowId: number;
    readonly error: string;
}

/** One panel plus its OPAQUE D6 state blob, in transit between two windows (mirrors `PanelSeed`). */
export interface WindowSeed {
    readonly panelId: string;
    /** Exactly what `panel.state.get` returned; handed back verbatim to `panel.state.set`. */
    readonly state: unknown;
}

/** This window's boot seed: a single panel to open, or `seeded:false` for an ordinary window. */
export interface BootSeed {
    readonly seeded: boolean;
    readonly seed: WindowSeed | null;
}

/** The outcome of a `window.close` (the source window closing itself after moving its panels away). */
export interface CloseResult {
    readonly closed: boolean;
    readonly outcome: string;
    readonly error: string;
}

// ------------------------------------------------------------------------------- total parsers

function readString(source: Record<string, unknown>, key: string): string {
    const value = source[key];
    return typeof value === "string" ? value : "";
}

function readNumber(source: Record<string, unknown>, key: string): number {
    const value = source[key];
    return typeof value === "number" && Number.isFinite(value) ? value : 0;
}

function readBoolean(source: Record<string, unknown>, key: string): boolean {
    return source[key] === true;
}

/** Parse one `{panelId, state}` seed. `null` when it carries no usable panel id. */
export function parseWindowSeed(value: unknown): WindowSeed | null {
    if (!isRecord(value)) {
        return null;
    }
    const panelId = readString(value, "panelId");
    if (panelId === "") {
        return null;
    }
    // `state` is OPAQUE — carried verbatim; `??` keeps a missing member as null (persists no state).
    return { panelId, state: value["state"] ?? null };
}

/** Parse a `window.rehomed` result into its panel seeds, dropping any that carry no id. */
export function parseRehomed(value: unknown): readonly WindowSeed[] {
    if (!isRecord(value) || !Array.isArray(value["panels"])) {
        return [];
    }
    const seeds: WindowSeed[] = [];
    for (const entry of value["panels"]) {
        const seed = parseWindowSeed(entry);
        if (seed !== null) {
            seeds.push(seed);
        }
    }
    return seeds;
}

// ------------------------------------------------------------------------------- the client

/**
 * The typed client over the `window.*` bridge methods.
 *
 * Thin on purpose: it validates envelopes and narrows types, and holds NO window state — the registry
 * lives in the Shell. A REFUSAL is RETURNED as a degraded result (`created:false` / `moved:false`),
 * never thrown for an ordinary designed state, so the tear-out command's loud-degrade path is a plain
 * branch rather than a try/catch. A transport failure still rejects — that is not an ordinary state.
 */
export class WindowClient {
    readonly #bridge: ShellBridge;

    constructor(bridge: ShellBridge) {
        this.#bridge = bridge;
    }

    /** The live window set. `null` only when the Shell answered something unreadable. */
    async list(): Promise<WindowList | null> {
        try {
            const result = await this.#bridge.call(WINDOW_LIST_METHOD);
            if (!isRecord(result) || !Array.isArray(result["windows"])) {
                return null;
            }
            const windows = result["windows"].filter(
                (id): id is number => typeof id === "number" && Number.isFinite(id),
            );
            return { windowId: readNumber(result, "windowId"), windows };
        } catch (error) {
            if (error instanceof BridgeError) {
                return null;
            }
            throw error;
        }
    }

    /**
     * Tear the panel out into a NEW window, seeded from `state`. NEVER throws for a create FAILURE —
     * the whole point of 03 §7 is that the caller degrades loudly on `created:false`, so a bridge
     * refusal (a build with no factory) resolves to a loud `TearOutResult` rather than an exception.
     */
    async tearOut(panelId: string, state: unknown, title?: string): Promise<TearOutResult> {
        const params: Record<string, unknown> = { panelId, state };
        if (title !== undefined) {
            params["title"] = title;
        }
        try {
            const result = await this.#bridge.call(WINDOW_TEAR_OUT_METHOD, params);
            if (!isRecord(result)) {
                return { created: false, windowId: 0, outcome: "malformed", error: "the Shell returned an unreadable tear-out result" };
            }
            return {
                created: readBoolean(result, "created"),
                windowId: readNumber(result, "windowId"),
                outcome: readString(result, "outcome"),
                error: readString(result, "error"),
            };
        } catch (error) {
            if (error instanceof BridgeError) {
                // A refusal (no factory bound, a malformed request the Shell rejected) is itself a
                // LOUD, honest failure — the same degrade path a factory failure takes.
                return { created: false, windowId: 0, outcome: "refused", error: error.message };
            }
            throw error;
        }
    }

    /** Move the panel to an ALREADY-OPEN window `windowId` (delivered on that window's rehomed poll). */
    async moveTo(panelId: string, state: unknown, windowId: number): Promise<MoveResult> {
        try {
            const result = await this.#bridge.call(WINDOW_MOVE_TO_METHOD, { panelId, state, windowId });
            if (!isRecord(result)) {
                return { moved: false, windowId, error: "the Shell returned an unreadable move result" };
            }
            return {
                moved: readBoolean(result, "moved"),
                windowId: readNumber(result, "windowId"),
                error: readString(result, "error"),
            };
        } catch (error) {
            if (error instanceof BridgeError) {
                return { moved: false, windowId, error: error.message };
            }
            throw error;
        }
    }

    /** Read THIS window's boot seed, consumed once (the tear-out target's open-at-boot path). */
    async seed(): Promise<BootSeed> {
        try {
            const result = await this.#bridge.call(WINDOW_SEED_METHOD);
            if (!isRecord(result) || result["seeded"] !== true) {
                return { seeded: false, seed: null };
            }
            return { seeded: true, seed: parseWindowSeed(result) };
        } catch (error) {
            if (error instanceof BridgeError) {
                return { seeded: false, seed: null };
            }
            throw error;
        }
    }

    /** Drain every panel queued to rehome INTO this window (move-to-N and rehome-on-close delivery). */
    async rehomed(): Promise<readonly WindowSeed[]> {
        try {
            return parseRehomed(await this.#bridge.call(WINDOW_REHOMED_METHOD));
        } catch (error) {
            if (error instanceof BridgeError) {
                return [];
            }
            throw error;
        }
    }

    /** Ask the Shell to close THIS window (refused for the primary, which the app shuts down). */
    async close(): Promise<CloseResult> {
        try {
            const result = await this.#bridge.call(WINDOW_CLOSE_METHOD);
            if (!isRecord(result)) {
                return { closed: false, outcome: "malformed", error: "the Shell returned an unreadable close result" };
            }
            return {
                closed: readBoolean(result, "closed"),
                outcome: readString(result, "outcome"),
                error: readString(result, "error"),
            };
        } catch (error) {
            if (error instanceof BridgeError) {
                return { closed: false, outcome: "refused", error: error.message };
            }
            throw error;
        }
    }
}
