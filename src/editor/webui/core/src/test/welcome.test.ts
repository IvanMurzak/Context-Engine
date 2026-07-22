// T1 unit tests for the M9 e14c welcome surface (welcome.ts). Two properties this tier pins: (1) every
// `welcome.*` result parser is TOTAL and fail-closed (a malformed / partial envelope yields null or the
// documented degrade, never a throw); and (2) `mountWelcome` builds the front door with NO innerHTML,
// renders one row per recent + one chip per template, carries the O1 no-flourish CTA, and wires each
// action back through the bridge. The native OS dialog and the real `context new`/`edit` subprocesses are
// the CI-only / drill halves (editor-shell-welcome-t2); here the bridge is a recording fake.

import { assert, assertEqual, assertNull, type TestCase } from "./harness.js";
import { ShellBridge, type BridgeQueryFunction } from "../bridge.js";
import {
    WELCOME_MODE_PROJECT,
    WELCOME_MODE_WELCOME,
    WELCOME_OPEN_METHOD,
    WELCOME_PICK_FOLDER_METHOD,
    WelcomeClient,
    mountWelcome,
    parseNewProjectResult,
    parseOpenResult,
    parsePickFolderResult,
    parseRecentProject,
    parseWelcomeState,
    parseWelcomeTemplate,
} from "../welcome.js";
import type { WelcomeState } from "../welcome.js";

// A ShellBridge over a fake transport: `responder` produces the `result` for each call (or throws to
// emulate an `unknown_method` refusal). Records every call so a test can assert the wiring.
interface RecordedCall {
    readonly method: string;
    readonly params: Record<string, unknown>;
}

function recordingBridge(
    calls: RecordedCall[],
    responder: (method: string, params: Record<string, unknown>) => unknown,
): ShellBridge {
    const query: BridgeQueryFunction = (q) => {
        const request = JSON.parse(q.request) as {
            id: number;
            method: string;
            params: Record<string, unknown>;
        };
        calls.push({ method: request.method, params: request.params });
        try {
            const result = responder(request.method, request.params);
            q.onSuccess(JSON.stringify({ jsonrpc: "2.0", id: request.id, result }));
        } catch {
            q.onSuccess(
                JSON.stringify({
                    jsonrpc: "2.0",
                    id: request.id,
                    error: { code: -32601, message: "unknown", data: { reason: "bridge.unknown_method" } },
                }),
            );
        }
        return request.id;
    };
    return new ShellBridge(query);
}

const SAMPLE_STATE: WelcomeState = {
    mode: WELCOME_MODE_WELCOME,
    projectName: "",
    recents: [
        { path: "C:/games/alpha", name: "alpha", lastOpenedMs: 200 },
        { path: "C:/games/beta", name: "beta", lastOpenedMs: 100 },
    ],
    templates: [{ name: "default", label: "Empty Project", description: "A minimal runnable project." }],
};

