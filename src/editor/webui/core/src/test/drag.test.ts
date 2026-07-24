// T1 unit tests for the `drag.*` client + the cross-window drag pump (M9 e10c). Like window.ts before
// it, a defect here is INVISIBLE at runtime until a live-CEF-smoke red one CI round away: the editor is
// up, nothing errors, and a cross-window drop silently does nothing. The properties pinned here are the
// ones whose failure hides:
//
//   * `pumpCrossWindowDrag` does NOTHING when no cross-window drag is active — so in-window Dockview DnD
//     is never intercepted (design 04 §2's "within one window, Dockview native DnD");
//   * when a drag IS over this window, the pump hit-tests the LOCAL layout and reports the zone back for
//     the SAME generation the probe carried — the genuine cross-origin round trip;
//   * a Shell with no drag surface (an older build) is an ordinary "no drag" state (INACTIVE probe),
//     never a thrown exception.

import { assert, assertEqual, type TestCase } from "./harness.js";
import { ShellBridge, type BridgeQuery, type BridgeQueryFunction } from "../bridge.js";
import {
    DragClient,
    makeDropZoneHitTest,
    parseProbe,
    pumpCrossWindowDrag,
    type DropZone,
} from "../drag.js";

type Responder = (method: string, params: Record<string, unknown>) => unknown;

/** A ShellBridge whose Shell answers per-method from `responder` (a refusal on a thrown responder). */
function bridgeFor(responder: Responder): ShellBridge {
    const query: BridgeQueryFunction = (request: BridgeQuery): number => {
        const parsed = JSON.parse(request.request) as {
            id: number;
            method: string;
            params: Record<string, unknown>;
        };
        try {
            const result = responder(parsed.method, parsed.params ?? {});
            request.onSuccess(JSON.stringify({ jsonrpc: "2.0", id: parsed.id, result }));
        } catch (error) {
            request.onSuccess(
                JSON.stringify({
                    jsonrpc: "2.0",
                    id: parsed.id,
                    error: { code: -32000, message: String(error), data: { reason: "test.refused" } },
                }),
            );
        }
        return parsed.id;
    };
    return new ShellBridge(query);
}

/** A hit-tester over a fake element of `w × h`, so the round-trip test needs no real DOM. */
function boxHitTest(w: number, h: number): ReturnType<typeof makeDropZoneHitTest> {
    const fake = { getBoundingClientRect: () => ({ width: w, height: h }) } as unknown as HTMLElement;
    return makeDropZoneHitTest(() => fake);
}

export const dragTests: readonly TestCase[] = [
    {
        name: "parseProbe: active parses; anything else is the inactive probe",
        run: () => {
            const p = parseProbe({ active: true, panelId: "builtin.problems", x: 80, y: 40, generation: 3 });
            assert(p.active, "active");
            assertEqual(p.panelId, "builtin.problems", "panelId");
            assertEqual(p.x, 80, "x");
            assertEqual(p.generation, 3, "generation");
            assert(!parseProbe({ active: false }).active, "active:false -> inactive");
            assert(!parseProbe("nope").active, "non-record -> inactive");
            assert(!parseProbe({}).active, "no active member -> inactive");
        },
    },
    {
        name: "DragClient.probe: parses an active hover; a refusal is the inactive probe (never throws)",
        run: async () => {
            const active = new DragClient(
                bridgeFor(() => ({ active: true, panelId: "p", x: 10, y: 20, generation: 5 })),
            );
            const p = await active.probe();
            assert(p.active && p.panelId === "p" && p.generation === 5, "active hover parsed");

            const refusing = new DragClient(
                bridgeFor(() => {
                    throw new Error("no drag surface");
                }),
            );
            assert(!(await refusing.probe()).active, "a refusal -> inactive, not a throw");
        },
    },
    {
        name: "pumpCrossWindowDrag: an active drag hit-tests + reports the zone for the SAME generation",
        run: async () => {
            let reportCalls = 0;
            let reported: Record<string, unknown> = {};
            const bridge = bridgeFor((method, params) => {
                if (method === "drag.probe") {
                    return { active: true, panelId: "p", x: 30, y: 40, generation: 9 };
                }
                if (method === "drag.report-zone") {
                    reportCalls += 1;
                    reported = params;
                    return { recorded: true };
                }
                return {};
            });
            const probe = await pumpCrossWindowDrag(new DragClient(bridge), boxHitTest(200, 200));
            assert(probe.active, "the pump saw the active drag");
            assertEqual(reportCalls, 1, "it reported a zone back — the cross-origin round trip");
            assertEqual(reported["valid"], true, "the cursor is inside -> valid zone");
            assertEqual(reported["generation"], 9, "reports for the probe's generation");
        },
    },
    {
        name: "pumpCrossWindowDrag: NO active drag -> reports nothing (in-window Dockview DnD untouched)",
        run: async () => {
            let reportCalls = 0;
            const bridge = bridgeFor((method) => {
                if (method === "drag.probe") {
                    return { active: false };
                }
                if (method === "drag.report-zone") {
                    reportCalls += 1;
                    return { recorded: true };
                }
                return {};
            });
            await pumpCrossWindowDrag(new DragClient(bridge), boxHitTest(200, 200));
            assertEqual(reportCalls, 0, "an inactive probe reports nothing — the pump is a pure no-op");
        },
    },
    {
        name: "makeDropZoneHitTest: valid inside the window box; invalid outside / with no root",
        run: () => {
            const hit = boxHitTest(100, 80);
            const inside: DropZone = hit(50, 40);
            assert(inside.valid && inside.zoneId === "center", "inside -> a valid center zone");
            assert(!hit(120, 40).valid, "past the right edge -> no zone");
            assert(!hit(50, 90).valid, "past the bottom edge -> no zone");
            assert(!hit(-1, 40).valid, "left of the origin -> no zone");
            const noRoot = makeDropZoneHitTest(() => null);
            assert(!noRoot(10, 10).valid, "no DOM root -> no zone (never throws)");
        },
    },
];
