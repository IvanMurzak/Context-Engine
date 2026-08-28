// T1 unit tests for `parsePersistedState` (M9 e07a). The Shell round-trips the editor-state blob and
// editor-core must be TOTAL against a malformed or partial envelope (editorstate.ts) — a corrupt
// persisted blob degrades to "fresh project" rather than throwing on boot.

import { assert, assertEqual, type TestCase } from "./harness.js";
import {
    REGION_KIND_CAPTION,
    REGION_KIND_CAPTION_CLOSE,
    REGION_KIND_CAPTION_MAX,
    REGION_KIND_CAPTION_MIN,
    REGION_KIND_NATIVE,
    REGION_KIND_VIEWPORT,
    parsePersistedState,
} from "../editorstate.js";

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
    {
        name: "regions: the closed RegionKind vocabulary is exactly the six wire tokens",
        run: () => {
            // Pinned as LITERALS (the uibus closed-set rationale): these are the strings the Shell
            // parses (editor_state_bridge.h) and the `webui-panel-contract` gate byte-compares, so
            // this case reds if either the two originals or editor-window-chrome a1's four caption
            // tokens (02 §6) drift on this side. Note the hyphenated spellings: the C++ ENUM says
            // `caption_min`, the WIRE says `caption-min`, and the Shell refuses the underscore form.
            assertEqual(REGION_KIND_VIEWPORT, "viewport", "viewport");
            assertEqual(REGION_KIND_NATIVE, "native", "native");
            assertEqual(REGION_KIND_CAPTION, "caption", "caption");
            assertEqual(REGION_KIND_CAPTION_MIN, "caption-min", "caption-min");
            assertEqual(REGION_KIND_CAPTION_MAX, "caption-max", "caption-max");
            assertEqual(REGION_KIND_CAPTION_CLOSE, "caption-close", "caption-close");
        },
    },
];
