// T1 for the DAEMON-EVENT FAN-OUT's editor-core half (M9 e13c-2, design 04 §5 / 05 §1).
//
// TWO TIERS OF CLAIM, DELIBERATELY SEPARATED.
//
//   1. THE PUMP, driven with no DOM at all: what it polls, what it delivers, what it must NOT deliver,
//      and how it degrades. Pure, so each rule is provable on its own rather than through a browser.
//
//   2. THE LAST HOP, driven against a REAL sandboxed iframe with a REAL `MessageChannel` port and a
//      REAL opaque origin (`event.origin === "null"`). That is the DoD line "an event published by
//      the daemon arrives inside the iframe", and it is asserted the only way that sentence can be
//      honestly asserted: the CHILD DOCUMENT reads the value off the envelope it received and reports
//      it back, so the assertion is made on a value that came OUT of the sandboxed document rather
//      than on one the host still holds. The harness page carries no CSP by design
//      (`src/editor/webui/test/harness.html`), so this is a genuine cross-document hop, not a mock of
//      one.
//
// ⚠ WHAT THIS TIER CANNOT SEE, STATED RATHER THAN IMPLIED. The DAEMON half of the chain — a real
// `event` frame pushed onto a package's baseline session, pumped into the BOUNDED buffer, and drained
// by `panel.events.poll` — is C++, and is pinned by `editor-shell-test_package_events` on all three
// `build` legs. Between the two tiers every hop is covered by the tier that can actually observe it;
// neither file claims the other's half.

import { assert, assertEqual, delay, waitFor, type TestCase } from "./harness.js";
import { BridgeError, type ShellBridge } from "../bridge.js";
import {
    IFRAME_ALLOW,
    IFRAME_LOADING,
    IFRAME_REFERRER_POLICY,
    IFRAME_SANDBOX,
} from "../extpanel.js";
import {
    PANEL_EVENTS_DELIVER_VERB,
    PANEL_EVENTS_POLL_METHOD,
    PackageEventPump,
    parsePackageEventBatch,
    type PackageEventBatch,
    type PackageEventTargets,
} from "../packageevents.js";
import {
    EXT_PORT_HANDSHAKE_TAG,
    PANEL_BRIDGE_TAG,
    PANEL_BRIDGE_VERSION,
    PanelPortBridge,
} from "../panelport.js";

// ------------------------------------------------------------------------------------- fixtures

/** One daemon wire envelope, in the shape `PackageSessionHost::pump` buffers (event + `subId`). */
function daemonEvent(seq: number, topic: string, value: string): Record<string, unknown> {
    return {
        seq,
        incarnationId: "inc-1",
        generation: 1,
        topic,
        payload: { value },
        subId: "sub-1",
    };
}

/** A recorded delivery: which package, and the batch it was handed. */
interface Delivery {
    readonly packageId: string;
    readonly batch: PackageEventBatch;
}

/** A `PackageEventTargets` that records instead of touching the DOM. */
function recordingTargets(packages: readonly string[], ports = 1): {
    targets: PackageEventTargets;
    deliveries: Delivery[];
} {
    const deliveries: Delivery[] = [];
    return {
        deliveries,
        targets: {
            packages: (): readonly string[] => packages,
            deliver: (packageId: string, batch: PackageEventBatch): number => {
                deliveries.push({ packageId, batch });
                return ports;
            },
        },
    };
}

/** Every `panel.events.poll` the pump issued, and the scripted reply for each package. */
interface FakeShell {
    readonly bridge: ShellBridge;
    readonly polled: string[];
}

function fakeShell(replies: Record<string, unknown>, refuse: readonly string[] = []): FakeShell {
    const polled: string[] = [];
    const call = (method: string, params?: Record<string, unknown>): Promise<unknown> => {
        if (method !== PANEL_EVENTS_POLL_METHOD) {
            return Promise.reject(
                new BridgeError({ code: -32601, message: "unexpected method", reason: "unknown_method" }),
            );
        }
        const packageId = String(params?.["packageId"] ?? "");
        polled.push(packageId);
        if (refuse.includes(packageId)) {
            return Promise.reject(
                new BridgeError({ code: -32601, message: "no such route", reason: "unknown_method" }),
            );
        }
        return Promise.resolve(replies[packageId]);
    };
    return { bridge: { call } as unknown as ShellBridge, polled };
}

// --------------------------------------------------------------------------- the iframe harness

/**
 * The panel-side agent for the last-hop case: it offers its port exactly as the Shell-injected
 * bootstrap does, then REPORTS every port envelope back to the parent.
 *
 * ES5 ON PURPOSE (`var`, `function`) — injected as raw HTML into a document with no build step, as
 * the real bootstrap is. It deliberately never REPLIES to anything: a one-way delivery correlates
 * nothing, and a fixture that answered would hide a delivery that had (wrongly) been sent as a
 * `request`.
 */
