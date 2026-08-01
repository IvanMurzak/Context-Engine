// THE `editor.ui` FAN-OUT TO PACKAGE PANELS (M9 e13c-4, design 04 §5 / 05 §4 / 08 §2, C-F18).
//
// WHAT THIS CLOSES. `bridge.ui.subscribe` shipped HARD-DENIED from e13b-2 with a comment naming this
// task: the gate was real (a grant lookup, not a hardcoded refusal) but the GRANTED half answered
// "editor.ui delivery to package panels is not wired in this build". This module is that delivery, and
// it is built on the push path e13c-2 already laid rather than on a new one:
//
//     EditorUiBus.publish  ->  THIS fan-out  ->  PanelHost.deliverToPackage  ->  PanelPortBridge.deliver
//                                                                                (postMessage into the frame)
//
// The last two hops are `packageevents.ts`'s, verbatim — the SAME `deliverToPackage` that pushes
// `events.deliver` batches out of the Shell's bounded buffer. Only the SOURCE differs, and that
// difference is the whole design: daemon facts come from the Shell over a poll, `editor.ui` facts are
// LOCAL to this window's bus and never touch the daemon (D7 — `check_ui_bus_boundary.py` is the
// standing gate on that boundary, and nothing here reaches a bridge).
//
// ⚠ WHY THIS IS NOT A SECOND POLL. `packageevents.ts` polls because the e05c bridge accepts no
// persistent queries, so the Shell cannot push into the renderer at all. That constraint does not
// apply here: the bus is IN this document. Subscribing to it directly is not a shortcut around a
// security posture — it is the absence of a transport that would need one.
//
// ================================ THE THREE CONTROLS ================================
//
//  1. THE CAPABILITY IS CHECKED BEFORE THIS MODULE IS REACHED, AND NOWHERE INSIDE IT. `panelverbs.ts`
//     calls `requireCapability` at the ONE named enforcement point and only then invokes the seam this
//     module implements. A second check here would be a second thing to keep in sync, and a drift
//     between the two reads as a grant — the reasoning `panelverbs.ts` states for the Shell's
//     `bridge.call` allowlist, applied one layer over. So: no grant lookup in this file, deliberately.
//
//  2. THE TOPIC VOCABULARY IS CLOSED, AND SUBSCRIBING IS ALL-OR-NOTHING. Only `BUILTIN_UI_TOPICS` are
//     subscribable. A package's OWN declared topics (`declareTopics`, uibus.ts) are a PUBLISH surface —
//     letting a package subscribe to another package's namespaced topic is precisely the
//     "builds a cross-package channel via `editor.ui`" half of C-F18, and there is no consent question
//     that could make it safe because the OTHER package never agreed. One unknown topic refuses the
//     WHOLE request rather than silently accepting the rest: a partial subscription looks identical,
//     from inside the frame, to a full one that never fires.
//
//  3. THE FAN-OUT IS PER PACKAGE, exactly as e13c-2's is. A package with two panels open has ONE
//     subscription and both frames get the SAME envelope, because `deliverToPackage` addresses every
//     live port of that package. Subscribing per PANEL would give panel #1 every fact and panel #2
//     nothing the moment a user opened a package's second panel — the lost-fact bug `packageevents.ts`
//     names, and it would be reintroduced here for free by keying on `panelId`.
//
// ⚠ SUBSCRIBING DELIVERS THE RETAINED SNAPSHOT IMMEDIATELY — inherited from `EditorUiBus.subscribe`,
// and DELIBERATELY not suppressed. A package panel mounted mid-session would otherwise draw nothing
// until the next change on each topic (the current theme, the current focus), which is the same
// stale-until-something-happens failure `snapshot-on-subscribe` exists to prevent for the editor's own
// subscribers. The fact it receives is one the grant already entitles it to — the very next publish
// would carry it — so this is a latency property, not a wider grant. It is asserted explicitly in the
// T1 tier because a reader of THIS file alone would not predict a delivery from a `subscribe` call.
//
// ⚠ WHAT A DELIVERED ENVELOPE CARRIES, AND WHY IT IS THE BUS'S OWN. `EditorUiEvent` is `{seq, topic,
// origin, payload}` and it travels WHOLE. `origin` is public (it is a window id, or the literal
// `"shell"` for a write-notice) and `seq` is this window's own counter, so neither conveys anything a
// panel could not observe by watching the editor; and stripping either would leave a package unable to
// order or de-duplicate facts, which is what the field exists for. Nothing project-authored rides here
// — the built-in topics carry chrome facts (focus, layout, drag, viewport, theme, palette, the write
// notice), which is the tier D7 defines them as.

import type { EditorUiBus, EditorUiEvent, EditorUiSubscription } from "./uibus.js";
import { BUILTIN_UI_TOPICS } from "./uibus.js";

/**
 * The verb every delivered `editor.ui` fact carries INSIDE the iframe.
 *
 * NO `bridge.` PREFIX — the two-directions rule `panelverbs.ts` states: `bridge.*` travels PANEL ->
 * HOST, everything the host sends DOWN the port does not. Its siblings are `events.deliver` (daemon
 * facts) and `commands.invoke`, and the names are deliberately parallel so a package author reading
 * one envelope can tell which stream it is.
 */
