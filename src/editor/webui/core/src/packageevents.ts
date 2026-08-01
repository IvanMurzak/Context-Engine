// THE DAEMON-EVENT FAN-OUT, editor-core's half (M9 e13c-2, design 04 §5 / 05 §1).
//
// THE CHAIN THIS MODULE CLOSES. A package panel subscribes over its port (`bridge.events.subscribe`,
// panelverbs.ts) → the Shell forwards it onto that package's BASELINE daemon session (e13c-1) → the
// daemon PUSHES `event` frames back → the Shell's owner loop pumps them into a BOUNDED per-package
// buffer (package_events.h) → THIS module drains that buffer and pushes each event INTO the iframe
// over the e13b-1 MessageChannel port. The last hop is the only one a package can observe, and it is
// what makes a subscription mean anything at all.
//
// ⚠ WHY IT IS A POLL AND NOT A PUSH, ALL THE WAY DOWN. The e05c bridge accepts NO PERSISTENT QUERIES
// by construction — `cef_shell.cpp` refuses `persistent` outright, with its rationale in-code — so
// there is no channel over which the Shell can push into the renderer at all. Every Shell→renderer
// fact in this editor is therefore drained on a tick: `themes.get`, `keybindings.get`,
// `session.state`, `window.rehomed`, `ui.mirror-poll`, and now `panel.events.poll`. Reaching for a
// push here would mean lifting that refusal, which is a deliberate security posture and not this
// task's to spend. The POLL stops at the RENDERER, though: the last hop into the iframe IS a push
// (`PanelPortBridge.deliver`), because a sandboxed frame can be posted to.
//
// ⚠ THE FAN-OUT IS PER PACKAGE, NOT PER PANEL, AND THE BUFFER IS DRAINED EXACTLY ONCE PER TICK. The
// Shell buffers per PACKAGE (its session is pooled per package), so a package with two panels open
// has ONE mailbox. Draining it per panel would give panel #1 every event and panel #2 nothing — a
// lost-fact bug that only appears once a user opens a package's second panel. So this polls once per
// package and delivers the SAME batch to each of that package's live ports.
//
// ⚠ THE LOUD HALF TRAVELS WITH THE EVENTS, and dropping it here would undo the whole point of
// bounding the buffer. `dropped` / `gapped` say "your cursor is worthless — re-snapshot"; a panel that
// missed events looks EXACTLY like a panel whose subject did not change, so a batch delivered without
// them leaves the package rendering stale data forever and blaming itself. They therefore ride the
// SAME envelope as the events, never a second channel a package could forget to listen on.

import { BridgeError, isRecord } from "./bridge.js";
import type { ShellBridge } from "./bridge.js";

/**
 * The Shell method this pump drains (M9 e13c-2).
 *
 * MIRRORS C++ `kPanelEventsPollMethod` (`src/editor/shell/include/context/editor/shell/package_events.h`),
 * exactly as `PANEL_DAEMON_CALL_METHOD` mirrors `kPanelDaemonCallMethod`. A rename on either side
 * leaves editor-core polling a method the Shell no longer routes and a package's events stop arriving
 * with NOTHING reporting it — the same silent-freeze class `session_bridge.h` records.
 */
export const PANEL_EVENTS_POLL_METHOD = "panel.events.poll";

/**
 * The verb every delivered batch carries INSIDE the iframe.
 *
 * NO `bridge.` PREFIX — the two-directions rule panelverbs.ts states: `bridge.*` travels PANEL → HOST,
 * everything the host sends DOWN the port does not (`commands.invoke` is the other). A package listens
 * for this on its port and applies the batch.
 */
export const PANEL_EVENTS_DELIVER_VERB = "events.deliver";

/** One drained batch, as the Shell reports it and as a panel receives it. */
export interface PackageEventBatch {
    /** The daemon's own `{seq, incarnationId, generation, topic, payload}` envelopes, plus `subId`. */
    readonly events: readonly unknown[];
    /** How many events THIS EDITOR discarded (its bound was reached). 0 for a daemon-side gap. */
    readonly dropped: number;
    /** Was anything lost at all? TRUE ⇒ the cursor is worthless; re-snapshot. */
    readonly gapped: boolean;
}

