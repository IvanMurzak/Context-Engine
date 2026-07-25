// T1 cases for the THIRD-PARTY PANEL PORT (M9 e13b-1, panelport.ts) — the grant, its one-shot, its
// revocation, and the versioned envelope over it.
//
// ⚠ WHY THIS TIER CAN PROVE THE REAL THING, and what it cannot. `webui-ts-unit` runs in a REAL
// headless browser, so every case below uses a REAL `<iframe>` on a REAL OPAQUE ORIGIN, a REAL
// `MessageChannel`, and REAL port TRANSFER semantics — the three facts the whole design turns on. The
// child document is a `sandbox="allow-scripts"` `srcdoc` frame carrying a script that behaves as the
// Shell-injected bootstrap does: it mints a channel and transfers one port UP. That is not a mock of
// the mechanism; it IS the mechanism, minus the two things only the live CEF leg can supply — that the
// SHELL is what injects the script (pinned instead by `test_ext_scheme.cpp`, which owns the bytes and
// the splice point) and that it posts to `kAppOrigin` exactly (same place). Everything about WHO MAY
// HOLD THE PORT is decided here.
//
// THREE PREMISES ARE MEASURED RATHER THAN ASSUMED, because the design is unsound if any of them is
// wrong and each is the kind of platform claim that is easy to state and easy to get backwards:
//   * a sandboxed frame's `event.origin` really is the literal `"null"` (so no origin string could
//     ever have authenticated a package — B-F6);
//   * a frame created with its source set BEFORE insertion fires exactly ONE `load` event for its
//     first document, i.e. no initial `about:blank` load inflates the count the revocation rule uses;
//   * a `sandbox="allow-scripts"` frame really can navigate ITSELF (the `ext_scheme.h` hazard's own
//     premise), and that navigation really does produce a second `load` on the element.
//
// NON-VACUITY was proven by PLANTING, not asserted: the three weakenings the parent DoD names — hand
// the port with no handshake, accept a second port, skip revocation on re-navigation — each reddened a
// named case here before being reverted byte-exact. The plant log is in the PR body.

import { assert, assertEqual, AssertionError, type TestCase } from "./harness.js";
import {
    EXT_PORT_HANDSHAKE_TAG,
    PANEL_BRIDGE_REFUSALS,
    PANEL_BRIDGE_TAG,
    PANEL_BRIDGE_TOKEN_MAX_LENGTH,
    PANEL_BRIDGE_VERSION,
    PANEL_PORT_STATE_ATTRIBUTE,
    PanelPortBridge,
    type PanelVerbHandler,
} from "../panelport.js";

// --------------------------------------------------------------------------------- the child fixture

/**
 * The panel-side agent. It opens with `offer()` — the Shell-injected bootstrap's exact role, in the
 * document's FIRST script — and then serves commands the host posts down, so a case can ask the real
 * child to offer a second port, to send a specific envelope over a specific channel, or to navigate
 * itself away.
 *
 * ⚠ IT POSTS WITH `targetOrigin: "*"`, WHERE THE REAL BOOTSTRAP POSTS TO `kAppOrigin`. Unavoidable and
 * harmless: this harness page is served from `http://localhost:<port>`, not from `context-editor://app`,
 * so a precise target would match nothing. The real bootstrap's exact target IS pinned — by
 * `test_ext_scheme.cpp`, which asserts the served bytes name `kAppOrigin` and contain no wildcard. The
 * two halves of that property therefore live in the two tiers that can each see one of them.
 *
 * ES5 ON PURPOSE (`var`, `function`): it is injected as raw HTML into a document with no build step,
 * exactly as the Shell's bootstrap is.
 */