export const PANEL_UI_DELIVER_VERB = "ui.deliver";

/**
 * How many `editor.ui` topics ONE `bridge.ui.subscribe` REQUEST may name.
 *
 * The built-in vocabulary is closed and small, so this can never bind on a well-behaved package — it
 * exists because `subscribe` is reachable from untrusted code in a process that stays up for days, and
 * a per-call array with no ceiling is an unbounded allocation an attacker chooses. Sized to the
 * vocabulary rather than to a round number, so widening the vocabulary without revisiting this cannot
 * silently make it binding.
 *
 * ⚠ IT BOUNDS THE REQUEST, NOT THE HELD SET, AND THAT IS THE ONLY VERSION THAT BOUNDS ANYTHING. Every
 * requested topic is checked against the closed vocabulary, so the MERGED set is already bounded by
 * that vocabulary's size and a ceiling over it can never be reached — it would read as a protection
 * while admitting the very allocation this names. `subscribe` states the ordering that makes it real.
 */
export const PACKAGE_UI_TOPIC_LIMIT = BUILTIN_UI_TOPICS.length;

/** What one `bridge.ui.subscribe` answered. `diagnostic` is empty exactly when it was accepted. */
export interface PackageUiSubscribeResult {
    /** The topics now subscribed for this package, in request order. Empty on a refusal. */
    readonly topics: readonly string[];
    /** Why the request was refused, or `""`. `panelverbs.ts` turns a non-empty one into a refusal. */
    readonly diagnostic: string;
}

/** What the fan-out needs from the panel host. A seam, so it is drivable with no DOM at all. */
export interface PackageUiTargets {
    /** Push one envelope into every live port of `packageId`. Returns how many ports took it. */
    deliver(packageId: string, event: EditorUiEvent): number;
}

/**
 * Is `topic` subscribable by a package? Control 2's whole decision, exposed so the T1 tier drives it
 * directly rather than only through a subscription.
 */
export function isSubscribableUiTopic(topic: string): boolean {
    return (BUILTIN_UI_TOPICS as readonly string[]).includes(topic);
}

/**
 * The `editor.ui` -> package-panel fan-out for ONE window.
 *
 * ONE bus listener per TOPIC no matter how many packages want it (not one per package per topic): the
 * bus delivers to every listener on a per-publish path, and a listener count that grew with the
 * package count would make an ordinary editor's frame cost a function of how many panels are open.
 */
export class PackageUiFanout {
    readonly #bus: EditorUiBus;
    readonly #targets: PackageUiTargets;
    /** package id -> the topics it subscribed to. The addressing table control 3 keys on. */
    readonly #byPackage = new Map<string, Set<string>>();
    /** topic -> this window's ONE bus subscription for it, held so it can be disposed. */
    readonly #busSubscriptions = new Map<string, EditorUiSubscription>();
    #delivered = 0;
    #refused = 0;

    constructor(bus: EditorUiBus, targets: PackageUiTargets) {
        this.#bus = bus;
        this.#targets = targets;
    }

    /** Envelopes pushed into a frame (one fact delivered to two ports counts twice). */
    get delivered(): number {
        return this.#delivered;
    }

    /** Subscribe requests refused — an unknown topic, an empty list, or the per-package cap. */
    get refused(): number {
        return this.#refused;
    }

    /** How many packages currently hold a subscription. */
    get packages(): number {
        return this.#byPackage.size;
    }

