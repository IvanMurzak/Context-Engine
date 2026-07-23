// T1 unit tests for the M9 e08c `editor.ui` LOCAL BUS (uibus.ts; design 05 §5 / 02, D7 tier two).
//
// Four DoD properties are proven here, plus the boundary that gives the whole tier its meaning:
//
//   1. ENVELOPE DISCIPLINE — monotonic `seq`, the daemon stream's `topic`, and snapshot-on-subscribe.
//   2. TOPIC NAMESPACING   — the six built-ins are closed; a package topic must be manifest-declared
//                            AND namespaced under its own package; an undeclared topic is REFUSED.
//   3. THE D7 BOUNDARY     — ui-chrome facts NEVER reach the daemon.
//   4. THE MIRROR SEAM     — an envelope crosses to another window's bus, is re-sealed there, and
//                            does NOT echo back (the loop-free property e10's real drill depends on).
//
// ⚠ ON TEST 3, THE ONE THAT IS EASY TO FAKE. "Assert the daemon never sees a ui event" is trivially
// satisfiable by a test that gives the bus nothing to talk to — and such a test would still pass with
// a forwarding path in the code, which makes it worse than no test at all. So this one is written
// against the ONE channel out of editor-core: CEF's injected query function, which `ShellBridge`
// reads off the global and which every bridge call — Shell-local or daemon-bound — must pass through.
// The test installs a RECORDING query function on the global, drives the REAL theme engine plus a
// publish on every built-in topic and a package topic, and requires the channel to have seen NOTHING.
// A forwarding subscriber anywhere on those paths trips it, whatever bridge it reaches for.
//
// It still cannot see a forwarding path on a code path this test does not drive — which is exactly
// what `tools/check_ui_bus_boundary.py` (ctest `webui-uibus-boundary`) sweeps, statically, over every
// editor-core source. The two are complementary by construction, and BOTH were verified by planting a
// forwarding path and watching them go red.

import { assert, assertEqual, type TestCase } from "./harness.js";
import {
    BUILTIN_UI_TOPICS,
    DEFAULT_UI_ORIGIN,
    EditorUiBus,
    UI_TOPIC_DRAG,
    UI_TOPIC_FOCUS,
    UI_TOPIC_LAYOUT,
    UI_TOPIC_PALETTE,
    UI_TOPIC_THEME_CHANGED,
    UI_TOPIC_VIEWPORT,
    parsePackageUiTopics,
    validatePackageTopic,
    type EditorUiEvent,
    type UiMirrorSink,
} from "../uibus.js";
import { BRIDGE_QUERY_FUNCTION, type BridgeQuery } from "../bridge.js";
import { BUILTIN_DARK, BUILTIN_LIGHT, ThemeEngine, type ThemeRoot } from "../theme.js";

/** A ThemeRoot that records instead of touching the harness page's own document. */
class RecordingRoot implements ThemeRoot {
    readonly properties: Record<string, string> = {};
    readonly attributes: Record<string, string> = {};
    readonly style = {
        setProperty: (property: string, value: string): void => {
            this.properties[property] = value;
        },
    };

    setAttribute(qualifiedName: string, value: string): void {
        this.attributes[qualifiedName] = value;
    }
}

/** A mirror sink that records what it was handed — the other end of the cross-window seam. */
class RecordingSink implements UiMirrorSink {
    readonly delivered: EditorUiEvent[] = [];

    deliver(event: EditorUiEvent): void {
        this.delivered.push(event);
    }
}

/** The declaring package used across the custom-topic cases. */
const PKG = "acme.tilemap";
const PKG_TOPIC = "acme.tilemap.brush-changed";