const CHILD_FIXTURE = `<!doctype html><html><head><script>
(function () {
  var TAG = ${JSON.stringify(EXT_PORT_HANDSHAKE_TAG)};
  var channels = [];
  var silent = false;
  function report(payload) { parent.postMessage(payload, "*"); }
  function offer() {
    var ch = new MessageChannel();
    var index = channels.length;
    channels.push(ch);
    ch.port1.onmessage = function (event) {
      var data = event.data;
      report({ probe: "port-message", index: index, data: data });
      if (!silent && data && data.kind === "request" && typeof data.id === "string") {
        ch.port1.postMessage({ ctx: data.ctx, v: data.v, kind: "reply", id: data.id,
                               ok: true, result: "echo:" + String(data.verb) });
      }
    };
    ch.port1.start();
    report({ probe: "offered", index: index });
    parent.postMessage({ ctx: TAG }, "*", [ch.port2]);
  }
  window.addEventListener("message", function (event) {
    var data = event.data;
    if (!data || typeof data.cmd !== "string") { return; }
    if (data.cmd === "offer") {
      offer();
    } else if (data.cmd === "offer-no-port") {
      parent.postMessage({ ctx: TAG }, "*");
    } else if (data.cmd === "offer-two-ports") {
      var a = new MessageChannel();
      var b = new MessageChannel();
      parent.postMessage({ ctx: TAG }, "*", [a.port2, b.port2]);
    } else if (data.cmd === "offer-wrong-tag") {
      var c = new MessageChannel();
      parent.postMessage({ ctx: TAG + ".not" }, "*", [c.port2]);
    } else if (data.cmd === "send") {
      channels[data.index].port1.postMessage(data.envelope);
    } else if (data.cmd === "silence") {
      silent = true;
    } else if (data.cmd === "describe") {
      report({ probe: "describe", origin: String(location.origin), channels: channels.length });
    } else if (data.cmd === "navigate") {
      location.href = "about:blank";
    }
  });
  offer();
})();
</script></head><body>panel fixture</body></html>`;

// ------------------------------------------------------------------------------------- the harness

interface Probe {
    readonly probe: string;
    readonly [key: string]: unknown;
}

interface PanelHarness {
    readonly frame: HTMLIFrameElement;
    readonly bridge: PanelPortBridge;
    /** Every `{probe: …}` report the child sent, in arrival order. */
    readonly probes: Probe[];
    /** `event.origin` for every message this frame sent — the B-F6 measurement. */
    readonly origins: string[];
    command(command: Record<string, unknown>): void;
    dispose(): void;
}

interface HarnessOptions {
    readonly verbs?: ReadonlyMap<string, PanelVerbHandler>;
    readonly requestTimeoutMs?: number;
    readonly handshakeTimeoutMs?: number;
    /** Replaces the child document. Used by the "never handshakes" case. */
    readonly childHtml?: string;
}

/**
 * Build a frame + bridge in EXACTLY the order `IframePanelRenderer.refresh` does — attributes, then
 * the bridge, then the source, then insertion.
 *
 * THAT ORDER IS PART OF WHAT IS UNDER TEST. The bootstrap's handshake can be posted before the
 * caller's next statement runs, so a harness that constructed the bridge after setting the source
 * would be testing a bridge that production does not build, and the "listeners before src" invariant
 * would have no coverage anywhere.
 */
function createHarness(options: HarnessOptions = {}): PanelHarness {
    const frame = window.document.createElement("iframe");
    frame.setAttribute("sandbox", "allow-scripts");
    frame.setAttribute("allow", "");
    frame.setAttribute("referrerpolicy", "no-referrer");
    // `loading="eager"` MIRRORS PRODUCTION, and here it is also the FLAKE FIX. ⚠ MEASURED, twice: two
    // cases timed out waiting for a handshake on one run of this tier and passed on the next with
    // byte-identical code. The frames are appended to a document that ~280 earlier cases have already
    // grown, so they land far below the viewport — and Chromium DEPRIORITISES an offscreen iframe's
    // load and throttles its timers, which is enough to push a `srcdoc` script's first task past a
    // wall-clock budget while nothing is actually wrong. Both halves of the fix are therefore kept:
    // eager loading, AND the fixed in-viewport placement below. An intermittent gate is worse than no
    // gate, so this is fixed at the cause rather than by widening the budget alone.
    frame.setAttribute("loading", "eager");
    frame.style.position = "fixed";
    frame.style.top = "0";
    frame.style.left = "0";
    frame.style.width = "100px";
    frame.style.height = "100px";

    const probes: Probe[] = [];
    const origins: string[] = [];
    const bridge = new PanelPortBridge({
        frame,
        panelId: "pkg.harness",
        ...(options.verbs === undefined ? {} : { verbs: options.verbs }),
        ...(options.requestTimeoutMs === undefined
            ? {}
            : { requestTimeoutMs: options.requestTimeoutMs }),
        ...(options.handshakeTimeoutMs === undefined
            ? {}
            : { handshakeTimeoutMs: options.handshakeTimeoutMs }),
    });

    const listener = (event: MessageEvent): void => {
        if (event.source !== frame.contentWindow) {
            return;
        }
        origins.push(event.origin);
        const data: unknown = event.data;
        if (typeof data === "object" && data !== null && typeof (data as Probe).probe === "string") {
            probes.push(data as Probe);
        }
    };
    window.addEventListener("message", listener);

    frame.srcdoc = options.childHtml ?? CHILD_FIXTURE;
    window.document.body.appendChild(frame);

    return {
        frame,
        bridge,
        probes,
        origins,
        command: (command: Record<string, unknown>): void => {
            frame.contentWindow?.postMessage(command, "*");
        },
        dispose: (): void => {
            window.removeEventListener("message", listener);
            bridge.dispose();
            frame.remove();
        },
    };
}