const EVENT_CHILD_FIXTURE = `<!doctype html><html><head><script>
(function () {
  var TAG = ${JSON.stringify(EXT_PORT_HANDSHAKE_TAG)};
  function report(payload) { parent.postMessage(payload, "*"); }
  var ch = new MessageChannel();
  ch.port1.onmessage = function (event) {
    var data = event.data;
    // READ THE VALUE OUT, INSIDE THE FRAME. Reporting the whole envelope back would let the parent
    // assert on a shape it already had; digging the payload out here is what proves the sandboxed
    // document could actually PARSE what it was handed.
    var first = data && data.params && data.params.events && data.params.events[0];
    report({
      probe: "delivered",
      kind: data && data.kind,
      verb: data && data.verb,
      origin: String(location.origin),
      count: data && data.params && data.params.events ? data.params.events.length : -1,
      dropped: data && data.params ? data.params.dropped : null,
      gapped: data && data.params ? data.params.gapped : null,
      topic: first ? first.topic : null,
      seq: first ? first.seq : null,
      value: first && first.payload ? first.payload.value : null
    });
  };
  ch.port1.start();
  parent.postMessage({ ctx: TAG }, "*", [ch.port2]);
})();
</script></head><body>event panel fixture</body></html>`;

interface EventHarness {
    readonly frame: HTMLIFrameElement;
    readonly bridge: PanelPortBridge;
    readonly probes: Record<string, unknown>[];
    dispose(): void;
}

/**
 * Build a frame + bridge in EXACTLY the order `IframePanelRenderer.refresh` does — attributes, then
 * the bridge, then the source, then insertion (panelport.test.ts § createHarness says why that order
 * is itself under test).
 *
 * The fixed in-viewport placement + `IFRAME_LOADING` are the MEASURED flake fix this tier's other
 * frame cases carry: Chromium deprioritises an offscreen iframe's load and throttles its timers, and
 * ~290 earlier cases have already grown the document.
 */
function createEventHarness(): EventHarness {
    const frame = window.document.createElement("iframe");
    frame.setAttribute("sandbox", IFRAME_SANDBOX);
    frame.setAttribute("allow", IFRAME_ALLOW);
    frame.setAttribute("referrerpolicy", IFRAME_REFERRER_POLICY);
    frame.setAttribute("loading", IFRAME_LOADING);
    frame.style.position = "fixed";
    frame.style.top = "0";
    frame.style.left = "0";
    frame.style.width = "100px";
    frame.style.height = "100px";

    const probes: Record<string, unknown>[] = [];
    const bridge = new PanelPortBridge({ frame, panelId: "hello-panel.events" });
    const listener = (event: MessageEvent): void => {
        if (event.source !== frame.contentWindow) {
            return;
        }
        const data: unknown = event.data;
        if (typeof data === "object" && data !== null) {
            probes.push(data as Record<string, unknown>);
        }
    };
    window.addEventListener("message", listener);

    frame.srcdoc = EVENT_CHILD_FIXTURE;
    window.document.body.appendChild(frame);

    return {
        frame,
        bridge,
        probes,
        dispose: (): void => {
            window.removeEventListener("message", listener);
            bridge.dispose();
            frame.remove();
        },
    };
}

/** The budget for a wait that follows a REAL cross-document load — bounds a HANG, not latency. */
const LOAD_BOUND_MS = 10_000;

// ------------------------------------------------------------------------------------------ cases

