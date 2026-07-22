// T1 unit tests for the structural guards editor-core boots through (M9 e07a): `isRecord`, and the
// two "detect a global, degrade to undefined when absent" readers `detectBridgeQuery` /
// `detectDockview`. Both detectors MUST stay loadable outside the Shell (a plain browser, this very
// harness) rather than throwing at import time (bridge.ts / dockview.ts) — so a wrong/absent global
// is `undefined`, never an exception.

import { assert, assertEqual, type TestCase } from "./harness.js";
import { isRecord, detectBridgeQuery, BRIDGE_QUERY_FUNCTION } from "../bridge.js";
import { detectDockview, DOCKVIEW_GLOBAL } from "../dockview.js";

export const guardsTests: readonly TestCase[] = [
    {
        name: "isRecord: only a non-null, non-array object is a record",
        run: () => {
            assertEqual(isRecord({}), true, "empty object");
            assertEqual(isRecord({ a: 1 }), true, "populated object");
            assertEqual(isRecord(null), false, "null");
            assertEqual(isRecord([]), false, "array");
            assertEqual(isRecord("s"), false, "string");
            assertEqual(isRecord(42), false, "number");
            assertEqual(isRecord(undefined), false, "undefined");
        },
    },
    {
        name: "detectBridgeQuery: returns the injected function, else undefined",
        run: () => {
            const query = (): number => 0;
            const found = detectBridgeQuery({ [BRIDGE_QUERY_FUNCTION]: query });
            assert(found === query, "the injected query function is returned as-is");
            assert(detectBridgeQuery({}) === undefined, "absent -> undefined");
            assert(
                detectBridgeQuery({ [BRIDGE_QUERY_FUNCTION]: "not a function" }) === undefined,
                "a non-function value -> undefined",
            );
            assert(detectBridgeQuery(null) === undefined, "a non-record scope -> undefined");
        },
    },
    {
        name: "detectDockview: a global carrying createDockview is the module; a wrong shape is undefined",
        run: () => {
            const module = { createDockview: (): unknown => ({}), themeDark: {} };
            assert(
                detectDockview({ [DOCKVIEW_GLOBAL]: module }) !== undefined,
                "a well-shaped global is the module",
            );
            assert(
                detectDockview({ [DOCKVIEW_GLOBAL]: { nope: true } }) === undefined,
                "a global without createDockview -> undefined (stale/wrong artifact)",
            );
            assert(detectDockview({}) === undefined, "absent -> undefined");
            assert(detectDockview(null) === undefined, "a non-record scope -> undefined");
        },
    },
];
