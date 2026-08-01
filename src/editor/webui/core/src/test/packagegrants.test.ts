// T1 cases for the CAPABILITY GRANT SOURCE and the `editor.ui` FAN-OUT (M9 e13c-4 — packagegrants.ts
// + packageui.ts, design 08 §2-§3 / 04 §5, C-F18).
//
// WHY BOTH MODULES IN ONE FILE. They are the two halves of one claim — "a package holds exactly what
// the operator consented to, and a package that holds `ui_events` actually RECEIVES `editor.ui` facts"
// — and the second half is only meaningful against the first. Splitting them would put the grant's
// positive counterpart in a file that never sees a grant.
//
// ⚠ THE DIRECTION THAT MATTERS. Every failure path of `ShellPackageGrants` must DENY: a Shell with no
// route, a refused call, a reply of the wrong shape. So each of those cases asserts `granted() ===
// false`, and each is paired with the same fixture SUCCEEDING — otherwise "it denies" would be
// satisfied by a class that denies unconditionally, which is exactly the object it replaces.
//
// ⚠ THE DELIVERY HALF IS ASSERTED ON THE ARTIFACT, NEVER ON AN ABSENCE. "the un-granted package got no
// facts" is satisfied for free by a bus that published nothing, a fan-out that subscribed nothing, or a
// harness whose ports were never wired. Every delivery case therefore asserts the EXACT envelope that
// arrived at a named package, and the negative half is measured in the SAME publish as a positive one —
// so a publish that reached nobody fails the case rather than passing it.

import { assert, assertEqual, type TestCase } from "./harness.js";
import { BridgeError } from "../bridge.js";
import type { ShellBridge } from "../bridge.js";
import {
    PACKAGE_GRANTS_DECIDE_METHOD,
    PACKAGE_GRANTS_LIST_METHOD,
    ShellPackageGrants,
    parsePackageGrants,
} from "../packagegrants.js";
import {
    PACKAGE_UI_TOPIC_LIMIT,
    PANEL_UI_DELIVER_VERB,
    PackageUiFanout,
    isSubscribableUiTopic,
} from "../packageui.js";
import { CAPABILITY_UI_EVENTS, DENY_ALL_CAPABILITY_GRANTS, capabilityDenial } from "../panelverbs.js";
import {
    BUILTIN_UI_TOPICS,
    EditorUiBus,
    UI_TOPIC_FOCUS,
    UI_TOPIC_LAYOUT,
    UI_TOPIC_THEME_CHANGED,
} from "../uibus.js";
import type { EditorUiEvent } from "../uibus.js";

// ------------------------------------------------------------------------------------- the fixtures

/** The `package.grants.list` reply shape the C++ `PackageGrantHost::list` really emits. */
function listReply(
    packages: readonly {
        id: string;
        version?: string;
        requested?: readonly string[];
        granted?: readonly string[];
        decided?: boolean;
    }[],
): unknown {
    return {
        packages: packages.map((entry) => ({
            id: entry.id,
            version: entry.version ?? "1.0.0",
            requested: entry.requested ?? [],
            granted: entry.granted ?? [],
            decided: entry.decided ?? true,
        })),
    };
}

/** A `ShellBridge` stand-in that answers ONE method and records what it was asked. */
function stubBridge(answer: () => Promise<unknown>): { bridge: ShellBridge; asked: string[] } {
    const asked: string[] = [];
    const bridge = {
        call: (method: string): Promise<unknown> => {
            asked.push(method);
            return answer();
        },
    };
    return { bridge: bridge as unknown as ShellBridge, asked };
}

/** A delivery target that records every push, so a case asserts WHO got WHAT rather than a count. */
interface DeliverySpy {
    readonly deliveries: { packageId: string; event: EditorUiEvent }[];
    readonly targets: { deliver(packageId: string, event: EditorUiEvent): number };
}
function deliverySpy(portsPerPackage = 1): DeliverySpy {
    const deliveries: { packageId: string; event: EditorUiEvent }[] = [];
    return {
        deliveries,
        targets: {
            deliver: (packageId: string, event: EditorUiEvent): number => {
                deliveries.push({ packageId, event });
                // Mirrors `PanelHost.deliverToPackage(...).delivered` — how many LIVE PORTS took it.
                return portsPerPackage;
            },
        },
    };
}

// ------------------------------------------------------------------------------------------ cases