function delay(ms: number): Promise<void> {
    return new Promise<void>((resolve) => {
        setTimeout(resolve, ms);
    });
}

/**
 * Poll until `predicate` holds, then return. Polling rather than a one-shot `await` because the facts
 * being waited on arrive from another document's event loop through two different task sources, so
 * "one microtask later" is not a bound the platform gives.
 */
async function waitFor(
    what: string,
    predicate: () => boolean,
    timeoutMs = 5_000,
    diagnose?: () => string,
): Promise<void> {
    const deadline = Date.now() + timeoutMs;
    while (!predicate()) {
        if (Date.now() > deadline) {
            // A TIMEOUT MUST NAME ITS CAUSE. This tier's only channel is the failure string, and a
            // bare "timed out" here would send the next reader to a full CI-log archaeology pass for
            // something the bridge's own counters already knew.
            throw new AssertionError(
                `timed out after ${String(timeoutMs)}ms waiting for ${what}` +
                    (diagnose === undefined ? "" : ` — ${diagnose()}`),
            );
        }
        await delay(5);
    }
}

/** Await the grant the child's opening `offer()` produces. */
async function waitForGrant(harness: PanelHarness): Promise<void> {
    await waitFor(
        "the child's handshake to be accepted",
        () => harness.bridge.granted,
        // 10s, not the 5s default: a real cross-document load on a loaded CI runner is the slowest
        // thing this file waits on, and the budget exists to bound a HANG, not to measure latency.
        10_000,
        () =>
            `state=${harness.bridge.state} stats=${JSON.stringify(harness.bridge.stats)} ` +
            `origins=${JSON.stringify(harness.origins)} probes=${JSON.stringify(harness.probes)}`,
    );
}

/** A well-formed request envelope, as an untrusted panel would build one. */
function requestEnvelope(id: string, verb: string, extra: Record<string, unknown> = {}):
    Record<string, unknown> {
    return { ctx: PANEL_BRIDGE_TAG, v: PANEL_BRIDGE_VERSION, kind: "request", id, verb, ...extra };
}

/** The reply the child observed for `id`, or undefined. */
function replyFor(harness: PanelHarness, id: string): Record<string, unknown> | undefined {
    for (const probe of harness.probes) {
        if (probe.probe !== "port-message") {
            continue;
        }
        const data = probe["data"];
        if (
            typeof data === "object" &&
            data !== null &&
            (data as Record<string, unknown>)["id"] === id
        ) {
            return data as Record<string, unknown>;
        }
    }
    return undefined;
}

async function waitForReply(harness: PanelHarness, id: string): Promise<Record<string, unknown>> {
    await waitFor(`a reply for ${id}`, () => replyFor(harness, id) !== undefined);
    return replyFor(harness, id) as Record<string, unknown>;
}

/**
 * The verbs the boundary redraw parked in e13b-2 / e13c / e13d. EVERY one of them must get the SAME
 * refusal today — that is what "so they can fill verbs without renegotiating the shape" means, and
 * naming them explicitly is what makes the promise checkable rather than rhetorical.
 */
const PARKED_VERBS: readonly string[] = [
    "bridge.call",
    "bridge.events.subscribe",
    "bridge.commands.register",
    "bridge.commands.unregister",
    "bridge.ui.subscribe",
    "bridge.theme.tokens",
    "bridge.state.get",
    "bridge.state.set",
];

// ------------------------------------------------------------------------------------------ cases

