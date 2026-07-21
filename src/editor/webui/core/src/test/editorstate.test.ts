// T1 unit tests for `parsePersistedState` (M9 e07a). The Shell round-trips the editor-state blob and
// editor-core must be TOTAL against a malformed or partial envelope (editorstate.ts) — a corrupt
// persisted blob degrades to "fresh project" rather than throwing on boot.

import { assert, assertEqual, type TestCase } from "./harness.js";
import { parsePersistedState } from "../editorstate.js";

export const editorstateTests: readonly TestCase[] = [
    {
        name: "parsePersistedState: a non-record degrades to a fresh {layout:null, panels:{}}",
        run: () => {
            assertEqual(parsePersistedState(null), { layout: null, panels: {} }, "null");
            assertEqual(parsePersistedState("nope"), { layout: null, panels: {} }, "string");
            assertEqual(parsePersistedState(42), { layout: null, panels: {} }, "number");
        },
    },
    {
        name: "parsePersistedState: a full blob passes layout + panels through",
        run: () => {
            const parsed = parsePersistedState({
                layout: { grid: { root: {} } },
                panels: { problems: { schemaVersion: 1, data: {} } },
            });
            assertEqual(parsed.layout, { grid: { root: {} } }, "layout passthrough");
            assertEqual(parsed.panels, { problems: { schemaVersion: 1, data: {} } }, "panels passthrough");
        },
    },
    {
        name: "parsePersistedState: a non-record panels member is replaced with an empty map",
        run: () => {
            const parsed = parsePersistedState({ layout: null, panels: "corrupt" });
            assertEqual(parsed.panels, {}, "corrupt panels -> {}");
        },
    },
    {
        name: "parsePersistedState: an absent layout reads as null (not undefined)",
        run: () => {
            const parsed = parsePersistedState({ panels: {} });
            assert(parsed.layout === null, "absent layout must be null");
        },
    },
];