export const packageGrantsTests: readonly TestCase[] = [
    // ------------------------------------------------------- the reply parse (fail-closed, whole)
    {
        // ⚠ PLANT: make `parseRecord` DEFAULT a missing/ill-typed `granted` to `[]` instead of
        // returning null. The "one malformed entry discards the whole reply" assertions below go RED
        // — and they are the ones that matter, because a partial table looks healthy while hiding a
        // Shell/bundle version skew.
        name: "packagegrants: the list reply parses whole, and ONE bad entry discards ALL of it",
        run: () => {
            const parsed = parsePackageGrants(
                listReply([
                    { id: "ext.hello", requested: ["read_query", "ui_events"], granted: ["ui_events"] },
                    { id: "ext.quiet", requested: [], granted: [], decided: false },
                ]),
            );
            assert(parsed !== null, "a well-formed reply parses");
            assertEqual(parsed?.length, 2, "both entries survive");
            assertEqual(parsed?.[0]?.id, "ext.hello", "the id is carried");
            assertEqual(parsed?.[0]?.granted, ["ui_events"], "…and the EXACT granted list");
            assertEqual(
                parsed?.[0]?.requested,
                ["read_query", "ui_events"],
                "…and the requested list, which the consent surface renders",
            );
            assertEqual(parsed?.[1]?.decided, false, "`decided` is carried, not inferred from granted");

            // FAIL-CLOSED, ALL-OR-NOTHING. Each of these has a HEALTHY sibling entry in the same
            // reply, so a parser that skipped the bad one would return a 1-entry table and pass a
            // laxer assertion; `null` is what proves the discard is total.
            //
            // ⚠ EVERY BROKEN ENTRY CARRIES A VALID `requested` AND `granted` EXCEPT THE ONE MEMBER
            // UNDER TEST, AND THAT IS LOAD-BEARING RATHER THAN TIDINESS. Measured by this task's plant
            // round: the fixtures originally OMITTED `requested` on four of these, so the absent
            // member decided every one of them and the plant "make `isStringArray` accept a non-string
            // element" scored GREEN — four sub-cases that could not fail on the rule they name. A
            // one-member mutation of an otherwise-valid entry is the only shape that discriminates.
            const healthy = { id: "ext.hello", requested: ["ui_events"], granted: ["ui_events"] };
            for (const broken of [
                { packages: [healthy, { id: "", requested: [], granted: [] }] },
                { packages: [healthy, { requested: [], granted: [] }] },
                { packages: [healthy, { id: "ext.bad", requested: [], granted: "ui_events" }] },
                { packages: [healthy, { id: "ext.bad", requested: [], granted: [7] }] },
                { packages: [healthy, { id: "ext.bad", requested: 3, granted: [] }] },
                { packages: [healthy, { id: "ext.bad", requested: [7], granted: [] }] },
                { packages: [healthy, "ext.bad"] },
            ]) {
                assertEqual(
                    parsePackageGrants({
                        packages: broken.packages.map((entry) =>
                            typeof entry === "string" ? entry : { ...entry },
                        ),
                    }),
                    null,
                    `a malformed entry discards the whole reply: ${JSON.stringify(broken)}`,
                );
            }
            // …and a reply that is not the shape at all.
            for (const bad of [undefined, null, 7, "packages", [], {}, { packages: 3 }]) {
                assertEqual(parsePackageGrants(bad), null, `refused: ${JSON.stringify(bad) ?? "undefined"}`);
            }
        },
    },
    {
        // ⚠ PLANT: make `granted()` fall back to `true` for an unknown package (or drop the `?.has`
        // guard so an unknown package throws and a caller "helpfully" catches it as granted). The
        // unknown-package and unknown-capability halves below go RED.
        name: "packagegrants: granted() answers for the EXACT pair, and denies everything else",
        run: () => {
            const grants = ShellPackageGrants.fromRecords([
                {
                    id: "ext.hello",
                    version: "1.0.0",
                    requested: ["read_query", "ui_events"],
                    granted: ["ui_events"],
                    decided: true,
                },
            ]);
            // THE POSITIVE, first: without it every denial below is satisfied by `empty()`.
            assert(grants.granted("ext.hello", CAPABILITY_UI_EVENTS), "the granted pair answers TRUE");
            // …and each way of being a DIFFERENT pair.
            assert(!grants.granted("ext.hello", "file_write"), "a capability it was NOT granted");
            assert(!grants.granted("ext.other", CAPABILITY_UI_EVENTS), "another package's grant");
            assert(!grants.granted("", CAPABILITY_UI_EVENTS), "an empty package id");
            assert(!grants.granted("ext.hello", ""), "an empty capability");
            // A REQUESTED capability is not a granted one — the manifest-is-not-a-grant rule, carried
            // through this class: `read_query` is on `requested` and NOT on `granted`.
            assert(
                !grants.granted("ext.hello", "read_query"),
                "a REQUESTED capability that was not granted stays denied",
            );
            assertEqual(grants.records.length, 1, "the records are exposed for a consent surface");

            // It really satisfies the gate's own predicate — driven through `capabilityDenial`, the
            // production function `requireCapability` calls, rather than through this class's own API.
            assertEqual(
                capabilityDenial(grants, "ext.hello", CAPABILITY_UI_EVENTS, ["ui_events"]),
                "",
                "the real gate predicate reports NO denial for a granted package",
            );
            assert(
                capabilityDenial(grants, "ext.other", CAPABILITY_UI_EVENTS, ["ui_events"]) !== "",
                "…and a denial for one that holds nothing",
            );
            // The deny-all floor is unchanged and still denies the same pair.
            assert(
                !DENY_ALL_CAPABILITY_GRANTS.granted("ext.hello", CAPABILITY_UI_EVENTS),
                "the fallback source grants nothing",
            );
        },
    },
    {
        // ⚠ PLANT: delete the `try`/`catch` in `load` (or return the parsed value without the `null`
        // check). The three failure arms below go RED — and they are the security-relevant ones: each
        // is a state a shipping editor really reaches (an older Shell, a secondary window's router).
        name: "packagegrants: EVERY load failure denies, and the success path is what proves it",
        run: async (): Promise<void> => {
            // THE SUCCESS PATH FIRST — the control the three failures are measured against.
            const good = stubBridge(() =>
                Promise.resolve(listReply([{ id: "ext.hello", granted: ["ui_events"] }])),
            );
            const loaded = await ShellPackageGrants.load(good.bridge);
            assertEqual(good.asked, [PACKAGE_GRANTS_LIST_METHOD], "it asks the Shell for the list");
            assert(loaded.granted("ext.hello", CAPABILITY_UI_EVENTS), "a real grant arrives");

            // (a) The Shell REFUSED the call — no route (an older Shell, or a secondary window).
            const refused = await ShellPackageGrants.load(
                stubBridge(() =>
                    Promise.reject(
                        new BridgeError({
                            code: -32601,
                            message: "unknown method",
                            reason: "unknown_method",
                        }),
                    ),
                ).bridge,
            );
            assert(!refused.granted("ext.hello", CAPABILITY_UI_EVENTS), "a refused call grants nothing");
            assertEqual(refused.records.length, 0, "…and reports no packages at all");

            // (b) A TRANSPORT fault — not a BridgeError. The same answer, deliberately: there is no
            //     third behaviour that is safe.
            const faulted = await ShellPackageGrants.load(
                stubBridge(() => Promise.reject(new Error("the pipe went away"))).bridge,
            );
            assert(!faulted.granted("ext.hello", CAPABILITY_UI_EVENTS), "a transport fault grants nothing");

            // (c) A reply of the WRONG SHAPE — the version-skew case.
            const skewed = await ShellPackageGrants.load(
                stubBridge(() => Promise.resolve({ grants: { "ext.hello": ["ui_events"] } })).bridge,
            );
            assert(!skewed.granted("ext.hello", CAPABILITY_UI_EVENTS), "a malformed reply grants nothing");

            // The decide route's NAME is pinned here too: it is the C++ mirror
            // (`kPackageGrantsDecideMethod`), and a drift on either side is a consent answer that
            // silently goes nowhere.
            assertEqual(PACKAGE_GRANTS_DECIDE_METHOD, "package.grants.decide", "the C++ mirror");
            assertEqual(PACKAGE_GRANTS_LIST_METHOD, "package.grants.list", "the C++ mirror");
        },
    },

    // ------------------------------------------------------------------ the editor.ui fan-out
    {
        // ⚠ PLANT: widen `isSubscribableUiTopic` to accept anything starting with `editor.ui.` (or to
        // consult the bus's `topics`, which INCLUDES package-declared ones). The namespaced-topic
        // assertion goes RED — and that is C-F18's cross-package channel, so the case is the control.
        name: "packageui: the subscribable topic vocabulary is CLOSED to the built-ins",
        run: () => {
            for (const topic of BUILTIN_UI_TOPICS) {
                assert(isSubscribableUiTopic(topic), `built-in topic is subscribable: ${topic}`);
            }
            // A package's OWN declared topic is a PUBLISH surface, never a subscribe one: letting a
            // package subscribe to another's namespaced topic IS the cross-package channel C-F18 names,
            // and no consent question could make it safe because the OTHER package never agreed.
            assert(!isSubscribableUiTopic("editor.ui.ext.hello.private"), "a package topic is NOT");
            assert(!isSubscribableUiTopic("editor.ui."), "the bare prefix is NOT");
            assert(!isSubscribableUiTopic("editor.ui.focus "), "a near-miss spelling is NOT");
            assert(!isSubscribableUiTopic(""), "the empty topic is NOT");
        },
    },
    {
        // ⚠ PLANT (a): drop the `#byPackage` topic check in `#fanOut` (deliver to every subscriber
        //     regardless of topic) -> the "only the subscribed package got it" half goes RED.
        // ⚠ PLANT (b): make `subscribe` return `{topics, diagnostic:""}` WITHOUT registering the bus
        //     listener -> the delivery half goes RED while the reply half stays green, which is why
        //     both are asserted.
        name: "packageui: a subscribed package RECEIVES the fact; an unsubscribed one does not (same publish)",
        run: () => {
            const bus = new EditorUiBus({ origin: "7" });
            const spy = deliverySpy();
            const fanout = new PackageUiFanout(bus, spy.targets);

            assertEqual(
                fanout.subscribe("ext.hello", [UI_TOPIC_FOCUS]),
                { topics: [UI_TOPIC_FOCUS], diagnostic: "" },
                "the subscribe is accepted and echoes what it now holds",
            );
            // A SECOND package on a DIFFERENT topic. This is the discriminating half: it makes the
            // publish below reach a live subscriber that must NOT be addressed, so "nobody got it"
            // cannot be why the negative passes.
            fanout.subscribe("ext.other", [UI_TOPIC_LAYOUT]);
            assertEqual(fanout.packages, 2, "two packages hold subscriptions");
            assertEqual(fanout.topicsFor("ext.hello"), [UI_TOPIC_FOCUS], "the exact held set");

            const report = bus.publish(UI_TOPIC_FOCUS, { panelId: "ext.hello.panel" });
            assert(report.published, "the bus really published (the fixture is not vacuous)");

            // THE POSITIVE ARTIFACT: exactly one delivery, to the subscribed package, carrying the
            // WHOLE envelope. Asserted as an exact object — a `deliveries.length >= 1` would be
            // satisfied by a fan-out that delivered to everyone.
            assertEqual(spy.deliveries.length, 1, "exactly one package was addressed");
            assertEqual(spy.deliveries[0]?.packageId, "ext.hello", "…the SUBSCRIBED one");
            assertEqual(
                spy.deliveries[0]?.event,
                { seq: report.seq, topic: UI_TOPIC_FOCUS, origin: "7", payload: { panelId: "ext.hello.panel" } },
                "the envelope travels WHOLE — seq, topic, origin and payload, exactly as published",
            );
            assertEqual(fanout.delivered, 1, "the running total counts the port that took it");

            // The verb is the one the frame listens for, and it is NOT the daemon-event one.
            // PINNED AS A VALUE, not as an inequality against `events.deliver`: two literal-typed
            // consts have no overlap, so `A !== B` is decided by the COMPILER and the pinned tsgo
            // rejects it outright (TS2367) — an assertion that cannot fail, which is the shape this
            // repo's conventions call out by name. The value IS where a cross-module drift can occur.
            assertEqual(PANEL_UI_DELIVER_VERB, "ui.deliver", "the push verb the package receives on");
        },
    },
    {
        // ⚠ PLANT: make `subscribe` REPLACE the held set instead of merging it -> the "still holds
        // focus" assertion goes RED. That is a lost-fact bug a package author cannot see from inside
        // the frame, which is why it is pinned rather than left to the implementation.
        name: "packageui: a second subscribe ADDS, and the fan-out is per PACKAGE not per panel",
        run: () => {
            const bus = new EditorUiBus({ origin: "0" });
            // TWO live ports for one package — what `PanelHost.deliverToPackage` reports when a user
            // has two of a package's panels open.
            const spy = deliverySpy(2);
            const fanout = new PackageUiFanout(bus, spy.targets);

            fanout.subscribe("ext.hello", [UI_TOPIC_FOCUS]);
            // ⚠ THE SECOND SUBSCRIBE NAMES ONLY THE NEW TOPIC, AND THAT IS WHAT MAKES THIS CASE
            // DISCRIMINATE. Measured by this task's plant round: it originally re-sent
            // `[LAYOUT, FOCUS]`, so a fan-out that REPLACED the held set instead of merging it
            // produced the identical result and the plant scored GREEN — an assertion that could not
            // fail on the rule it names. A merge is only observable when the second request omits
            // something the first held.
            fanout.subscribe("ext.hello", [UI_TOPIC_LAYOUT]);
            assertEqual(
                fanout.topicsFor("ext.hello"),
                [UI_TOPIC_FOCUS, UI_TOPIC_LAYOUT].sort(),
                "both topics are held: the second subscribe did NOT cancel the first",
            );
            // IDEMPOTENT PER TOPIC, asserted separately now that the merge half no longer hides it: a
            // re-subscribe to a topic already held changes nothing and attaches no second listener.
            fanout.subscribe("ext.hello", [UI_TOPIC_FOCUS]);
            assertEqual(
                fanout.topicsFor("ext.hello"),
                [UI_TOPIC_FOCUS, UI_TOPIC_LAYOUT].sort(),
                "a re-subscribe to a held topic is a no-op",
            );

            bus.publish(UI_TOPIC_FOCUS, { a: 1 });
            bus.publish(UI_TOPIC_LAYOUT, { b: 2 });
            assertEqual(spy.deliveries.length, 2, "one fan-out per fact, not one per panel");
            // ONE mailbox, TWO ports: the delivered TOTAL counts ports, so a package with two panels
            // open gets each fact in both frames — the lost-fact bug `packageevents.ts` names, checked
            // here for the sibling stream.
            assertEqual(fanout.delivered, 4, "two facts x two live ports");
        },
    },
    {
        // ⚠ PLANT: accept the good topics and drop only the bad one (a per-topic filter instead of an
        // all-or-nothing refusal) -> the `topicsFor(...).length === 0` assertion goes RED. A partial
        // subscription is indistinguishable, from inside the frame, from a full one that never fires.
        name: "packageui: an unsubscribable topic refuses the WHOLE request, and subscribes nothing",
        run: () => {
            const bus = new EditorUiBus({ origin: "0" });
            const spy = deliverySpy();
            const fanout = new PackageUiFanout(bus, spy.targets);

            const refused = fanout.subscribe("ext.hello", [UI_TOPIC_FOCUS, "editor.ui.ext.hello.secret"]);
            assert(refused.diagnostic !== "", "it is refused");
            assert(
                refused.diagnostic.includes("editor.ui.ext.hello.secret"),
                `the diagnostic NAMES the offending topic: ${refused.diagnostic}`,
            );
            assertEqual(refused.topics, [], "nothing is reported as accepted");
            assertEqual(fanout.topicsFor("ext.hello"), [], "…and nothing was actually subscribed");
            assertEqual(fanout.refused, 1, "the refusal is counted");

            // …and the SAME request minus the bad topic IS accepted, so the refusal is a vocabulary
            // decision rather than a subscribe that never works.
            assertEqual(
                fanout.subscribe("ext.hello", [UI_TOPIC_FOCUS]).diagnostic,
                "",
                "the good half of the same request is accepted on its own",
            );
            bus.publish(UI_TOPIC_FOCUS, { ok: true });
            assertEqual(spy.deliveries.length, 1, "and it really receives facts afterwards");

            // An EMPTY topic list is refused too — a subscription to nothing is a silent no-op the
            // package would wait on forever.
            const empty = fanout.subscribe("ext.hello", []);
            assert(empty.diagnostic !== "", "an empty topic list is refused");

            // The per-package cap. Sized to the vocabulary, so it can only bind on a package asking
            // for a topic twice over the whole built-in set.
            const over = fanout.subscribe(
                "ext.greedy",
                Array.from({ length: PACKAGE_UI_TOPIC_LIMIT + 1 }, (_unused, i) =>
                    i < BUILTIN_UI_TOPICS.length ? BUILTIN_UI_TOPICS[i] ?? "" : "editor.ui.extra",
                ),
            );
            assert(over.diagnostic !== "", "a request past the cap is refused");
            assertEqual(fanout.topicsFor("ext.greedy"), [], "…and subscribes nothing");
        },
    },
    {
        // ⚠ PLANT: make `dispose` clear `#byPackage` but leave the bus subscriptions attached -> the
        // "the bus listener is released" assertion goes RED. Nothing else would notice: delivery stops
        // either way, so only the bus's own listener count can tell a released listener from a
        // retained one that delivers to nobody.
        name: "packageui: dispose releases the package AND its bus listener, and is re-subscribable",
        run: () => {
            const bus = new EditorUiBus({ origin: "0" });
            const spy = deliverySpy();
            const fanout = new PackageUiFanout(bus, spy.targets);
            fanout.subscribe("ext.hello", [UI_TOPIC_THEME_CHANGED]);
            fanout.subscribe("ext.other", [UI_TOPIC_THEME_CHANGED]);

            bus.publish(UI_TOPIC_THEME_CHANGED, { variant: "dark" });
            assertEqual(spy.deliveries.length, 2, "both packages are addressed");

            fanout.dispose("ext.hello");
            assertEqual(fanout.packages, 1, "the disposed package is gone");
            bus.publish(UI_TOPIC_THEME_CHANGED, { variant: "light" });
            // THE POSITIVE HALF of the same publish: the OTHER package still receives, so the listener
            // for this topic was correctly RETAINED while anyone still holds it.
            assertEqual(spy.deliveries.length, 3, "the surviving package still receives");
            assertEqual(spy.deliveries[2]?.packageId, "ext.other", "…and it is the right one");

            fanout.dispose("ext.other");
            bus.publish(UI_TOPIC_THEME_CHANGED, { variant: "dark" });
            assertEqual(spy.deliveries.length, 3, "with nobody subscribed, nothing is delivered");

            // RE-SUBSCRIBABLE: the released listener is re-attached on demand. Without this the
            // assertion above would also pass against a fan-out that had torn itself permanently down.
            //
            // ⚠ AND THE RE-SUBSCRIBE DELIVERS THE RETAINED SNAPSHOT IMMEDIATELY, before any new
            // publish. That is `EditorUiBus.subscribe`'s own contract (snapshot-on-subscribe) reaching
            // a package panel, and it is the behaviour that makes a panel mounted mid-session show the
            // CURRENT theme instead of nothing until the next change. Pinned here — with the retained
            // payload asserted exactly — because it is a delivery a reader would not predict from this
            // module's code alone, and because a future "optimisation" that dropped it would look like
            // a harmless refactor while leaving every late-mounted panel blank.
            fanout.subscribe("ext.hello", [UI_TOPIC_THEME_CHANGED]);
            assertEqual(spy.deliveries.length, 4, "the retained snapshot is delivered on subscribe");
            assertEqual(
                spy.deliveries[3]?.event.payload,
                { variant: "dark" },
                "…and it carries the LAST published fact, not a fresh or empty one",
            );
            bus.publish(UI_TOPIC_THEME_CHANGED, { variant: "light" });
            assertEqual(spy.deliveries.length, 5, "a re-subscribe works, so the teardown was clean");
            assertEqual(
                spy.deliveries[4]?.event.payload,
                { variant: "light" },
                "…and new facts flow again",
            );
        },
    },
    {
        // ⚠ PLANT: remove the `try`/`catch` in `#fanOut`. The publisher-survives assertion goes RED —
        // and the failure it models is an editor action dying because a third-party panel's port
        // misbehaved, which is the whole reason the guard is there.
        name: "packageui: a throwing delivery target cannot take down the PUBLISHER",
        run: () => {
            const bus = new EditorUiBus({ origin: "0" });
            const reached: string[] = [];
            const fanout = new PackageUiFanout(bus, {
                deliver: (packageId: string): number => {
                    reached.push(packageId);
                    if (packageId === "ext.hostile") {
                        throw new Error("the port is gone");
                    }
                    return 1;
                },
            });
            fanout.subscribe("ext.hostile", [UI_TOPIC_FOCUS]);
            fanout.subscribe("ext.polite", [UI_TOPIC_FOCUS]);

            const report = bus.publish(UI_TOPIC_FOCUS, { panelId: "x" });
            assert(report.published, "the publisher completed despite the throwing target");
            // BOTH were attempted, and the polite one's delivery COUNTED — the positive artifact that
            // distinguishes "the throw was contained" from "the fan-out stopped at the first target".
            assertEqual(reached, ["ext.hostile", "ext.polite"], "every subscriber was attempted");
            assertEqual(fanout.delivered, 1, "only the successful delivery is counted");
        },
    },
];
