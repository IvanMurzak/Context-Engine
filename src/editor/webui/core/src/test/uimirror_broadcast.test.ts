// T1 for the cross-window `editor.ui` mirror TRANSPORT (M9 e10d, design 05 §5, D7 tier 2) — the TS
// half of INHERITED DRILL 2.
//
// WHAT THIS PROVES THAT e08c COULD NOT. e08c unit-tested the two `receiveMirrored` loop breakers in
// isolation, but its two-bus ring drill only ever exercised the POINT-TO-POINT one (a mirrored
// envelope is not re-delivered to that bus's own sinks). The OTHER breaker — an envelope arriving back
// at its OWN `origin` is dropped — is the one a BROADCASTING transport needs, and e08c's ring drill
// could not light it because a point-to-point ring never delivers a sender its own frame back. This
// file drives the REAL `ShellUiMirrorSink` + REAL `UiMirrorPoller` + REAL `EditorUiBus.receiveMirrored`
// through a transport with the ACTUAL Shell's broadcasting shape (`window_bridge.h` `ui.mirror` fans an
// envelope to EVERY window including the sender), and asserts:
//   * CONVERGENCE — a fact published in window A reaches window B and B's bus applies + retains it;
//   * ECHO SUPPRESSION IN THE BROADCASTING SHAPE — window A gets its OWN envelope back and DROPS it
//     (its subscriber does not fire twice, its `receiveMirrored` reports the own-origin refusal). A
//     single-window build has one bus and no second origin, so this A-applies-none / B-applies-one
//     distinction cannot exist there — which is exactly why the drill waited for a real second window.

import { assert, assertEqual, type TestCase } from "./harness.js";
import { EditorUiBus, UI_TOPIC_THEME_CHANGED, type EditorUiEvent } from "../uibus.js";
import { parseMirroredEvent, UiMirrorPoller, wireUiMirror } from "../uimirror.js";
import type { ShellBridge } from "../bridge.js";

/**
 * A fake Shell with the REAL relay's broadcasting shape: `ui.mirror` fans an envelope into EVERY
 * window's queue INCLUDING the sender's; `ui.mirror-poll` drains the calling window's queue. One
 * shared instance backs both windows' bridges, because a broadcast from A must reach B's queue —
 * exactly what the C++ `UiMirrorStore` + the bridge fan-out do (asserted C++-side in test_ui_mirror.cpp).
 */
class SharedBroadcastShell {
    readonly #queues = new Map<string, Record<string, unknown>[]>();
    readonly #windows: readonly string[];

    constructor(windows: readonly string[]) {
        this.#windows = windows;
        for (const id of windows) {
            this.#queues.set(id, []);
        }
    }

