// T1 unit tests for the `panel.*` total parsers (M9 e07a). These are the widest cross-language seam
// in the editor (panels.ts note 1) and every parser is designed to be TOTAL — `null`/fail-closed on a
// malformed envelope rather than surfacing `undefined` deep in the DOM. This tier pins that contract.

import { assert, assertEqual, assertNull, type TestCase } from "./harness.js";
import { parsePanelManifest, parsePanelRoster, parsePanelRender } from "../panels.js";

export const panelsTests: readonly TestCase[] = [
    {
        name: "parsePanelManifest: a full manifest maps every field",
        run: () => {
            const manifest = parsePanelManifest({
                id: "problems",
                kind: "diagnostics",
                title: "Problems",
                icon: "warning",
                contractVersion: 3,
                dock: { zone: "bottom", singleton: true, minWidth: 120, minHeight: 80 },
                content: { type: "uitree" },
                state: { schemaVersion: 2 },
                capabilities: ["read", "focus"],
                hosted: true,
                gestures: false,
                persists: true,
                revision: 7,
            });
            assert(manifest !== null, "a well-formed manifest must parse");
            assertEqual(manifest?.id, "problems", "id");
            assertEqual(manifest?.title, "Problems", "title");
            assertEqual(manifest?.contentType, "uitree", "contentType");
            assertEqual(manifest?.dock.zone, "bottom", "dock.zone");
            assertEqual(manifest?.dock.singleton, true, "dock.singleton");
            assertEqual(manifest?.schemaVersion, 2, "schemaVersion (read off state)");
            assertEqual(manifest?.capabilities, ["read", "focus"], "capabilities");
            assertEqual(manifest?.hosted, true, "hosted");
        },
    },
    {
        name: "parsePanelManifest: a missing id fails closed to null",
        run: () => {
            assertNull(parsePanelManifest({ title: "no id here" }), "no id");
            assertNull(parsePanelManifest("not even an object"), "non-record");
            assertNull(parsePanelManifest(null), "null");
        },
    },
    {
        name: "parsePanelManifest: an unknown content type fails closed to 'unknown' (the HTML-sink guard)",
        run: () => {
            // An unrecognised content.type must NOT default into the innerHTML sink — panels.ts §
            // readContentType is deliberately NOT defaulted to uitree.
            assertEqual(
                parsePanelManifest({ id: "p", content: { type: "iframe-of-doom" } })?.contentType,
                "unknown",
                "unrecognised content type",
            );
            assertEqual(
                parsePanelManifest({ id: "p" })?.contentType,
                "unknown",
                "missing content type",
            );
            assertEqual(
                parsePanelManifest({ id: "p", content: { type: "iframe" } })?.contentType,
                "iframe",
                "iframe stays a recognised (still non-hostable) type",
            );
        },
    },
    {
        name: "parsePanelManifest: an unknown dock zone falls back to center; non-string caps are dropped",
        run: () => {
            const manifest = parsePanelManifest({
                id: "p",
                dock: { zone: "diagonal" },
                capabilities: ["ok", 42, null, "fine"],
            });
            assertEqual(manifest?.dock.zone, "center", "drifted zone -> center");
            assertEqual(manifest?.capabilities, ["ok", "fine"], "non-string capabilities dropped");
        },
    },
    {
        name: "parsePanelRoster: a valid roster keeps parseable panels and skips the rest",
        run: () => {
            const roster = parsePanelRoster({
                contractMajor: 1,
                panels: [
                    { id: "a" },
                    { title: "no id — dropped" },
                    { id: "b", hosted: true },
                    "garbage entry",
                ],
            });
            assert(roster !== null, "a well-formed roster must parse");
            assertEqual(roster?.contractMajor, 1, "contractMajor");
            assertEqual(roster?.panels.length, 2, "only the two id-bearing entries survive");
            assertEqual(roster?.panels.map((p) => p.id), ["a", "b"], "surviving ids in order");
        },
    },
    {
        name: "parsePanelRoster: a non-array panels member fails closed to null",
        run: () => {
            assertNull(parsePanelRoster({ contractMajor: 1, panels: "nope" }), "panels not an array");
            assertNull(parsePanelRoster({ contractMajor: 1 }), "panels absent");
            assertNull(parsePanelRoster(null), "non-record");
        },
    },
    {
        name: "parsePanelRender: a valid render parses html + focus order + commands",
        run: () => {
            const render = parsePanelRender({
                panelId: "problems",
                revision: 4,
                html: "<ul></ul>",
                focusOrder: ["row-0", "row-1"],
                commands: [{ id: "clear", title: "Clear" }, { title: "no id — dropped" }],
            });
            assert(render !== null, "a well-formed render must parse");
            assertEqual(render?.panelId, "problems", "panelId");
            assertEqual(render?.html, "<ul></ul>", "html");
            assertEqual(render?.focusOrder, ["row-0", "row-1"], "focusOrder");
            assertEqual(render?.commands, [{ id: "clear", title: "Clear" }], "commands (id-less dropped)");
        },
    },
    {
        name: "parsePanelRender: a missing panelId fails closed to null",
        run: () => {
            assertNull(parsePanelRender({ html: "<div/>" }), "no panelId");
            assertNull(parsePanelRender(null), "non-record");
        },
    },
];
