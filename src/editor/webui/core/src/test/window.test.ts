// T1 unit tests for the `window.*` client + parsers (M9 e10b). This is editor-core's ONLY path to a
// tear-out / move / rehome (window.ts), and — like session.ts before it — a defect here is INVISIBLE
// at runtime until a live-CEF-smoke red one CI round away: the editor is up, nothing errors, and a
// panel move silently does nothing. The properties pinned here are the ones whose failure hides:
//
//   * a tear-out FAILURE is a LOUD result (`created:false` + the outcome token + a reason), NEVER a
//     thrown exception nor a silent success (03 §7) — the whole point of the degrade path;
//   * a bridge REFUSAL (a build with no window factory) degrades to that same loud result rather than
//     rejecting, so the caller's degrade branch is a plain read, not a try/catch;
//   * the D6 state blob is OPAQUE — carried verbatim through seed / move / rehome — so a value a
//     fresh panel could NOT have (a typed input, a scroll offset) survives the relay byte-for-byte
//     (standing lesson #2).

import { assert, assertEqual, assertNull, type TestCase } from "./harness.js";
import { ShellBridge, type BridgeQuery, type BridgeQueryFunction } from "../bridge.js";
import { WindowClient, parseWindowSeed, parseRehomed } from "../window.js";

/** A responder returns a method's result, or THROWS to make the Shell answer a refusal envelope. */
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

// A D6 blob a FRESH panel could not reproduce — a typed-in query plus a scroll offset.
const IMPOSSIBLE_STATE = { schemaVersion: 1, data: { query: "half-typed", scrollTop: 4096 } };