export const welcomeTests: readonly TestCase[] = [
    // --------------------------------------------------------------- the parsers (fail-closed)
    {
        name: "parseWelcomeState: reads mode, name, recents and templates; skips bad entries",
        run: () => {
            const parsed = parseWelcomeState({
                mode: "welcome",
                projectName: "",
                recents: [
                    { path: "C:/a", name: "a", lastOpenedMs: 5 },
                    { name: "no-path" }, // dropped: no path
                    42, // dropped: not a record
                ],
                templates: [{ name: "default", label: "Empty", description: "d" }, {}],
            });
            assert(parsed !== null, "state parsed");
            assertEqual(parsed?.mode, "welcome", "mode");
            assertEqual(parsed?.recents.length, 1, "one usable recent");
            assertEqual(parsed?.recents[0]?.path, "C:/a", "recent path");
            assertEqual(parsed?.templates.length, 1, "one usable template");
        },
    },
    {
        name: "parseWelcomeState: null on a non-record or a missing mode",
        run: () => {
            assertNull(parseWelcomeState(null), "null input");
            assertNull(parseWelcomeState("welcome"), "string input");
            assertNull(parseWelcomeState({ recents: [] }), "no mode");
        },
    },
    {
        name: "parseWelcomeState: absent recents/templates degrade to empty arrays",
        run: () => {
            const parsed = parseWelcomeState({ mode: "project", projectName: "g" });
            assert(parsed !== null, "parsed");
            assertEqual(parsed?.mode, WELCOME_MODE_PROJECT, "project mode");
            assertEqual(parsed?.projectName, "g", "project name");
            assertEqual(parsed?.recents.length, 0, "empty recents");
            assertEqual(parsed?.templates.length, 0, "empty templates");
        },
    },
    {
        name: "parseRecentProject: null without a path; name defaults to the path",
        run: () => {
            assertNull(parseRecentProject({ name: "x" }), "no path");
            assertNull(parseRecentProject(7), "not a record");
            const parsed = parseRecentProject({ path: "C:/p" });
            assertEqual(parsed?.name, "C:/p", "name defaults to path");
            assertEqual(parsed?.lastOpenedMs, 0, "lastOpenedMs defaults to 0");
        },
    },
    {
        name: "parseWelcomeTemplate: null without a name; label defaults to the name",
        run: () => {
            assertNull(parseWelcomeTemplate({ label: "x" }), "no name");
            const parsed = parseWelcomeTemplate({ name: "default" });
            assertEqual(parsed?.label, "default", "label defaults to name");
        },
    },
    {
        name: "parsePickFolderResult: reads picked + path; picked defaults false",
        run: () => {
            assertEqual(parsePickFolderResult({ picked: true, path: "C:/x" })?.picked, true, "picked");
            assertEqual(parsePickFolderResult({})?.picked, false, "picked defaults false");
            assertNull(parsePickFolderResult(null), "null input");
        },
    },
    {
        name: "parseOpenResult / parseNewProjectResult: total over partial envelopes",
        run: () => {
            const open = parseOpenResult({ opened: true, action: "spawn", path: "C:/x" });
            assertEqual(open?.opened, true, "opened");
            assertEqual(open?.action, "spawn", "action");
            const created = parseNewProjectResult({ created: true, runnable: true, opened: true });
            assertEqual(created?.created, true, "created");
            assertEqual(created?.runnable, true, "runnable");
            assertEqual(created?.directory, "", "directory defaults empty");
        },
    },

    // --------------------------------------------------------------- the client (refusal => null)
    {
        name: "WelcomeClient.state: parses a live result and returns null on an unknown_method refusal",
        run: () => {
            const calls: RecordedCall[] = [];
            const present = recordingBridge(calls, () => ({ mode: "welcome", recents: [], templates: [] }));
            void new WelcomeClient(present).state().then((state) => {
                assert(state !== null && state.mode === "welcome", "present surface parsed");
            });

            // A surface that refuses (throws => unknown_method) resolves to null, NOT a rejection — the
            // exact signal boot.ts uses to fall back to the editor path.
            const absent = recordingBridge(calls, () => {
                throw new Error("no welcome surface");
            });
            void new WelcomeClient(absent).state().then((state) => {
                assertNull(state, "absent surface => null");
            });
        },
    },

    // --------------------------------------------------------------- the renderer (DOM, no innerHTML)
    {
        name: "mountWelcome: renders one row per recent + one chip per template, and an O1 no-flourish CTA",
        run: () => {
            const calls: RecordedCall[] = [];
            const bridge = recordingBridge(calls, () => ({}));
            const container = document.createElement("div");
            const mount = mountWelcome(bridge, container, SAMPLE_STATE);

            assertEqual(mount.recentCount, 2, "two recents rendered");
            assertEqual(mount.templateCount, 1, "one template chip rendered");
            assertEqual(
                container.querySelectorAll(".welcome-recent-row").length,
                2,
                "two recent rows in the DOM",
            );
            assertEqual(
                container.querySelectorAll(".welcome-template-chip").length,
                1,
                "one template chip in the DOM",
            );

            const cta = container.querySelector(".welcome-cta");
            assert(cta !== null, "the primary CTA exists");
            // O1: ordinary primary styling, NEVER the aurora/flourish.
            assert(!(cta as HTMLElement).classList.contains("aurora"), "CTA has no aurora class");
            assert(
                (cta as HTMLElement).getAttribute("data-flourish") === null,
                "CTA has no data-flourish",
            );
        },
    },
    {
        name: "mountWelcome: clicking a recent row opens that project through the bridge",
        run: () => {
            const calls: RecordedCall[] = [];
            const bridge = recordingBridge(calls, () => ({ opened: true, action: "spawn", path: "" }));
            const container = document.createElement("div");
            mountWelcome(bridge, container, SAMPLE_STATE);

            const firstRow = container.querySelector(".welcome-recent-row") as HTMLElement;
            firstRow.dispatchEvent(new MouseEvent("click"));

            // The click synchronously invokes bridge.call(welcome.open, { path }) via the WelcomeClient.
            const openCall = calls.find((c) => c.method === WELCOME_OPEN_METHOD);
            assert(openCall !== undefined, "welcome.open was called");
            assertEqual(openCall?.params.path, "C:/games/alpha", "opened the clicked recent's path");
        },
    },
    {
        name: "mountWelcome: the empty-recents state renders a placeholder and still lists templates",
        run: () => {
            const calls: RecordedCall[] = [];
            const bridge = recordingBridge(calls, () => ({ picked: false, path: "" }));
            const container = document.createElement("div");
            const empty: WelcomeState = { ...SAMPLE_STATE, recents: [] };
            const mount = mountWelcome(bridge, container, empty);
            assertEqual(mount.recentCount, 0, "no recents");
            assert(
                container.querySelector(".welcome-recent-empty") !== null,
                "an empty-recents placeholder is shown",
            );

            // "Open project…" invokes the folder picker.
            const openFolder = container.querySelector(".welcome-open-folder") as HTMLElement;
            openFolder.dispatchEvent(new MouseEvent("click"));
            assert(
                calls.some((c) => c.method === WELCOME_PICK_FOLDER_METHOD),
                "Open project… invoked the folder picker",
            );
        },
    },
];