/** What the pump needs from the panel host. A seam, so the pump is drivable with no DOM at all. */
export interface PackageEventTargets {
    /** The package ids with at least one LIVE port panel mounted, deduplicated. */
    packages(): readonly string[];
    /** Push one batch into every live port of `packageId`. Returns how many ports took it. */
    deliver(packageId: string, batch: PackageEventBatch): number;
}

/**
 * Read one `panel.events.poll` reply. TOTAL — a member that is not the shape we need is defaulted, not
 * coerced, in the discipline `panels.ts` establishes.
 *
 * ⚠ `gapped` DEFAULTS TO FALSE AND `dropped` TO 0, WHICH IS THE FAIL-**OPEN** DIRECTION — deliberately,
 * and only because the reply comes from the SHELL, the trusted side of this bridge. Defaulting
 * `gapped` to TRUE would make every malformed or older-Shell reply order a re-snapshot, i.e. turn an
 * absent field into a permanent resubscribe loop. The untrusted direction (a panel's request) is
 * parsed fail-CLOSED elsewhere; this one is a host reading its own host.
 */
export function parsePackageEventBatch(value: unknown): PackageEventBatch | null {
    if (!isRecord(value) || !Array.isArray(value["events"])) {
        return null;
    }
    const dropped = value["dropped"];
    return {
        events: value["events"],
        dropped: typeof dropped === "number" && Number.isFinite(dropped) ? dropped : 0,
        gapped: value["gapped"] === true,
    };
}

/**
 * Drain each mounted package's Shell-side buffer once per tick and push the batch into its frames.
 *
 * NEVER THROWS out of `poll()`: it runs on a `setInterval` in a renderer whose only diagnostic channel
 * is a DOM attribute, so an escaping rejection is an unhandled error nobody sees. A `BridgeError` (an
 * older Shell with no `panel.events.poll` route) ends that package's poll for this tick — the fan-out
 * is ABSENT, not broken, and every other panel keeps working.
 */
export class PackageEventPump {
    readonly #bridge: ShellBridge;
    readonly #targets: PackageEventTargets;
    #delivered = 0;
    #dropped = 0;
    #gaps = 0;
    #polls = 0;

    constructor(bridge: ShellBridge, targets: PackageEventTargets) {
        this.#bridge = bridge;
        this.#targets = targets;
    }

    /** Events pushed into a frame across every poll (one event delivered to two ports counts twice). */
    get delivered(): number {
        return this.#delivered;
    }

    /** Events the SHELL reported discarding — the editor-side half of the loud pair, accumulated. */
    get dropped(): number {
        return this.#dropped;
    }

    /** Batches that arrived `gapped` (an overflow HERE or a gap in the daemon's own ring). */
    get gaps(): number {
        return this.#gaps;
    }

    /** Completed poll rounds. An idle editor with no package panels never increments it. */
    get polls(): number {
        return this.#polls;
    }

    /**
     * One round: poll every mounted package's buffer and deliver what came back. Returns how many
     * events were pushed into a frame in THIS round.
     *
     * A package whose batch is empty AND clean is not delivered at all — an idle package must not cost
     * its panels a message per tick. But an empty batch that is `gapped` IS delivered: "you missed
     * events" is a fact even when no event survived to carry it, and swallowing it is exactly how a
     * panel ends up silently stale.
     */
    async poll(): Promise<number> {
        const packages = this.#targets.packages();
        if (packages.length === 0) {
            return 0;
        }
        this.#polls += 1;
        let delivered = 0;
        for (const packageId of packages) {
            let reply: unknown;
            try {
                reply = await this.#bridge.call(PANEL_EVENTS_POLL_METHOD, { packageId });
            } catch (error) {
                if (error instanceof BridgeError) {
                    continue; // no route / a refused package — absent, not broken
                }
                continue; // a transport fault; the next tick tries again
            }
            const batch = parsePackageEventBatch(reply);
            if (batch === null) {
                continue;
            }
            this.#dropped += batch.dropped;
            if (batch.gapped) {
                this.#gaps += 1;
            }
            if (batch.events.length === 0 && !batch.gapped) {
                continue; // idle: no message, no cost
            }
            const ports = this.#targets.deliver(packageId, batch);
            const pushed = batch.events.length * ports;
            delivered += pushed;
            this.#delivered += pushed;
        }
        return delivered;
    }
}