export const packageEventTests: readonly TestCase[] = [
    {
        name: "packageevents: a poll reply is parsed with its LOUD pair intact",
        run: (): void => {
            const batch = parsePackageEventBatch({
                events: [daemonEvent(4, "diagnostics", "a")],
                dropped: 3,
                gapped: true,
            });
            assert(batch !== null, "a well-formed reply parses");
            assertEqual(batch?.events.length, 1, "the events survive");
            assertEqual(batch?.dropped, 3, "`dropped` is carried, not defaulted away");
            assertEqual(batch?.gapped, true, "`gapped` is carried");
            // The POSITIVE half of the same claim: a CLEAN reply must parse as clean, or "gapped is
            // true after an overflow" could hold because the field is simply always true.
            const clean = parsePackageEventBatch({ events: [], dropped: 0, gapped: false });
            assertEqual(clean?.dropped, 0, "a clean reply reports nothing dropped");
            assertEqual(clean?.gapped, false, "…and is not gapped");
            // A reply that is not the shape we need is REFUSED rather than coerced into an empty one:
            // an invented empty batch would read as "the daemon said nothing", which is a lie.
            assertEqual(parsePackageEventBatch({ dropped: 1 }), null, "no `events` array => null");
            assertEqual(parsePackageEventBatch(null), null, "a non-object => null");
        },
    },
    {
        name: "packageevents: the pump polls each mounted package ONCE and delivers its own batch",
        run: async (): Promise<void> => {
            const shell = fakeShell({
                "hello-panel": { events: [daemonEvent(1, "diagnostics", "a")], dropped: 0, gapped: false },
                "other-panel": {
                    events: [daemonEvent(9, "session", "b"), daemonEvent(10, "session", "c")],
                    dropped: 0,
                    gapped: false,
                },
            });
            // TWO PANELS OF ONE PACKAGE, which is the shape the deduplication exists for: the Shell
            // buffer DRAINS, so a second poll for the same package would answer empty and the second
            // panel would get nothing. `packages()` is what must dedupe, and `PanelHost` does.
            const { targets, deliveries } = recordingTargets(["hello-panel", "other-panel"], 2);
            const pump = new PackageEventPump(shell.bridge, targets);

            const delivered = await pump.poll();
            assertEqual(shell.polled.join(","), "hello-panel,other-panel", "one poll per package");
            assertEqual(deliveries.length, 2, "one delivery per package");
            assertEqual(deliveries[0]?.packageId, "hello-panel", "…addressed to its own package");
            assertEqual(
                (deliveries[1]?.batch.events[0] as Record<string, unknown> | undefined)?.["seq"],
                9,
                "a package receives ITS OWN events, not another package's",
            );
            // 1 event x 2 ports + 2 events x 2 ports.
            assertEqual(delivered, 6, "every live port of a package is counted");
            assertEqual(pump.delivered, 6, "…and folded into the running total");
            assertEqual(pump.polls, 1, "one completed round");
        },
    },
    {
        name: "packageevents: an IDLE package costs its panels no message, but a GAPPED empty batch is still delivered",
        run: async (): Promise<void> => {
            const shell = fakeShell({
                idle: { events: [], dropped: 0, gapped: false },
                lost: { events: [], dropped: 7, gapped: true },
            });
            const { targets, deliveries } = recordingTargets(["idle", "lost"]);
            const pump = new PackageEventPump(shell.bridge, targets);
            await pump.poll();

            // THE POSITIVE ARTIFACT: exactly ONE delivery, and it is the GAPPED one. An
            // "idle was not delivered" assertion alone could pass because nothing was delivered at
            // all, which is precisely the vacuity this pairing removes.
            assertEqual(deliveries.length, 1, "only one of the two packages was delivered to");
            assertEqual(deliveries[0]?.packageId, "lost", "and it is the gapped one");
            assertEqual(deliveries[0]?.batch.dropped, 7, "carrying how many events were lost");
            assertEqual(deliveries[0]?.batch.gapped, true, "…and the re-snapshot instruction");
            assertEqual(pump.dropped, 7, "the running dropped total accumulates");
            assertEqual(pump.gaps, 1, "…and the gap count");
        },
    },
    {
        name: "packageevents: a Shell that does not route the poll is ABSENT, not broken",
        run: async (): Promise<void> => {
            const shell = fakeShell(
                { good: { events: [daemonEvent(1, "diagnostics", "a")], dropped: 0, gapped: false } },
                ["stale"],
            );
            const { targets, deliveries } = recordingTargets(["stale", "good"]);
            const pump = new PackageEventPump(shell.bridge, targets);
            const delivered = await pump.poll();

            // The refused package is skipped and the round CONTINUES — one panel's missing route must
            // not take every other panel's events down with it.
            assertEqual(deliveries.length, 1, "the refused package delivered nothing");
            assertEqual(deliveries[0]?.packageId, "good", "…and the healthy one still did");
            assertEqual(delivered, 1, "the round completed");
        },
    },
    {
        name: "packageevents: with no package panels mounted the pump never calls the Shell at all",
        run: async (): Promise<void> => {
            const shell = fakeShell({});
            const { targets } = recordingTargets([]);
            const pump = new PackageEventPump(shell.bridge, targets);
            assertEqual(await pump.poll(), 0, "nothing delivered");
            assertEqual(shell.polled.length, 0, "and not one bridge round trip on an idle tick");
            assertEqual(pump.polls, 0, "an empty round is not counted as a round");
        },
    },
    {
        // THE DoD LINE. Every hop from the `panel.events.poll` reply to the INSIDE of a real
        // opaque-origin sandboxed document, with the value read out by the document itself.
        name: "packageevents: an event drained from the Shell arrives INSIDE a real sandboxed iframe (e13c-2 end to end)",
        run: async (): Promise<void> => {
            const harness = createEventHarness();
            try {
                await waitFor(
                    "the child's handshake to be accepted",
                    () => harness.bridge.granted,
                    LOAD_BOUND_MS,
                    () => `state=${harness.bridge.state} probes=${JSON.stringify(harness.probes)}`,
                );

                // The REAL pump, over a fake Shell answering with a daemon-shaped envelope, delivering
                // through the REAL `PanelPortBridge` this frame handshook with.
                const shell = fakeShell({
                    "hello-panel": {
                        events: [daemonEvent(42, "derivation", "settled")],
                        dropped: 2,
                        gapped: true,
                    },
                });
                const pump = new PackageEventPump(shell.bridge, {
                    packages: (): readonly string[] => ["hello-panel"],
                    deliver: (_packageId: string, batch: PackageEventBatch): number =>
                        harness.bridge.deliver(PANEL_EVENTS_DELIVER_VERB, batch) ? 1 : 0,
                });
                assertEqual(await pump.poll(), 1, "the pump reported one event delivered");

                await waitFor(
                    "the child to report the delivered envelope",
                    () => harness.probes.some((probe) => probe["probe"] === "delivered"),
                    LOAD_BOUND_MS,
                    () => `stats=${JSON.stringify(harness.bridge.stats)}`,
                );
                const seen = harness.probes.find((probe) => probe["probe"] === "delivered");

                // THE PREMISE, MEASURED HERE TOO: the document really is opaque-origin, so this is the
                // hop no origin string could have authenticated — the port is what carried it.
                assertEqual(seen?.["origin"], "null", "the receiving document has an opaque origin");
                assertEqual(seen?.["kind"], "event", "it arrived as a ONE-WAY fact, not a request");
                assertEqual(seen?.["verb"], PANEL_EVENTS_DELIVER_VERB, "under the delivery verb");
                // The VALUES the child dug out of the envelope — the daemon's own event fields.
                assertEqual(seen?.["count"], 1, "one event crossed");
                assertEqual(seen?.["topic"], "derivation", "the daemon's topic survived the hop");
                assertEqual(seen?.["seq"], 42, "…and its seq");
                assertEqual(seen?.["value"], "settled", "…and its payload");
                // THE LOUD PAIR REACHED THE PACKAGE. Without it a panel that missed events looks
                // exactly like a panel whose subject did not change, which is the whole reason the
                // Shell bounds the buffer LOUDLY rather than silently.
                assertEqual(seen?.["dropped"], 2, "the drop count reached INSIDE the package frame");
                assertEqual(seen?.["gapped"], true, "…and so did the re-snapshot instruction");

                // AND IT CORRELATED NOTHING: a delivery must not occupy a pending slot, or 64 events
                // to a quiet panel would exhaust the host's request bound.
                assertEqual(harness.bridge.stats.pending, 0, "no pending host request was created");
                assertEqual(harness.bridge.stats.eventsDelivered, 1, "one fact posted");
            } finally {
                harness.dispose();
            }
        },
    },
    {
        name: "packageevents: a delivery on a port that was never granted is a false, not a throw",
        run: async (): Promise<void> => {
            // No frame document at all, so no handshake can ever arrive: the honest state of a panel
            // whose package failed to load. `deliver` must answer false rather than throw out of a
            // `setInterval` tick in a renderer with no console.
            const frame = window.document.createElement("iframe");
            const bridge = new PanelPortBridge({ frame, panelId: "no-port", handshakeTimeoutMs: 50 });
            try {
                assertEqual(
                    bridge.deliver(PANEL_EVENTS_DELIVER_VERB, { events: [], dropped: 0, gapped: false }),
                    false,
                    "an ungranted port takes nothing",
                );
                await delay(120);
                assertEqual(bridge.state, "revoked", "…and the grant window still closed normally");
                assertEqual(bridge.stats.eventsDelivered, 0, "nothing was counted as delivered");
            } finally {
                bridge.dispose();
                frame.remove();
            }
        },
    },
    {
        name: "packageevents: the delivery envelope carries the production tag and version",
        run: (): void => {
            // Pinned because a package's own listener filters on both, and a drift would make every
            // event silently unrecognised inside the frame — the same failure class the `session.state`
            // constant mirror exists to prevent, one layer out.
            assertEqual(PANEL_BRIDGE_TAG, "context.panel-bridge", "the port envelope tag");
            assertEqual(PANEL_BRIDGE_VERSION, 1, "the port envelope version");
            assertEqual(PANEL_EVENTS_POLL_METHOD, "panel.events.poll", "mirrors kPanelEventsPollMethod");
            assertEqual(PANEL_EVENTS_DELIVER_VERB, "events.deliver", "the host -> panel delivery verb");
        },
    },
];