export const windowTests: readonly TestCase[] = [
    // --- the total parsers ------------------------------------------------------------------------
    {
        name: "parseWindowSeed: keeps the opaque state verbatim; fails closed on no id",
        run: () => {
            const seed = parseWindowSeed({ panelId: "builtin.problems", state: IMPOSSIBLE_STATE });
            assert(seed !== null, "a seed with an id parses");
            assertEqual(seed?.panelId, "builtin.problems", "panelId");
            assertEqual(seed?.state, IMPOSSIBLE_STATE, "the opaque D6 blob survives verbatim");
            assertNull(parseWindowSeed({ state: IMPOSSIBLE_STATE }), "no panelId -> null");
            assertNull(parseWindowSeed("nope"), "non-record -> null");
            // A panel that persists no state is legitimate — the seed still parses with null state.
            assertEqual(parseWindowSeed({ panelId: "p" })?.state, null, "missing state -> null (persists none)");
        },
    },
    {
        name: "parseRehomed: parses the panel list, drops id-less entries, empty on a bad envelope",
        run: () => {
            const seeds = parseRehomed({
                panels: [
                    { panelId: "a", state: IMPOSSIBLE_STATE },
                    { state: { data: 1 } }, // no id — dropped
                    { panelId: "b", state: null },
                ],
            });
            assertEqual(seeds.length, 2, "only the two id-bearing seeds survive");
            assertEqual(seeds.map((s) => s.panelId), ["a", "b"], "order preserved");
            assertEqual(seeds[0]?.state, IMPOSSIBLE_STATE, "opaque state carried through");
            assertEqual(parseRehomed({ panels: "nope" }).length, 0, "non-array panels -> empty");
            assertEqual(parseRehomed(null).length, 0, "non-record -> empty");
        },
    },

    // --- list -------------------------------------------------------------------------------------
    {
        name: "WindowClient.list: reports this window + peers; null on an unreadable envelope",
        run: async () => {
            const client = new WindowClient(bridgeFor(() => ({ windowId: 1, windows: [0, 1, 2] })));
            const list = await client.list();
            assertEqual(list, { windowId: 1, windows: [0, 1, 2] }, "windowId + peer ids");
            const bad = new WindowClient(bridgeFor(() => ({ windowId: 0 }))); // no windows array
            assertNull(await bad.list(), "missing windows array -> null");
        },
    },

    // --- tear-out: the LOUD degradation contract (03 §7) ------------------------------------------
    {
        name: "WindowClient.tearOut: a success reports the new window id",
        run: async () => {
            let sentPanel = "";
            let sentState: unknown = undefined;
            const client = new WindowClient(
                bridgeFor((method, params) => {
                    assertEqual(method, "window.tear-out", "the tear-out method name");
                    sentPanel = String(params["panelId"]);
                    sentState = params["state"];
                    return { created: true, windowId: 3, outcome: "created", error: "" };
                }),
            );
            const result = await client.tearOut("builtin.inspector", IMPOSSIBLE_STATE);
            assert(result.created, "created");
            assertEqual(result.windowId, 3, "the new window id");
            assertEqual(sentPanel, "builtin.inspector", "the panel id reached the Shell");
            // The opaque state a fresh panel could not have travelled to the Shell verbatim.
            assertEqual(sentState, IMPOSSIBLE_STATE, "the D6 state blob reached the Shell verbatim");
        },
    },
    {
        name: "WindowClient.tearOut: a create FAILURE is a loud result, not a throw",
        run: async () => {
            const client = new WindowClient(
                bridgeFor(() => ({
                    created: false,
                    windowId: 0,
                    outcome: "factory_failed",
                    error: "no native window backend on this platform",
                })),
            );
            const result = await client.tearOut("builtin.problems", null);
            assert(!result.created, "created is false — the create failed");
            assertEqual(result.outcome, "factory_failed", "the WindowCreateOutcome token is carried");
            assert(result.error.length > 0, "a human reason the user can be shown");
        },
    },
    {
        name: "WindowClient.tearOut: a bridge REFUSAL degrades loudly (never rejects)",
        run: async () => {
            // The Shell refused the call outright (a build with no window factory). The caller must
            // still get a loud, actionable result to degrade on — not an exception to catch.
            const client = new WindowClient(
                bridgeFor(() => {
                    throw new Error("no window factory is bound in this build");
                }),
            );
            const result = await client.tearOut("builtin.problems", null);
            assert(!result.created, "a refusal is a loud created:false");
            assertEqual(result.outcome, "refused", "the refusal is named as such");
            assert(result.error.length > 0, "the refusal reason is carried");
        },
    },

    // --- move-to / seed / rehomed: the same relay, the target's two read moments ------------------
    {
        name: "WindowClient.moveTo: a success reports moved; a refusal is moved:false",
        run: async () => {
            let target = -1;
            const client = new WindowClient(
                bridgeFor((_method, params) => {
                    target = Number(params["windowId"]);
                    return { moved: true, windowId: target, error: "" };
                }),
            );
            const result = await client.moveTo("builtin.problems", IMPOSSIBLE_STATE, 0);
            assert(result.moved, "moved");
            assertEqual(target, 0, "the target window id reached the Shell");

            const refused = new WindowClient(
                bridgeFor(() => {
                    throw new Error("window.unknown_target");
                }),
            );
            const bad = await refused.moveTo("builtin.problems", null, 9);
            assert(!bad.moved, "a refused move is moved:false, not a throw");
        },
    },
    {
        name: "WindowClient.seed: a seeded window gets its panel + opaque state; an ordinary one does not",
        run: async () => {
            const seeded = new WindowClient(
                bridgeFor(() => ({ seeded: true, panelId: "builtin.inspector", state: IMPOSSIBLE_STATE })),
            );
            const boot = await seeded.seed();
            assert(boot.seeded, "seeded");
            assertEqual(boot.seed?.panelId, "builtin.inspector", "the moved panel id");
            // The value a fresh panel could not have arrived intact — the whole DoD of tear-out.
            assertEqual(boot.seed?.state, IMPOSSIBLE_STATE, "the D6 state survived the relay verbatim");

            const ordinary = new WindowClient(bridgeFor(() => ({ seeded: false })));
            const none = await ordinary.seed();
            assert(!none.seeded, "an ordinary window reports seeded:false");
            assertNull(none.seed, "no seed");
        },
    },
    {
        name: "WindowClient.rehomed: drains the queued panels; empty on a refusal",
        run: async () => {
            const client = new WindowClient(
                bridgeFor(() => ({
                    panels: [{ panelId: "builtin.problems", state: IMPOSSIBLE_STATE }],
                })),
            );
            const seeds = await client.rehomed();
            assertEqual(seeds.length, 1, "one panel rehomed in");
            assertEqual(seeds[0]?.panelId, "builtin.problems", "the rehomed panel id");
            assertEqual(seeds[0]?.state, IMPOSSIBLE_STATE, "opaque state carried through");

            const refused = new WindowClient(
                bridgeFor(() => {
                    throw new Error("unknown_method");
                }),
            );
            assertEqual((await refused.rehomed()).length, 0, "a refusal drains to empty, never throws");
        },
    },
    {
        name: "WindowClient.close: a success reports closed; a refusal is closed:false",
        run: async () => {
            const client = new WindowClient(
                bridgeFor(() => ({ closed: true, outcome: "destroyed", error: "" })),
            );
            assert((await client.close()).closed, "closed");
            const refused = new WindowClient(
                bridgeFor(() => {
                    throw new Error("primary_refused");
                }),
            );
            const bad = await refused.close();
            assert(!bad.closed, "a refused close (the primary) is closed:false, not a throw");
        },
    },
];