    /** The topics `packageId` holds, sorted — the observable a case asserts an exact value against. */
    topicsFor(packageId: string): readonly string[] {
        return [...(this.#byPackage.get(packageId) ?? [])].sort();
    }

    /**
     * Subscribe `packageId` to `topics` (control 2: closed vocabulary, all-or-nothing).
     *
     * ⚠ ADDITIVE ACROSS CALLS AND IDEMPOTENT PER TOPIC. A package that subscribes to focus and then to
     * layout holds both; one that re-subscribes to focus holds it once. Replacing the set instead
     * would make a second `subscribe` silently CANCEL the first, which is a lost-fact bug a package
     * author has no way to see.
     */
    subscribe(packageId: string, topics: readonly string[]): PackageUiSubscribeResult {
        if (topics.length === 0) {
            this.#refused += 1;
            return { topics: [], diagnostic: "bridge.ui.subscribe requires at least one topic" };
        }
        // ⚠ THE REQUEST IS BOUNDED HERE, BEFORE ANYTHING IS ALLOCATED FROM IT, AND THAT ORDER IS THE
        // WHOLE PROTECTION. The vocabulary loop below is total, so a surviving request names only
        // built-in topics — but it may name ARBITRARILY MANY COPIES of one, and merging spreads every
        // entry into a fresh array before a `Set` collapses them. Bounding the MERGED set instead
        // bounds a value the vocabulary loop already bounds to the closed vocabulary's size, so it can
        // never refuse anything: it reads as a ceiling while admitting the allocation it names.
        // A legitimate request cannot exceed the vocabulary, since it names DISTINCT built-in topics.
        if (topics.length > PACKAGE_UI_TOPIC_LIMIT) {
            this.#refused += 1;
            return {
                topics: [],
                diagnostic:
                    `a bridge.ui.subscribe request may name at most ` +
                    `${String(PACKAGE_UI_TOPIC_LIMIT)} editor.ui topics`,
            };
        }
        for (const topic of topics) {
            if (!isSubscribableUiTopic(topic)) {
                this.#refused += 1;
                // The diagnostic NAMES the topic and says what the surface is, because the most likely
                // cause is a package trying its own namespaced topic — which is refused by design, not
                // by omission, and an author who cannot tell those apart files the wrong bug.
                return {
                    topics: [],
                    diagnostic:
                        `"${topic}" is not a subscribable editor.ui topic (a package may subscribe ` +
                        `only to the built-in topics: ${BUILTIN_UI_TOPICS.join(", ")})`,
                };
            }
        }
        const held = this.#byPackage.get(packageId) ?? new Set<string>();
        const added = topics.filter((topic) => !held.has(topic));
        this.#byPackage.set(packageId, new Set<string>([...held, ...topics]));
        for (const topic of added) {
            if (this.#busSubscriptions.has(topic)) {
                // ⚠ ALREADY ATTACHED FOR ANOTHER PACKAGE, so the bus listener this package would have
                // inherited its RETAINED SNAPSHOT from is already installed and `EditorUiBus.subscribe`
                // — the only thing that replays that snapshot — will not run again. Deliver it here, to
                // the JOINING package alone, or the second and every later subscriber to a topic draws
                // nothing until the next publish: exactly the stale-until-something-happens failure the
                // header's snapshot-on-subscribe property exists to prevent.
                const snapshot = this.#bus.snapshot(topic);
                if (snapshot !== undefined) {
                    try {
                        this.#delivered += this.#targets.deliver(packageId, snapshot);
                    } catch {
                        // A dead port is an ordinary outcome, exactly as in `#fanOut`.
                    }
                }
            } else {
                // FIRST holder of this topic — `#byPackage` was written above, so the bus's own
                // snapshot replay reaches this package through `#fanOut`. Delivering here as well
                // would double it. (`dispose` drops a bus subscription exactly when no package holds
                // the topic, so "not attached" really does mean "nobody holds it".)
                this.#attach(topic);
            }
        }
        return { topics: [...topics], diagnostic: "" };
    }

    /**
     * Drop everything `packageId` holds.
     *
     * ⚠ DELIBERATELY NOT WIRED TO PER-PANEL TEARDOWN, and the reason is control 3. `PanelVerbTable`'s
     * `dispose` runs when ONE panel closes, while a subscription belongs to the PACKAGE — so calling
     * this from there would silently cancel the stream of a package's OTHER open panel, which is the
     * lost-fact bug control 3 exists to prevent, reintroduced by the cleanup rather than by the
     * delivery. Delivery to a package with no live port already costs nothing (`deliverToPackage`
     * addresses live ports only and answers 0), so an un-disposed subscription is inert rather than
     * leaky. This exists for the caller that knows a package is GONE — an uninstall, or a window
     * teardown — and for the T1 tier, which is what pins that the bus listener is released with it.
     *
     * The bus listener for a topic nobody holds any more IS disposed: the fan-out outlives every panel,
     * so a listener kept for a departed package would keep this window doing work on every publish.
     */
    dispose(packageId: string): void {
        if (!this.#byPackage.delete(packageId)) {
            return;
        }
        for (const [topic, subscription] of [...this.#busSubscriptions]) {
            if (!this.#anyPackageHolds(topic)) {
                subscription.dispose();
                this.#busSubscriptions.delete(topic);
            }
        }
    }

    #anyPackageHolds(topic: string): boolean {
        for (const topics of this.#byPackage.values()) {
            if (topics.has(topic)) {
                return true;
            }
        }
        return false;
    }

    #attach(topic: string): void {
        if (this.#busSubscriptions.has(topic)) {
            return;
        }
        this.#busSubscriptions.set(
            topic,
            this.#bus.subscribe(topic, (event: EditorUiEvent): void => {
                this.#fanOut(event);
            }),
        );
    }

    #fanOut(event: EditorUiEvent): void {
        for (const [packageId, topics] of this.#byPackage) {
            if (!topics.has(event.topic)) {
                continue;
            }
            // NEVER THROWS OUT OF A BUS LISTENER: this runs inside `EditorUiBus.publish`, whose local
            // fan-out is synchronous, so an escaping error would take down the PUBLISHER — an editor
            // action failing because a third-party panel's port misbehaved. `deliverToPackage` already
            // contains its own `postMessage` faults; this guard is the belt for anything it cannot.
            try {
                this.#delivered += this.#targets.deliver(packageId, event);
            } catch {
                // A dead port is an ordinary outcome, not a fault to report on a per-publish path.
            }
        }
    }
}
