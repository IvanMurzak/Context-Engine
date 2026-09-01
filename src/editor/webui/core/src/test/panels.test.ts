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
                dock: { zone: "bottom", minWidth: 120, minHeight: 80 },
                instances: { mode: "limited", max: 4 },
                path: "Scene/Debug",
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
            assertEqual(manifest?.instances.mode, "limited", "instances.mode");
            assertEqual(manifest?.instances.max, 4, "instances.max");
            assertEqual(manifest?.path, "Scene/Debug", "path");
            assertEqual(manifest?.schemaVersion, 2, "schemaVersion (read off state)");
            assertEqual(manifest?.capabilities, ["read", "focus"], "capabilities");
            assertEqual(manifest?.hosted, true, "hosted");
            assertEqual(manifest?.gestures, false, "gestures");
            assertEqual(manifest?.persists, true, "persists");
        },
    },
    {
        // M9 e09b-2 — the BROWSER end of the `gestures:false -> true` flip. Nothing else in this tier
        // asserted `gestures` at all, and it is the one host fact whose loss is INVISIBLE: a manifest
        // that parsed it as `false` would make `PanelHost.#create` build a `UitreePanelRenderer` with
        // gestures off, `HydrationRuntime` would never install its pointer handlers, and the
        // Inspector's L-20 commit gesture would simply never reach the Shell — with every C++ test,
        // every parser test, and the build all still green. So it is pinned in BOTH directions here.
        name: "parsePanelManifest: `gestures` round-trips and fails CLOSED (the commit-gesture seam)",
        run: () => {
            assertEqual(
                parsePanelManifest({ id: "builtin.inspector", gestures: true })?.gestures,
                true,
                "a writing panel's gesture capability must survive the parse",
            );
            // Fail-closed on every not-exactly-true shape: an absent, non-boolean or truthy-but-not-
            // `true` value must not turn into a capability the Shell does not actually host.
            assertEqual(parsePanelManifest({ id: "p" })?.gestures, false, "absent -> false");
            assertEqual(
                parsePanelManifest({ id: "p", gestures: "true" })?.gestures,
                false,
                "a string 'true' is not the boolean",
            );
            assertEqual(parsePanelManifest({ id: "p", gestures: 1 })?.gestures, false, "1 -> false");
            assertEqual(
                parsePanelManifest({ id: "p", gestures: null })?.gestures,
                false,
                "null -> false",
            );
        },
    },
    {
        // Manifest v3 (c2). `instances` is the member that replaced `dock.singleton`, and its parser
        // is the ONE in this file that fails closed to the RESTRICTIVE value rather than the
        // permissive one — because what it decides is how many live copies of a panel may exist, not
        // where a panel first appears. Pinned in both directions: a stated mode survives, and every
        // not-a-known-mode shape lands on `singleton`.
        name: "parsePanelManifest: `instances` fails CLOSED to singleton (the copy-count seam)",
        run: () => {
            assertEqual(
                parsePanelManifest({ id: "p", instances: { mode: "unlimited" } })?.instances.mode,
                "unlimited",
                "a stated mode must survive the parse",
            );
            assertEqual(
                parsePanelManifest({ id: "p", instances: { mode: "limited", max: 3 } })?.instances,
                { mode: "limited", max: 3 },
                "limited carries its ceiling",
            );
            for (const bad of [
                undefined,
                { mode: "many" },
                { mode: "SINGLETON" },
                { mode: 1 },
                "unlimited",
                null,
            ]) {
                assertEqual(
                    parsePanelManifest({ id: "p", instances: bad })?.instances.mode,
                    "singleton",
                    `an unusable instances (${JSON.stringify(bad)}) must fail closed`,
                );
            }
            // A negative ceiling is clamped to "unstated", matching the C++ registry's refusal of one.
            assertEqual(
                parsePanelManifest({ id: "p", instances: { mode: "limited", max: -4 } })?.instances
                    .max,
                0,
                "a negative max is not a ceiling",
            );
            // `path` is display text with an empty default (= top level), read permissively.
            assertEqual(parsePanelManifest({ id: "p" })?.path, "", "absent path -> top level");
            assertEqual(
                parsePanelManifest({ id: "p", path: "Scene/Debug" })?.path,
                "Scene/Debug",
                "path round-trips",
            );
            assertEqual(parsePanelManifest({ id: "p", path: 7 })?.path, "", "a non-string path");
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