    /** A `ShellBridge`-shaped view for one window (the sink/poller only use `.call`). */
    bridgeFor(selfId: string): ShellBridge {
        const call = (method: string, params?: Record<string, unknown>): Promise<unknown> => {
            if (method === "ui.mirror" && params !== undefined) {
                // BROADCAST to every window, the sender included — the shape that arms the echo drop.
                for (const id of this.#windows) {
                    this.#queues.get(id)?.push(params);
                }
                return Promise.resolve({ mirrored: true, windows: this.#windows.length });
            }
            if (method === "ui.mirror-poll") {
                const pending = this.#queues.get(selfId) ?? [];
                this.#queues.set(selfId, []);
                return Promise.resolve({ events: pending });
            }
            return Promise.resolve(null);
        };
        return { call } as unknown as ShellBridge;
    }
}

/** A wired window: its bus, its inbound poller, and a subscriber's fire count on theme-changed. */
interface Window {
    bus: EditorUiBus;
    poller: UiMirrorPoller;
    themeFires: () => number;
}

function wireWindow(shell: SharedBroadcastShell, originId: string): Window {
    const bus = new EditorUiBus({ origin: originId });
    let fires = 0;
    bus.subscribe(UI_TOPIC_THEME_CHANGED, () => {
        fires += 1;
    });
    const { poller } = wireUiMirror(shell.bridgeFor(originId), bus);
    return { bus, poller, themeFires: () => fires };
}

export const uimirrorBroadcastTests: readonly TestCase[] = [
    {
        name: "broadcast mirror: A's publish converges in B, and A drops its OWN echo",
        run: async () => {
            const shell = new SharedBroadcastShell(["0", "1"]);
            const a = wireWindow(shell, "0");
            const b = wireWindow(shell, "1");

            // Window A publishes a theme-changed fact. LOCALLY, A's subscriber fires exactly once and
            // the sink hands the envelope to the (broadcasting) Shell — which queues it for BOTH A and B.
            const report = a.bus.publish(UI_TOPIC_THEME_CHANGED, { variant: "dark" });
            assert(report.published, "the local publish succeeded");
            assertEqual(a.themeFires(), 1, "A's own subscriber fired once from the LOCAL publish");
            // Let the fire-and-forget sink's broadcast settle (it enqueues synchronously, but await a
            // tick so the test is honest about the async transport boundary).
            await Promise.resolve();

            // CONVERGENCE: window B drains the broadcast and APPLIES it (foreign origin "0").
            const bApplied = await b.poller.poll();
            assertEqual(bApplied, 1, "B applied exactly one mirrored envelope");
            assertEqual(b.themeFires(), 1, "B's subscriber fired from the mirrored fact");
            const bSnap = b.bus.snapshot(UI_TOPIC_THEME_CHANGED);
            assert(bSnap !== undefined, "B retained the mirrored fact (snapshot-on-subscribe honoured)");
            assertEqual(bSnap?.origin, "0", "B sees the fact stamped with A's origin, not its own");
            assertEqual((bSnap?.payload as { variant: string }).variant, "dark", "the payload converged");

            // ECHO SUPPRESSION: window A drains its OWN envelope back (the broadcasting shape delivered
            // it to A too) and DROPS it — nothing applied, its subscriber does NOT fire a second time.
            const aApplied = await a.poller.poll();
            assertEqual(aApplied, 0, "A applied NOTHING — its own echo was suppressed by origin");
            assertEqual(a.themeFires(), 1, "A's subscriber did NOT fire again from its own echo");
            // ...and the drop is REPORTED, the specific branch e08c could not reach end to end.
            const echoed = a.bus.rejections.some((r) => r.diagnostic.includes("echoed back to its own origin"));
            assert(echoed, "A recorded the own-origin echo refusal (the broadcasting loop breaker)");
        },
    },
    {
        name: "broadcast mirror: SYMMETRY — B's publish converges in A, and B drops its own echo",
        run: async () => {
            const shell = new SharedBroadcastShell(["0", "1"]);
            const a = wireWindow(shell, "0");
            const b = wireWindow(shell, "1");

            b.bus.publish(UI_TOPIC_THEME_CHANGED, { variant: "light" });
            await Promise.resolve();

            assertEqual(await a.poller.poll(), 1, "A applied B's mirrored fact");
            assertEqual(a.bus.snapshot(UI_TOPIC_THEME_CHANGED)?.origin, "1", "A sees B's origin");
            assertEqual(await b.poller.poll(), 0, "B dropped its own echo");
            assertEqual(b.themeFires(), 1, "B's subscriber fired once (local), not from its echo");
        },
    },
    {
        name: "broadcast mirror: a third window with no publish of its own still converges on A's fact",
        run: async () => {
            // Three windows: A publishes, and BOTH B and C converge — N windows, one truth (05 §5).
            const shell = new SharedBroadcastShell(["0", "1", "2"]);
            const a = wireWindow(shell, "0");
            const b = wireWindow(shell, "1");
            const c = wireWindow(shell, "2");

            a.bus.publish(UI_TOPIC_THEME_CHANGED, { variant: "dark" });
            await Promise.resolve();

            assertEqual(await b.poller.poll(), 1, "B converged");
            assertEqual(await c.poller.poll(), 1, "C converged");
            assertEqual(await a.poller.poll(), 0, "A suppressed its own echo");
            assertEqual(c.bus.snapshot(UI_TOPIC_THEME_CHANGED)?.origin, "0", "C sees A's origin");
        },
    },
    {
        name: "parseMirroredEvent: total against a malformed batch entry",
        run: () => {
            const ok = parseMirroredEvent({ seq: 4, topic: "editor.ui.focus", origin: "1", payload: 7 });
            assert(ok !== null, "a well-formed envelope parses");
            assertEqual((ok as EditorUiEvent).origin, "1", "origin narrows");
            assertEqual((ok as EditorUiEvent).seq, 4, "seq narrows");
            // A frame with no topic or no origin cannot be applied OR echo-suppressed — dropped, not
            // half-applied.
            assert(parseMirroredEvent({ topic: "editor.ui.focus" }) === null, "missing origin -> null");
            assert(parseMirroredEvent({ origin: "1" }) === null, "missing topic -> null");
            assert(parseMirroredEvent(null) === null, "a non-object -> null");
            // A missing payload keeps null rather than throwing (opaque, carried verbatim).
            const noPayload = parseMirroredEvent({ topic: "editor.ui.layout", origin: "0" });
            assert(noPayload !== null && noPayload.payload === null, "a missing payload reads as null");
        },
    },
];
