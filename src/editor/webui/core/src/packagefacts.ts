// THE PACKAGE FACT PUBLISH CHANNEL, editor-core's half (editor-UX d2, D4/D5; design 02 §C / 05 §3).
//
// THE CHAIN THIS MODULE CLOSES, and it is deliberately the SHORTEST of the three package channels:
//
//     a panel's `bridge.facts.publish`  ->  THIS  ->  Shell `panel.facts.publish`
//                                                     (the events.publishes[] check, package_facts.h)
//                                                     ->  daemon `events.publish` (retain + dedup)
//                                                     ->  every consented subscriber's own stream
//
// The RETURN direction is not here and must not be looked for here: a published fact comes BACK — to
// this package and to every other consented one — over the existing `panel.events.poll` fan-out
// (`packageevents.ts`), as an ordinary `events.deliver` batch. That is D4's "no new transport" in
// one sentence: publishing is a request/response call, delivery is the subscription that already
// exists, and nothing in this file knows how a fact is delivered.
//
// ⚠ WHY A DEDICATED VERB RATHER THAN `bridge.call("events.publish", …)`. This is the security
// content of D4 and not an ergonomics preference. `bridge.call` forwards its `method` AND `params`
// VERBATIM to the Shell's allowlist (panelverbs.ts § the fan-in note), so putting `events.publish`
// on that allowlist would let a panel publish on ANY topic the daemon had registered — including
// another package's — with the Shell's "did that package declare that topic" check standing beside
// the path rather than on it. The allowlist therefore stays closed and this is a separate route
// whose Shell handler does the declaration check before anything reaches a session.
//
// ⚠ A FACT IS A STATE, AND `changed:false` IS A SUCCESS. The daemon retains the last value per topic
// and refuses a repeat (D5 rule 1 — the cycle breaker; two packages hold different daemon origins,
// so origin echo suppression cannot break an A -> B -> A mirror and only state dedup can). A
// publisher therefore learns whether its fact MOVED from the reply's `changed`, never from watching
// for its own event — which is design 05 §1's third rule, applied to a new topic family. The
// accepted cost, so no caller reports it as a bug: a package CANNOT send pure edge events; the
// second identical publish deduplicates. Model an edge as state (a counter, a token).

import { BridgeError } from "./bridge.js";
import type { ShellBridge } from "./bridge.js";

/**
 * The Shell method this publishes over (editor-UX d2).
 *
 * MIRRORS C++ `kPanelFactsPublishMethod`
 * (`src/editor/shell/include/context/editor/shell/package_facts.h`), exactly as
 * `PANEL_EVENTS_POLL_METHOD` mirrors `kPanelEventsPollMethod` — and byte-compared against it out of
 * the BUILT bundle by `tools/check_webui_assets.py --panel-contract`. A rename on either side leaves
 * editor-core calling a method the Shell no longer routes; `PanelPortBridge` maps that
 * `unknown_method` onto `verb_not_granted`, which a package cannot tell apart from "this build has
 * no fact bus", so every publisher would go silent with NOTHING reporting it.
 */
export const PANEL_FACTS_PUBLISH_METHOD = "panel.facts.publish";

/**
 * What one publish answered. `ok:false` carries the Shell's or the daemon's own machine code.
 *
 * MIRRORS `PanelFactOutcome` (panelverbs.ts) STRUCTURALLY rather than importing it, exactly as
 * `PackageUiSubscribeResult` mirrors `PanelUiSubscribeOutcome`: the SHAPE travels between the two
 * layers, the transport does not. Importing it here would give the verb table a dependency on this
 * module's `ShellBridge`, which is the coupling `panelverbs.ts` is transport-free to avoid.
 */
export interface PanelFactOutcome {
    readonly ok: boolean;
    /** The daemon's `{topic, changed, seq}` on success. */
    readonly result?: unknown;
    /** The refusal's machine-readable code (`panel.facts.topic_not_declared`, `package.fact_reentrant`, …). */
    readonly code?: string;
    readonly message?: string;
}

/** Publish one fact on `topic`. The seam `panelverbs.ts` closes a package over. */
export type PanelFactPublish = (topic: string, payload: unknown) => Promise<PanelFactOutcome>;

/**
 * Bind ONE package's fact publisher (editor-UX d2).
 *
 * ⚠ `packageId` IS AN ARGUMENT OF THIS FACTORY, NOT OF THE RETURNED FUNCTION — the same structural
 * property `makePackageDaemonCall` has, and the same reason: every sandboxed frame reports
 * `event.origin === "null"`, so a CLOSURE is the only thing that can carry a package's identity.
 * `bridge.facts.publish` takes no package argument because THIS FUNCTION is the scope, so no request
 * can publish under another package's name — and the Shell then checks that name against the
 * manifest it read, never against anything the panel sent.
 *
 * TOTAL — it never rejects, because `bridge.facts.publish`'s handler must turn every outcome into a
 * `PanelVerbRefusal`; an escaping rejection would reach `PanelPortBridge.#invoke`'s generic
 * host-fault path and tell the package nothing — least of all which of the five daemon-side refusals
 * it hit.
 */
export function makePackageFactPublish(bridge: ShellBridge, packageId: string): PanelFactPublish {
    return async (topic: string, payload: unknown): Promise<PanelFactOutcome> => {
        try {
            const result = await bridge.call(PANEL_FACTS_PUBLISH_METHOD, {
                packageId,
                topic,
                payload,
            });
            return { ok: true, result };
        } catch (error) {
            if (error instanceof BridgeError) {
                // The Shell's / daemon's own code, RELAYED rather than re-classified: the five
                // refusals (`topic_not_declared`, `topic_not_namespaced`, `topic_undeclared`,
                // `fact_too_large`, `fact_reentrant`) each need a different fix, and collapsing them
                // here would make all five read as "it did not work" — exactly the state D5 rule 3's
                // diagnostic exists to lift an author out of.
                return { ok: false, code: error.reason, message: error.message };
            }
            // Not a refusal the Shell authored — a client-side transport/shape fault. Its message is
            // renderer state, so it is NOT echoed (the discipline `PanelPortBridge.#invoke` applies).
            return { ok: false, code: "bridge.transport", message: "the Shell could not be reached" };
        }
    };
}