export const panelPortTests: readonly TestCase[] = [
    {
        // ⚠ PREMISE 1, MEASURED. If this is ever false, `event.origin` WOULD have been usable and the
        // whole injected-bootstrap mechanism would be over-engineering. It is not false, and the
        // reason it must be measured rather than cited is that the opposite claim is one line away
        // from plausible: the frame's `src` names a real origin, and only the ABSENT
        // `allow-same-origin` token makes the document's origin opaque.
        name: "panelport: a sandboxed frame's messages arrive with the OPAQUE origin \"null\" (B-F6)",
        run: async (): Promise<void> => {
            const harness = createHarness();
            try {
                await waitForGrant(harness);
                harness.command({ cmd: "describe" });
                await waitFor("the child's self-description", () =>
                    harness.probes.some((probe) => probe.probe === "describe"),
                );
                assert(
                    harness.origins.length > 0,
                    "the child sent at least one message this case observed",
                );
                for (const origin of harness.origins) {
                    assertEqual(
                        origin,
                        "null",
                        "every message from a sandbox-without-allow-same-origin frame carries the " +
                            "opaque origin as the literal string \"null\", so no origin string could " +
                            "distinguish one package's document from another's",
                    );
                }
                const described = harness.probes.find((probe) => probe.probe === "describe");
                assertEqual(
                    described?.["origin"],
                    "null",
                    "and the document agrees about its own origin from the inside",
                );
            } finally {
                harness.dispose();
            }
        },
    },
    {
        // ⚠ PREMISE 2, MEASURED — the fact the revocation rule rests on. An initial `about:blank` load
        // counted here would revoke EVERY healthy panel the moment its real document finished loading,
        // which is a total outage that no amount of reading the spec would have ruled out with
        // confidence.
        name: "panelport: a frame whose source is set before insertion fires exactly ONE load (the load-count rule)",
        run: async (): Promise<void> => {
            const harness = createHarness();
            try {
                await waitForGrant(harness);
                await waitFor("the first document's load event", () => harness.bridge.stats.loads >= 1);
                // Settle: give any spurious second load a generous window to arrive.
                await delay(200);
                assertEqual(
                    harness.bridge.stats.loads,
                    1,
                    "exactly one load event for the first document — no initial about:blank load is " +
                        "counted, which is what lets 'the second load' mean 're-navigation'",
                );
                assert(!harness.bridge.revoked, "so a healthy panel is NOT revoked by its own load");
                assert(harness.bridge.port !== undefined, "and it still holds its port");
                assertEqual(
                    harness.frame.getAttribute(PANEL_PORT_STATE_ATTRIBUTE),
                    "granted",
                    "the frame element reflects the live state",
                );
            } finally {
                harness.dispose();
            }
        },
    },
    {
        name: "panelport: the first conforming handshake is granted a port, and the state is reflected on the frame",
        run: async (): Promise<void> => {
            const harness = createHarness();
            try {
                // BEFORE the child runs, the element already says `pending` — which is what proves the
                // bridge was constructed before the source was set (a bridge built afterwards could
                // not have written this attribute before the load began).
                assert(
                    harness.frame.getAttribute(PANEL_PORT_STATE_ATTRIBUTE) !== null,
                    "the port state is reflected from construction, not from the grant",
                );
                await waitForGrant(harness);
                assert(harness.bridge.granted, "the handshake was accepted");
                assert(harness.bridge.port !== undefined, "and a live port is held");
                assert(!harness.bridge.revoked, "nothing revoked it");
                assertEqual(
                    harness.bridge.stats.refusedHandshakes,
                    0,
                    "the opening handshake was not refused on any axis",
                );
                assertEqual(
                    harness.frame.getAttribute(PANEL_PORT_STATE_ATTRIBUTE),
                    "granted",
                    "and the element says so",
                );
                assert(
                    await harness.bridge.waitForPort(),
                    "waitForPort resolves true once the outcome is known",
                );
            } finally {
                harness.dispose();
            }
        },
    },
    {
        // ⚠ PLANT (a) FROM THE PARENT DoD — "hand the port without the handshake". A build that granted
        // on `load` instead of on a handshake reddens HERE: this child never handshakes, yet its
        // document loads perfectly.
        name: "panelport: a frame that loads but never handshakes is granted NOTHING",
        run: async (): Promise<void> => {
            const harness = createHarness({
                childHtml: "<!doctype html><html><body>a panel with no bootstrap</body></html>",
            });
            try {
                await waitFor("the child document to load", () => harness.bridge.stats.loads >= 1);
                await delay(200);
                assert(
                    !harness.bridge.granted,
                    "NO port is granted without a handshake — a load event is not authentication, and " +
                        "granting on it is the plant this case exists to redden",
                );
                assert(harness.bridge.port === undefined, "and no port is held");
                assertEqual(
                    harness.frame.getAttribute(PANEL_PORT_STATE_ATTRIBUTE),
                    "pending",
                    "the grant window is still open, honestly reported",
                );
            } finally {
                harness.dispose();
            }
        },
    },
    {
        // ⚠ PLANT (b) FROM THE PARENT DoD — "accept a message from a second port". The one-shot is what
        // makes the Shell's always-first bootstrap load-bearing: without it, a document the panel
        // navigates to later could simply ask again.
        name: "panelport: the grant is ONE-SHOT — a second handshake from the same frame is refused",
        run: async (): Promise<void> => {
            const harness = createHarness();
            try {
                await waitForGrant(harness);
                const first = harness.bridge.port;
                harness.command({ cmd: "offer" });
                await waitFor(
                    "the second handshake to be refused",
                    () => harness.bridge.stats.refusedHandshakes >= 1,
                );
                await delay(100);
                assertEqual(
                    harness.bridge.stats.refusedHandshakes,
                    1,
                    "exactly one refusal — the second offer, and nothing else",
                );
                assert(
                    harness.bridge.port === first,
                    "the port is STILL the first one: a second handshake does not replace the grant, " +
                        "which is the difference between one-shot and last-one-wins",
                );
                // And the SECOND channel is dead: a request sent over it draws no reply, while the same
                // request over the FIRST channel does. Asserting both directions is what makes this a
                // statement about which port the host holds rather than about timing.
                harness.command({
                    cmd: "send",
                    index: 1,
                    envelope: requestEnvelope("second-port", "bridge.call"),
                });
                harness.command({
                    cmd: "send",
                    index: 0,
                    envelope: requestEnvelope("first-port", "bridge.call"),
                });
                const reply = await waitForReply(harness, "first-port");
                assertEqual(
                    (reply["error"] as Record<string, unknown> | undefined)?.["code"],
                    PANEL_BRIDGE_REFUSALS.verbNotGranted,
                    "the FIRST channel is live and answers",
                );
                assert(
                    replyFor(harness, "second-port") === undefined,
                    "and the SECOND channel never produced a reply — the host is not listening on it",
                );
            } finally {
                harness.dispose();
            }
        },
    },
    {
        name: "panelport: a handshake with no port, two ports, or the wrong tag is refused",
        run: async (): Promise<void> => {
            // A fresh harness per shape, because the one-shot means the FIRST offer would consume the
            // grant — each malformed shape has to be the opening one to prove anything.
            for (const command of ["offer-no-port", "offer-two-ports", "offer-wrong-tag"]) {
                const harness = createHarness({
                    childHtml: `<!doctype html><html><head><script>
                        window.addEventListener("message", function (event) {
                          var data = event.data;
                          if (!data || data.cmd !== ${JSON.stringify(command)}) { return; }
                          var TAG = ${JSON.stringify(EXT_PORT_HANDSHAKE_TAG)};
                          if (data.cmd === "offer-no-port") {
                            parent.postMessage({ ctx: TAG }, "*");
                          } else if (data.cmd === "offer-two-ports") {
                            var a = new MessageChannel();
                            var b = new MessageChannel();
                            parent.postMessage({ ctx: TAG }, "*", [a.port2, b.port2]);
                          } else {
                            var c = new MessageChannel();
                            parent.postMessage({ ctx: TAG + ".not" }, "*", [c.port2]);
                          }
                        });
                        </script></head><body>x</body></html>`,
                });
                try {
                    await waitFor("the child to load", () => harness.bridge.stats.loads >= 1);
                    harness.command({ cmd: command });
                    await waitFor(
                        `the ${command} handshake to be refused`,
                        () => harness.bridge.stats.refusedHandshakes >= 1,
                    );
                    assert(
                        !harness.bridge.granted,
                        `${command}: no grant. A handshake must carry EXACTLY one port and the exact ` +
                            "tag — two ports is a shape nothing sanctioned produces, and accepting " +
                            "port[0] from it would silently drop a live channel the peer believes it holds",
                    );
                    assert(harness.bridge.port === undefined, `${command}: and no port is held`);
                } finally {
                    harness.dispose();
                }
            }
        },
    },
    {
        // ⚠ PREMISE 3 + PLANT (c) FROM THE PARENT DoD — "skip revocation on re-navigation". This is the
        // `ext_scheme.h` hazard driven end to end: the panel navigates ITSELF (which a
        // `sandbox="allow-scripts"` frame may always do — sandbox restricts navigating the TOP), the
        // element sees a second load, and the port dies.
        name: "panelport: a self-navigation REVOKES the port (the ext_scheme.h obligation, driven live)",
        run: async (): Promise<void> => {
            const harness = createHarness();
            try {
                await waitForGrant(harness);
                await waitFor("the first load", () => harness.bridge.stats.loads >= 1);
                harness.command({ cmd: "navigate" });
                await waitFor(
                    "the second load event (the panel navigated itself)",
                    () => harness.bridge.stats.loads >= 2,
                );
                assert(
                    harness.bridge.revoked,
                    "the port is REVOKED on the second load. Skipping this is the third plant: the " +
                        "frame element editor-core believes hosts package A now hosts whatever A " +
                        "navigated to, and a live port would be that document's capability handle",
                );
                assert(harness.bridge.port === undefined, "the port handle is dropped");
                assert(
                    harness.bridge.granted,
                    "and `granted` STAYS latched — it records that a grant happened, so a handshake " +
                        "arriving now is refused by both latches rather than by whichever runs first",
                );
                assertEqual(
                    harness.frame.getAttribute(PANEL_PORT_STATE_ATTRIBUTE),
                    "revoked",
                    "the element reflects the revocation",
                );
                const reply = await harness.bridge.request("bridge.call");
                assertEqual(
                    reply.error?.code,
                    PANEL_BRIDGE_REFUSALS.portRevoked,
                    "and a host-side call over the dead port is refused, not left hanging",
                );
                assert(
                    !(await harness.bridge.waitForPort()),
                    "waitForPort resolves false after a revocation",
                );
            } finally {
                harness.dispose();
            }
        },
    },
    {
        name: "panelport: EVERY parked verb gets the identical, stable refusal (the deny-all table)",
        run: async (): Promise<void> => {
            const harness = createHarness();
            try {
                await waitForGrant(harness);
                for (const [index, verb] of PARKED_VERBS.entries()) {
                    const id = `parked-${String(index)}`;
                    harness.command({ cmd: "send", index: 0, envelope: requestEnvelope(id, verb) });
                    const reply = await waitForReply(harness, id);
                    assertEqual(
                        reply,
                        {
                            ctx: PANEL_BRIDGE_TAG,
                            v: PANEL_BRIDGE_VERSION,
                            kind: "reply",
                            id,
                            ok: false,
                            error: {
                                code: PANEL_BRIDGE_REFUSALS.verbNotGranted,
                                verb,
                                message:
                                    "this verb is not granted to package panels in this build",
                            },
                        },
                        `${verb}: the WHOLE refusal envelope is pinned, not just its code — e13b-2, ` +
                            "e13c and e13d fill verbs into this table and must not have to " +
                            "renegotiate the shape a refusal takes",
                    );
                }
                assertEqual(
                    harness.bridge.stats.refusedRequests,
                    PARKED_VERBS.length,
                    "each one counted exactly once",
                );
                assertEqual(
                    harness.bridge.stats.repliesSent,
                    PARKED_VERBS.length,
                    "and each was answered — a refusal a panel never receives is a hang, not a refusal",
                );
            } finally {
                harness.dispose();
            }
        },
    },
    {
        // THE DENY-ALL IS A LOOKUP MISS, NOT A HARDCODED `return refuse()`. Without this case every
        // negative above would pass just as happily against a bridge that can never answer anything —
        // which would be a transport that could not be extended without rewriting its core.
        name: "panelport: the verb table is a TABLE — an installed handler is invoked and its result returned",
        run: async (): Promise<void> => {
            const seen: unknown[] = [];
            const verbs = new Map<string, PanelVerbHandler>([
                [
                    "probe.granted",
                    (params: unknown): unknown => {
                        seen.push(params);
                        return { echoed: params };
                    },
                ],
            ]);
            const harness = createHarness({ verbs });
            try {
                await waitForGrant(harness);
                harness.command({
                    cmd: "send",
                    index: 0,
                    envelope: requestEnvelope("granted", "probe.granted", { params: { n: 7 } }),
                });
                const reply = await waitForReply(harness, "granted");
                assertEqual(reply["ok"], true, "an installed verb is ANSWERED, not refused");
                assertEqual(
                    reply["result"],
                    { echoed: { n: 7 } },
                    "its result travels back, and its params reached it",
                );
                assertEqual(seen, [{ n: 7 }], "the handler really ran, exactly once");

                // ...and a verb that is NOT in the table still gets the standard refusal, from the same
                // bridge instance. So the refusal is decided by the lookup.
                harness.command({
                    cmd: "send",
                    index: 0,
                    envelope: requestEnvelope("missing", "probe.absent"),
                });
                const refused = await waitForReply(harness, "missing");
                assertEqual(
                    (refused["error"] as Record<string, unknown>)["code"],
                    PANEL_BRIDGE_REFUSALS.verbNotGranted,
                    "an absent verb is refused by the SAME bridge that just answered a present one",
                );
            } finally {
                harness.dispose();
            }
        },
    },
    {
        name: "panelport: malformed envelopes are refused by shape, and uncorrelatable ones are dropped",
        run: async (): Promise<void> => {
            const harness = createHarness();
            try {
                await waitForGrant(harness);
                const before = harness.bridge.stats;

                // A wrong VERSION is answered — the panel can be told to speak v1.
                harness.command({
                    cmd: "send",
                    index: 0,
                    envelope: { ...requestEnvelope("versioned", "bridge.call"), v: 99 },
                });
                const versioned = await waitForReply(harness, "versioned");
                assertEqual(
                    (versioned["error"] as Record<string, unknown>)["code"],
                    PANEL_BRIDGE_REFUSALS.unsupportedVersion,
                    "an unsupported envelope version is refused as such",
                );
                assert(
                    (versioned["error"] as Record<string, unknown>)["verb"] === undefined,
                    "and echoes NO verb — the envelope was not understood well enough to name one",
                );

                // A missing / non-string / OVERSIZED verb is malformed but correlatable, so each is
                // answered. The oversized row is the load-bearing one: `verb` is ECHOED in the refusal,
                // so refusing rather than truncating is what stops an untrusted peer from choosing an
                // unbounded string that travels back out of the host — the same amplification
                // `clamp_logged_url` refuses on the native side.
                const oversized = "v".repeat(PANEL_BRIDGE_TOKEN_MAX_LENGTH + 1);
                const malformed: readonly (readonly [string, Record<string, unknown>])[] = [
                    [
                        "no-verb",
                        { ctx: PANEL_BRIDGE_TAG, v: PANEL_BRIDGE_VERSION, kind: "request", id: "no-verb" },
                    ],
                    ["num-verb", { ...requestEnvelope("num-verb", "x"), verb: 12 }],
                    ["long-verb", requestEnvelope("long-verb", oversized)],
                ];
                for (const [id, envelope] of malformed) {
                    harness.command({ cmd: "send", index: 0, envelope });
                    const reply = await waitForReply(harness, id);
                    assertEqual(
                        (reply["error"] as Record<string, unknown>)["code"],
                        PANEL_BRIDGE_REFUSALS.malformedRequest,
                        `${id}: refused as malformed`,
                    );
                    assert(
                        (reply["error"] as Record<string, unknown>)["verb"] === undefined,
                        `${id}: and echoes no verb — there was no usable one to name`,
                    );
                }

                // UNADDRESSED and UNCORRELATABLE envelopes get NO reply at all: answering them would
                // turn the port into an echo for anything a panel cares to send, and there is nowhere
                // to send a refusal for a request with no usable id.
                const repliesBefore = harness.bridge.stats.repliesSent;
                harness.command({ cmd: "send", index: 0, envelope: { hello: "world" } });
                harness.command({
                    cmd: "send",
                    index: 0,
                    envelope: { ...requestEnvelope("x", "bridge.call"), ctx: "some.other.protocol" },
                });
                harness.command({
                    cmd: "send",
                    index: 0,
                    envelope: { ...requestEnvelope("x", "bridge.call"), id: 5 },
                });
                harness.command({
                    cmd: "send",
                    index: 0,
                    envelope: { ...requestEnvelope("x", "bridge.call"), kind: "shout" },
                });
                await waitFor(
                    "the four unanswerable envelopes to be counted",
                    () => harness.bridge.stats.refusedRequests >= before.refusedRequests + 8,
                );
                await delay(100);
                assertEqual(
                    harness.bridge.stats.repliesSent,
                    repliesBefore,
                    "not one of the four produced a reply",
                );
            } finally {
                harness.dispose();
            }
        },
    },
    {
        name: "panelport: a host-issued request CORRELATES its reply, and TIMES OUT when none comes",
        run: async (): Promise<void> => {
            const harness = createHarness({ requestTimeoutMs: 120 });
            try {
                await waitForGrant(harness);
                // Correlation: the child echoes with the id it was given, and two concurrent requests
                // resolve to their OWN replies rather than to whichever arrived first.
                const [first, second] = await Promise.all([
                    harness.bridge.request("probe.one"),
                    harness.bridge.request("probe.two"),
                ]);
                assertEqual(first.ok, true, "the first request resolved");
                assertEqual(first.result, "echo:probe.one", "...to ITS own reply");
                assertEqual(second.result, "echo:probe.two", "and the second to its own");
                assertEqual(harness.bridge.stats.pending, 0, "no pending entry leaked");

                // Timeout: the child stops answering, so the budget decides.
                harness.command({ cmd: "silence" });
                await delay(20);
                const timedOut = await harness.bridge.request("probe.silent");
                assertEqual(
                    timedOut.error?.code,
                    PANEL_BRIDGE_REFUSALS.timeout,
                    "an unanswered host request resolves to a timeout refusal — it neither hangs nor " +
                        "rejects, because a renderer has nowhere to handle a rejection",
                );
                assertEqual(timedOut.error?.verb, "probe.silent", "and names the verb it was about");
                assertEqual(
                    harness.bridge.stats.pending,
                    0,
                    "and the pending entry is cleaned up, so a silent panel cannot grow the map",
                );
            } finally {
                harness.dispose();
            }
        },
    },
    {
        name: "panelport: the handshake window is BOUNDED — an unanswered offer revokes the grant",
        run: async (): Promise<void> => {
            const harness = createHarness({
                handshakeTimeoutMs: 60,
                childHtml: "<!doctype html><html><body>silent</body></html>",
            });
            try {
                assert(
                    !(await harness.bridge.waitForPort()),
                    "waitForPort reports the closed window rather than waiting forever",
                );
                assert(harness.bridge.revoked, "the grant window closed");
                assert(!harness.bridge.granted, "and nothing was ever granted");
                assertEqual(
                    harness.frame.getAttribute(PANEL_PORT_STATE_ATTRIBUTE),
                    "revoked",
                    "reported on the element",
                );
                const reply = await harness.bridge.request("bridge.call");
                assertEqual(
                    reply.error?.code,
                    PANEL_BRIDGE_REFUSALS.portRevoked,
                    "a call after the window closed is refused as revoked, not as merely unavailable",
                );
            } finally {
                harness.dispose();
            }
        },
    },
    {
        name: "panelport: dispose closes the port, settles pending work, and detaches the listener",
        run: async (): Promise<void> => {
            const harness = createHarness({ requestTimeoutMs: 5_000 });
            try {
                await waitForGrant(harness);
                harness.command({ cmd: "silence" });
                await delay(20);
                // A request in flight at dispose time must SETTLE, not linger to its budget.
                const pending = harness.bridge.request("probe.inflight");
                await waitFor("the request to be registered", () => harness.bridge.stats.pending === 1);
                harness.bridge.dispose();
                const reply = await pending;
                assertEqual(
                    reply.error?.code,
                    PANEL_BRIDGE_REFUSALS.portRevoked,
                    "an in-flight request settles on dispose",
                );
                assert(harness.bridge.revoked, "dispose revokes");
                assertEqual(harness.bridge.stats.pending, 0, "and leaves nothing pending");

                // THE LISTENER IS GONE. A further handshake from the (still live) child must not even
                // be counted: a disposed bridge that kept listening would keep a detached frame's
                // renderer alive and would start comparing `null` against every other panel's messages.
                const refusedBefore = harness.bridge.stats.refusedHandshakes;
                harness.command({ cmd: "offer" });
                await delay(150);
                assertEqual(
                    harness.bridge.stats.refusedHandshakes,
                    refusedBefore,
                    "a disposed bridge observes nothing at all",
                );
                // And it is idempotent.
                harness.bridge.dispose();
                assert(harness.bridge.revoked, "a second dispose is a no-op");
            } finally {
                harness.dispose();
            }
        },
    },
];