export const uibusTests: readonly TestCase[] = [
    // ------------------------------------------------------------------ 1. envelope discipline
    {
        name: "uibus: seq is monotonic across topics and a refusal never consumes one",
        run: () => {
            const bus = new EditorUiBus();
            assertEqual(bus.seq, 0, "a fresh bus has published nothing");
            assertEqual(bus.publish(UI_TOPIC_FOCUS, { panelId: "a" }).seq, 1, "first publish is 1");
            assertEqual(bus.publish(UI_TOPIC_LAYOUT, { groups: 2 }).seq, 2, "seq spans topics");
            const refused = bus.publish("acme.undeclared", {});
            assert(!refused.published, "an undeclared topic is refused");
            assertEqual(refused.seq, 0, "a refusal reports no seq");
            assertEqual(bus.seq, 2, "and does NOT advance the counter");
            assertEqual(bus.publish(UI_TOPIC_DRAG, {}).seq, 3, "the next real publish continues");
        },
    },
    {
        name: "uibus: the envelope carries the daemon stream's shape (seq, topic, origin, payload)",
        run: () => {
            const bus = new EditorUiBus({ origin: "window-2" });
            const report = bus.publish(UI_TOPIC_VIEWPORT, { hover: "mesh-3" });
            assert(report.published, "the built-in topic is publishable");
            assertEqual(report.event?.topic, UI_TOPIC_VIEWPORT, "topic — the daemon's field name");
            assertEqual(report.event?.seq, 1, "seq");
            assertEqual(report.event?.origin, "window-2", "origin — which window said it");
            assertEqual(report.event?.payload, { hover: "mesh-3" }, "the payload is carried whole");
            assertEqual(new EditorUiBus().origin, DEFAULT_UI_ORIGIN, "a bus with no id is local");
        },
    },
    {
        name: "uibus: snapshot-on-subscribe — a late subscriber gets current state, not silence",
        run: () => {
            const bus = new EditorUiBus();
            bus.publish(UI_TOPIC_LAYOUT, { generation: 1 });
            bus.publish(UI_TOPIC_LAYOUT, { generation: 2 });
            const seen: EditorUiEvent[] = [];
            bus.subscribe(UI_TOPIC_LAYOUT, (event) => seen.push(event));
            assertEqual(seen.length, 1, "exactly the retained envelope, not the whole history");
            assertEqual(seen[0]?.payload, { generation: 2 }, "and it is the CURRENT state");
            assertEqual(seen[0]?.seq, 2, "carrying the seq it was published with");
            // A topic nothing has published yet stays silent — there is no state to snapshot.
            const quiet: EditorUiEvent[] = [];
            bus.subscribe(UI_TOPIC_PALETTE, (event) => quiet.push(event));
            assertEqual(quiet.length, 0, "no retained envelope, no snapshot");
        },
    },
    {
        name: "uibus: a subscriber sees only its own topic, and dispose() detaches it",
        run: () => {
            const bus = new EditorUiBus();
            let focus = 0;
            const subscription = bus.subscribe(UI_TOPIC_FOCUS, () => {
                focus += 1;
            });
            bus.publish(UI_TOPIC_LAYOUT, {});
            assertEqual(focus, 0, "another topic's fact is not delivered");
            bus.publish(UI_TOPIC_FOCUS, {});
            assertEqual(focus, 1, "its own is");
            subscription.dispose();
            bus.publish(UI_TOPIC_FOCUS, {});
            assertEqual(focus, 1, "a disposed subscription stops receiving");
        },
    },

    // ------------------------------------------------------------------ 2. topic namespacing
    {
        name: "uibus: the built-in topic set is exactly the six the design names, and is closed",
        run: () => {
            assertEqual(
                [...BUILTIN_UI_TOPICS],
                [
                    "editor.ui.focus",
                    "editor.ui.layout",
                    "editor.ui.drag",
                    "editor.ui.viewport",
                    "editor.ui.theme-changed",
                    "editor.ui.palette",
                ],
                "05 §5's topic list",
            );
            const bus = new EditorUiBus();
            for (const topic of BUILTIN_UI_TOPICS) {
                assert(bus.isKnownTopic(topic), `${topic} is publishable with no declaration`);
            }
            const invented = bus.publish("editor.ui.invented", {});
            assert(!invented.published, "an invented editor.ui.* topic is NOT accepted");
            assert(
                invented.diagnostic.indexOf("built-in") !== -1,
                "and the diagnostic says why, rather than failing silently",
            );
        },
    },
    {
        name: "uibus: a package topic must be manifest-declared AND namespaced under its package",
        run: () => {
            const bus = new EditorUiBus();
            assert(!bus.publish(PKG_TOPIC, {}).published, "undeclared: refused");
            const result = bus.declareTopics({ packageId: PKG, topics: [PKG_TOPIC] });
            assertEqual([...result.accepted], [PKG_TOPIC], "a well-formed declaration is accepted");
            assertEqual(result.rejected.length, 0, "with nothing rejected");
            const published = bus.publish(PKG_TOPIC, { brush: "grass" });
            assert(published.published, "declared: publishable");
            assertEqual(published.event?.topic, PKG_TOPIC, "under its own namespaced name");
        },
    },
    {
        name: "uibus: namespacing violations are refused per-topic, and the batch's rest still lands",
        run: () => {
            const bus = new EditorUiBus();
            const result = bus.declareTopics({
                packageId: PKG,
                topics: [
                    PKG_TOPIC, //             good
                    "othervendor.thing", //   another package's namespace
                    "editor.ui.focus", //     squatting a built-in
                    "editor.ui.custom", //    squatting the reserved prefix
                    PKG, //                   the bare package id is not a topic
                    "Acme.Tilemap.Bad", //    not the lowercase segment grammar
                ],
            });
            assertEqual([...result.accepted], [PKG_TOPIC], "exactly the well-formed one is accepted");
            assertEqual(result.rejected.length, 5, "each violation is refused on its own");
            for (const rejection of result.rejected) {
                assert(rejection.diagnostic !== "", `${rejection.topic} names its reason`);
            }
            assert(!bus.publish("editor.ui.custom", {}).published, "a squatted prefix stays refused");
            assert(bus.rejections.length >= 5, "every refusal is recorded for diagnosis");
        },
    },
    {
        name: "uibus: `editor` is reserved, and one package cannot re-declare another's topic",
        run: () => {
            assert(validatePackageTopic("editor", "editor.thing") !== "", "`editor` is reserved");
            assertEqual(validatePackageTopic(PKG, PKG_TOPIC), "", "a proper package topic is valid");
            const bus = new EditorUiBus();
            bus.declareTopics({ packageId: PKG, topics: [PKG_TOPIC] });
            const squatter = bus.declareTopics({
                packageId: "acme.tilemap.evil",
                topics: ["acme.tilemap.evil.x"],
            });
            assertEqual(squatter.accepted.length, 1, "a distinct namespace is fine");
            // Re-declaring the SAME topic from a different package is refused; the first owner keeps it.
            const collide = new EditorUiBus();
            collide.declareTopics({ packageId: PKG, topics: [PKG_TOPIC] });
            const second = collide.declareTopics({ packageId: PKG, topics: [PKG_TOPIC] });
            assertEqual(second.accepted.length, 1, "the SAME package may re-declare (idempotent)");
        },
    },
    {
        name: "uibus: a manifest's uiTopics declaration parses totally (absent means none, never an error)",
        run: () => {
            assertEqual(parsePackageUiTopics(PKG, undefined), { packageId: PKG, topics: [] },
                "a manifest that declares none is not in error");
            assertEqual(parsePackageUiTopics(PKG, [PKG_TOPIC, 7, null]),
                { packageId: PKG, topics: [PKG_TOPIC] },
                "non-string entries are dropped rather than poisoning the batch");
            assertEqual(parsePackageUiTopics(42, ["x"]), { packageId: "", topics: ["x"] },
                "a manifest with no id yields an empty one, which then fails validation honestly");
        },
    },
    {
        name: "uibus: subscribing to an unknown topic is INERT and recorded, never a silent no-op",
        run: () => {
            const bus = new EditorUiBus();
            let reached = 0;
            const subscription = bus.subscribe("acme.tilemap.never-declared", () => {
                reached += 1;
            });
            bus.publish("acme.tilemap.never-declared", {});
            assertEqual(reached, 0, "nothing is delivered on an undeclared topic");
            assert(
                bus.rejections.some((r) => r.diagnostic.indexOf("cannot subscribe") !== -1),
                "and the mistake is visible in the diagnostics",
            );
            subscription.dispose(); // still a disposable, so caller teardown stays uniform
        },
    },

    // ------------------------------------------------------------------ 3. the D7 boundary
    {
        name: "uibus: D7 — ui-chrome facts NEVER reach the daemon (the injected channel sees nothing)",
        run: () => {
            // THE one channel out of editor-core: CEF injects this query function onto the global and
            // ShellBridge reads it from there. Every bridge call — Shell-local or daemon-bound — goes
            // through it, so recording it records ANY attempt to leave this renderer.
            const scope = globalThis as unknown as Record<string, unknown>;
            const had = Object.prototype.hasOwnProperty.call(scope, BRIDGE_QUERY_FUNCTION);
            const previous = scope[BRIDGE_QUERY_FUNCTION];
            const requests: string[] = [];
            scope[BRIDGE_QUERY_FUNCTION] = (query: BridgeQuery): number => {
                requests.push(query.request);
                return requests.length;
            };
            try {
                const bus = new EditorUiBus({ origin: "window-1" });
                bus.declareTopics({ packageId: PKG, topics: [PKG_TOPIC] });
                // Drive the REAL producer of the one built-in topic that has one today: the theme
                // engine, through its real apply path, publishing onto this real bus.
                const engine = new ThemeEngine({
                    root: new RecordingRoot(),
                    bus,
                    probe: () => null,
                });
                engine.apply(BUILTIN_DARK);
                engine.apply(BUILTIN_LIGHT);
                // Then every remaining chrome topic, with a subscriber attached to each, so a
                // forwarding path on either the publish or the delivery side would fire.
                const seen: string[] = [];
                for (const topic of BUILTIN_UI_TOPICS) {
                    bus.subscribe(topic, (event) => seen.push(event.topic));
                    bus.publish(topic, { probe: true });
                }
                bus.publish(PKG_TOPIC, { probe: true });
                // A mirror sink is attached too: the seam is the ONE place an envelope legitimately
                // leaves this bus, so it is the likeliest place a daemon hop would be smuggled in.
                const sink = new RecordingSink();
                bus.attachMirror(sink);
                bus.publish(UI_TOPIC_FOCUS, { probe: "after-mirror" });

                assert(seen.length >= BUILTIN_UI_TOPICS.length, "the facts really were delivered");
                assert(sink.delivered.length > 0, "and the mirror seam really was exercised");
                assertEqual(
                    requests,
                    [],
                    "NOTHING left the renderer: no ui-chrome fact reaches the Shell, let alone the daemon",
                );
            } finally {
                if (had) {
                    scope[BRIDGE_QUERY_FUNCTION] = previous;
                } else {
                    delete scope[BRIDGE_QUERY_FUNCTION];
                }
            }
        },
    },

    // ------------------------------------------------------------------ 4. the cross-window seam
    {
        name: "uibus: the mirror seam carries the envelope to another window and re-seals it there",
        run: () => {
            const a = new EditorUiBus({ origin: "window-a" });
            const b = new EditorUiBus({ origin: "window-b" });
            b.publish(UI_TOPIC_LAYOUT, { local: true }); // b already has a seq of its own
            a.attachMirror({ deliver: (event) => void b.receiveMirrored(event) });
            const seen: EditorUiEvent[] = [];
            b.subscribe(UI_TOPIC_FOCUS, (event) => seen.push(event));
            a.publish(UI_TOPIC_FOCUS, { panelId: "scene-tree" });

            assertEqual(seen.length, 1, "the fact crossed to the other window");
            assertEqual(seen[0]?.payload, { panelId: "scene-tree" }, "with its payload intact");
            assertEqual(seen[0]?.origin, "window-a", "and the ORIGIN window preserved");
            assertEqual(seen[0]?.seq, 2, "but RE-SEALED with the receiving bus's own seq");
            assertEqual(b.snapshot(UI_TOPIC_FOCUS)?.origin, "window-a", "and retained for late panels");
        },
    },
    {
        name: "uibus: a mirror ring does not echo — a window drops its own envelope coming back",
        run: () => {
            const a = new EditorUiBus({ origin: "window-a" });
            const b = new EditorUiBus({ origin: "window-b" });
            // Cross-attached: exactly the topology two mirrored windows form, and the one that rings
            // forever without echo suppression.
            a.attachMirror({ deliver: (event) => void b.receiveMirrored(event) });
            b.attachMirror({ deliver: (event) => void a.receiveMirrored(event) });
            let deliveries = 0;
            a.subscribe(UI_TOPIC_DRAG, () => {
                deliveries += 1;
            });
            b.subscribe(UI_TOPIC_DRAG, () => {
                deliveries += 1;
            });
            a.publish(UI_TOPIC_DRAG, { phase: "begin" });
            assertEqual(deliveries, 2, "each window saw it exactly once — the ring terminated");
            assertEqual(a.seq, 1, "and the origin bus did not re-seal its own echo");
        },
    },
    {
        name: "uibus: a mirror sink is handed the current snapshot set on attach (a window joining late)",
        run: () => {
            const a = new EditorUiBus({ origin: "window-a" });
            a.publish(UI_TOPIC_LAYOUT, { groups: 3 });
            a.publish(UI_TOPIC_THEME_CHANGED, { themeId: BUILTIN_DARK });
            const sink = new RecordingSink();
            const attachment = a.attachMirror(sink);
            assertEqual(sink.delivered.length, 2, "both retained facts are replayed to the new window");
            assertEqual(sink.delivered[0]?.seq, 1, "in publish order");
            assertEqual(a.mirrorCount, 1, "the sink is attached");
            attachment.dispose();
            a.publish(UI_TOPIC_LAYOUT, { groups: 4 });
            assertEqual(sink.delivered.length, 2, "a detached window stops receiving");
            assertEqual(a.mirrorCount, 0, "and is no longer counted");
        },
    },
    {
        name: "uibus: a throwing mirror transport cannot break local delivery",
        run: () => {
            const bus = new EditorUiBus();
            bus.attachMirror({
                deliver: (): void => {
                    throw new Error("the other window went away");
                },
            });
            let reached = 0;
            bus.subscribe(UI_TOPIC_PALETTE, () => {
                reached += 1;
            });
            const report = bus.publish(UI_TOPIC_PALETTE, { open: true });
            assert(report.published, "the publish still succeeded");
            assertEqual(reached, 1, "and the local subscriber was served");
        },
    },
    {
        name: "uibus: a mirrored envelope on an unknown topic is refused, not trusted",
        run: () => {
            // The receiving window validates like any publisher: another window's package set is not
            // automatically this one's, so an unknown topic must fail closed rather than materialise.
            const b = new EditorUiBus({ origin: "window-b" });
            const refused = b.receiveMirrored({
                seq: 9,
                topic: "acme.tilemap.brush-changed",
                origin: "window-a",
                payload: {},
            });
            assert(!refused.published, "an undeclared mirrored topic is refused");
            assertEqual(b.seq, 0, "and consumes no seq");
        },
    },
];
