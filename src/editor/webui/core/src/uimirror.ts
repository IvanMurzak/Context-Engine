// The cross-window `editor.ui` mirror TRANSPORT, JS side (M9 e10d, design 05 §5, D7 tier 2).
//
// e08c built the editor-core mirror SEAM in uibus.ts — `UiMirrorSink` (hand an envelope OUT of this
// window) and `EditorUiBus.receiveMirrored` (a total entry for one arriving from another) — and
// deliberately wired NO transport, because uibus.ts must stay structurally incapable of reaching the
// Shell (the D7 boundary `tools/check_ui_bus_boundary.py` enforces on THAT file). This module is the
// transport that seam left for e10 to own: it lives OUTSIDE uibus.ts precisely so the bridge-bearing
// half is here, where reaching the Shell is allowed. It targets a SHELL-LOCAL method (`ui.mirror`),
// never a daemon one — the Shell mirrors chrome facts between its own windows and never forwards them
// to the daemon (that is the D7 violation the whole tier exists to prevent).
//
// THE BROADCASTING SHAPE, and why it is the point. The Shell BROADCASTS every mirrored envelope to
// EVERY window including the sender (window_bridge.h `ui.mirror`). So this window receives its OWN
// envelope back on its poll, and `receiveMirrored` drops it by `origin` — the loop breaker e08c built
// but its ring drill could never exercise (its transport was point-to-point). Wiring THIS broadcasting
// transport is what finally lights that branch end to end.

import { BridgeError, isRecord, type ShellBridge } from "./bridge.js";
import type { EditorUiBus, EditorUiEvent, EditorUiSubscription, UiMirrorSink } from "./uibus.js";

// --------------------------------------------------------------------------- the wire vocabulary
// MUST match window_bridge.h's kUiMirror*Method. Cross-checked byte-for-byte out of the BUILT bundle
// by `tools/check_webui_assets.py --panel-contract` (ctest `webui-panel-contract`) — the same drift
// gate the window.* / drag.* surfaces ride: a rename on either side would leave editor-core calling a
// method the Shell no longer routes, and the cross-window mirror would silently stop propagating with
// no build error.

export const UI_MIRROR_METHOD = "ui.mirror";
export const UI_MIRROR_POLL_METHOD = "ui.mirror-poll";

/**
 * Parse ONE mirrored envelope out of a `ui.mirror-poll` batch, total against a malformed one.
 *
 * The Shell round-trips the `{seq, topic, origin, payload}` envelope verbatim (it never interprets
 * tier-2 chrome), so this only NARROWS: a member of the wrong type reads as its neutral default
 * rather than throwing. `null` for a non-object or a missing topic/origin — a frame with no topic or
 * no origin cannot be applied OR echo-suppressed, so it is dropped rather than half-applied.
 */
export function parseMirroredEvent(value: unknown): EditorUiEvent | null {
    if (!isRecord(value)) {
        return null;
    }
    const topic = value["topic"];
    const origin = value["origin"];
    if (typeof topic !== "string" || topic === "" || typeof origin !== "string" || origin === "") {
        return null;
    }
    const seq = value["seq"];
    return {
        seq: typeof seq === "number" && Number.isFinite(seq) ? seq : 0,
        topic,
        origin,
        // `payload` is OPAQUE — carried verbatim; `??` keeps a missing member as null.
        payload: value["payload"] ?? null,
    };
}

/**
 * The mirror sink that hands a locally-published envelope to the Shell's broadcast.
 *
 * `deliver` is fire-and-forget by contract (`UiMirrorSink.deliver` returns void): the local fan-out
 * already happened before the bus called us (uibus.ts delivers to local subscribers FIRST, mirrors
 * LAST), so a transport failure must never propagate back into the bus. A `BridgeError` (an older
 * Shell that does not route `ui.mirror`) is swallowed — the editor still works single-window; only the
 * cross-window mirror is absent, which is the honest degrade for a Shell that cannot mirror.
 */
export class ShellUiMirrorSink implements UiMirrorSink {
    readonly #bridge: ShellBridge;

    constructor(bridge: ShellBridge) {
        this.#bridge = bridge;
    }

    deliver(event: EditorUiEvent): void {
        void this.#bridge.call(UI_MIRROR_METHOD, event as unknown as Record<string, unknown>).catch(
            (error: unknown) => {
                if (error instanceof BridgeError) {
                    return; // an older Shell that cannot mirror — degrade to single-window, silently
                }
                // A non-bridge error is unexpected; still never propagate into the publish path.
            },
        );
    }
}

/**
 * The receiving end: drain the envelopes the Shell broadcast INTO this window and apply each to the
 * bus. `receiveMirrored` does the echo suppression — an envelope stamped with THIS window's own origin
 * (the sender's copy the broadcast delivered back) is dropped; every other is applied.
 *
 * A cheap GENERATION-free poll on the same shape as the rehome / session polls: the Shell answers
 * `{events:[]}` when nothing is pending, so an idle window costs one empty round trip per tick. A
 * bridge refusal (an older Shell) ends the poll for this window rather than throwing — the mirror is
 * absent, not broken.
 */
export class UiMirrorPoller {
    readonly #bridge: ShellBridge;
    readonly #bus: EditorUiBus;

    constructor(bridge: ShellBridge, bus: EditorUiBus) {
        this.#bridge = bridge;
        this.#bus = bus;
    }

    /**
     * Drain once and apply every drained envelope. Returns how many were APPLIED (an own-origin echo
     * the bus drops does not count) — a test asserts convergence from this, and boot reports it.
     */
    async poll(): Promise<number> {
        let applied = 0;
        try {
            const result = await this.#bridge.call(UI_MIRROR_POLL_METHOD);
            if (!isRecord(result) || !Array.isArray(result["events"])) {
                return 0;
            }
            for (const raw of result["events"]) {
                const event = parseMirroredEvent(raw);
                if (event === null) {
                    continue;
                }
                // receiveMirrored drops the own-origin echo (published === false) and applies the rest.
                if (this.#bus.receiveMirrored(event).published) {
                    applied += 1;
                }
            }
        } catch (error) {
            if (error instanceof BridgeError) {
                return applied; // an older Shell that does not route the poll — the mirror is absent
            }
            throw error;
        }
        return applied;
    }
}

/**
 * Wire the whole transport onto a bus: attach the sink (outbound) and return the poller (inbound) plus
 * the sink's subscription. Boot calls this once the window id is known (the bus's `origin` must be this
 * window's id for echo suppression to distinguish this window's own facts from a peer's).
 */
export interface UiMirrorWiring {
    readonly poller: UiMirrorPoller;
    readonly subscription: EditorUiSubscription;
}

export function wireUiMirror(bridge: ShellBridge, bus: EditorUiBus): UiMirrorWiring {
    const subscription = bus.attachMirror(new ShellUiMirrorSink(bridge));
    return { poller: new UiMirrorPoller(bridge, bus), subscription };
}
