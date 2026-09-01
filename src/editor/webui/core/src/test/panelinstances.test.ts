// T1 for the PANEL INSTANCE RUNTIME (editor-UX c3, design 04 §3) — D6's imperative half on the
// renderer side: identity is `(panelId, instanceId)`, `open` honours the manifest's declared mode,
// and every copy carries its own DOM scope, its own wire address and its own D6 state.
//
// WHY REAL DOCKVIEW AND NOT A STAND-IN. The whole claim is about IDENTITY inside the docking engine:
// `addPanel` is keyed by id, `toJSON` serialises those ids, and `fromJSON` re-creates a panel BY ID
// before this host has registered anything. A fake that keyed its own map would agree with itself
// about all three and prove none of them — the same reason `layoutrestore.test.ts` insists on the
// pinned engine.
//
// WHAT THIS FILE PROVES THAT THE C++ TIER CANNOT. `test_panel_host.cpp` drives the Shell's instance
// table directly: it proves the MODELS are per copy. It cannot see a Dockview id, a DOM id
// collision, or which `instanceId` actually went on the wire — the three things a renderer holding
// two copies of one kind gets wrong first.
//
// ⚠ THE DUPLICATE-DOM-ID CASE IS THE ONE TO READ FIRST. `HydrationRuntime` rewrites every model node
// id to `<scope>::<nodeId>` so two panels cannot both own `id="root"`. With the scope set to the
// PANEL id — which is what it was before c3 — two copies of ONE kind bring that collision straight
// back: duplicate ids in one document, which is invalid HTML and actively breaks assistive
// technology, since it resolves references by id. Nothing else in the tree can see it.

import { assert, assertEqual, waitFor, type TestCase } from "./harness.js";
import { ShellBridge, type BridgeQuery, type BridgeQueryFunction } from "../bridge.js";
import { detectDockview } from "../dockview.js";
import {
    EDITOR_STATE_GET_METHOD,
    EDITOR_STATE_PUBLISH_METHOD,
    EditorStateClient,
    LayoutPersistence,
} from "../editorstate.js";
import { PanelHost } from "../panelhost.js";
import {
    PANEL_COMMAND_METHOD,
    PANEL_INSTANCE_CLOSE_METHOD,
    PANEL_LIST_METHOD,
    PANEL_RENDER_METHOD,
    PANEL_STATE_GET_METHOD,
    PANEL_STATE_SET_METHOD,
    PanelClient,
    makeInstanceId,
    panelIdOfInstance,
} from "../panels.js";
import { makePanelDispatch } from "../boot.js";

// ------------------------------------------------------------------------------ the roster fixture

const SCHEMA_VERSION = 1;

interface ModeSpec {
    readonly mode: "singleton" | "limited" | "unlimited";
    readonly max?: number;
}

/** One `panel.list` entry in the WIRE shape `PanelHost::list()` emits, for a hosted `uitree` panel. */
function manifest(id: string, instances: ModeSpec): Record<string, unknown> {
    return {
        id,
        kind: "panel",
        title: id,
        icon: "",
        contractVersion: 3,
        dock: { zone: "right", minWidth: 0, minHeight: 0 },
        instances: { mode: instances.mode, max: instances.max ?? 0 },
        path: "",
        content: { type: "uitree", entry: "" },
        state: { schemaVersion: SCHEMA_VERSION },
        capabilities: [],
        commands: [],
        hosted: true,
        gestures: false,
        persists: true,
        revision: 1,
    };
}

interface MockShell {
    readonly bridge: ShellBridge;
    /** Every `(method, params)` pair the host actually sent, in order. */
    readonly calls: { method: string; params: Record<string, unknown> }[];
    /** The Shell's per-INSTANCE D6 store — the fact the "no leak" case reads. */
    readonly states: Map<string, unknown>;
    /** The instance ids the host asked the Shell to RELEASE. */
    readonly released: string[];
    published: Record<string, unknown> | null;
    persisted: Record<string, unknown>;
}

/**
 * A mock Shell whose `panel.*` methods are INSTANCE-AWARE, mirroring `panel_host.cpp`.
 *
 * ⚠ ITS RENDER ECHOES THE ADDRESSED COPY INTO THE HTML, which is what makes "each copy drew its own
 * model" observable at all. A mock that returned one constant body would let a host that rendered the
 * SAME instance twice pass every DOM assertion below — the shape of a test that cannot fail.
 */
function mockShell(panels: readonly Record<string, unknown>[]): MockShell {
    const calls: { method: string; params: Record<string, unknown> }[] = [];
    const states = new Map<string, unknown>();
    const released: string[] = [];
    const store: { published: Record<string, unknown> | null; persisted: Record<string, unknown> } = {
        published: null,
        persisted: { layout: null, panels: {} },
    };
    let served = 0;
    const query: BridgeQueryFunction = (request: BridgeQuery): number => {
        const parsed = JSON.parse(request.request) as {
            id: number;
            method: string;
            params?: Record<string, unknown>;
        };
        const params = parsed.params ?? {};
        calls.push({ method: parsed.method, params });
        served += 1;
        const panelId = typeof params["panelId"] === "string" ? params["panelId"] : "";
        // Mirrors the Shell's "no id ⇒ the kind's default copy" rule, so a host that forgot to
        // address a copy is not silently rescued by the mock.
        const instanceId =
            typeof params["instanceId"] === "string" && params["instanceId"] !== ""
                ? params["instanceId"]
                : makeInstanceId(panelId, 1);
        let result: unknown;
        let known = true;
        if (parsed.method === PANEL_LIST_METHOD) {
            result = { contractMajor: 3, panels };
        } else if (parsed.method === PANEL_RENDER_METHOD) {
            result = {
                panelId,
                instanceId,
                revision: 1,
                html:
                    `<section id="root" role="region" aria-label="${instanceId}">` +
                    `<p id="body">${instanceId}</p></section>`,
                focusOrder: [],
                commands: [],
            };
        } else if (parsed.method === PANEL_STATE_GET_METHOD) {
            result = {
                panelId,
                instanceId,
                state: {
                    schemaVersion: SCHEMA_VERSION,
                    data: states.get(instanceId) ?? instanceId,
                },
            };
        } else if (parsed.method === PANEL_STATE_SET_METHOD) {
            const blob = params["state"];
            const data =
                blob !== null && typeof blob === "object"
                    ? (blob as Record<string, unknown>)["data"]
                    : null;
            states.set(instanceId, data);
            result = { panelId, instanceId, restored: true, code: "", diagnostic: "", revision: 1 };
        } else if (parsed.method === PANEL_INSTANCE_CLOSE_METHOD) {
            released.push(instanceId);
            result = { panelId, instanceId, closed: states.delete(instanceId) || true };
        } else if (parsed.method === EDITOR_STATE_GET_METHOD) {
            result = store.persisted;
        } else if (parsed.method === EDITOR_STATE_PUBLISH_METHOD) {
            store.published = params;
            result = { published: true };
        } else {
            known = false;
        }
        const envelope = known
            ? { jsonrpc: "2.0", id: parsed.id, result }
            : {
                  jsonrpc: "2.0",
                  id: parsed.id,
                  error: {
                      code: -32601,
                      message: "unknown method",
                      data: { reason: "bridge.unknown_method" },
                  },
              };
        request.onSuccess(JSON.stringify(envelope));
        return served;
    };
    return {
        bridge: new ShellBridge(query),
        calls,
        states,
        released,
        get published(): Record<string, unknown> | null {
            return store.published;
        },
        set published(value: Record<string, unknown> | null) {
            store.published = value;
        },
        get persisted(): Record<string, unknown> {
            return store.persisted;
        },
        set persisted(value: Record<string, unknown>) {
            store.persisted = value;
        },
    };
}

interface Mounted {
    readonly host: PanelHost;
    readonly shell: MockShell;
    readonly client: PanelClient;
    readonly container: HTMLElement;
    dispose(): void;
}

/** Start a REAL PanelHost over REAL Dockview against the instance-aware mock Shell. */
async function mountHost(panels: readonly Record<string, unknown>[]): Promise<Mounted> {
    const dockview = detectDockview();
    assert(
        dockview !== undefined,
        "the pinned dockview-core UMD global is loaded — harness.html must load " +
            "dockview-core.min.js before the test bundle, or this whole file passes vacuously",
    );
    const container = window.document.createElement("div");
    container.style.width = "800px";
    container.style.height = "600px";
    window.document.body.appendChild(container);
    const shell = mockShell(panels);
    const client = new PanelClient(shell.bridge);
    const host = new PanelHost({
        container,
        client,
        dockview: dockview as NonNullable<typeof dockview>,
    });
    const report = await host.start();
    assert(report.started, "the docking root came up");
    return {
        host,
        shell,
        client,
        container,
        dispose: (): void => {
            host.dispose();
            container.remove();
        },
    };
}

/** The panel ids the ENGINE itself serialises — the same artifact the Shell persists. */
function arrangedIds(host: PanelHost): string {
    const layout = host.captureLayout();
    const panels = (layout as { panels?: Record<string, unknown> } | null)?.panels;
    return panels === undefined ? "" : Object.keys(panels).sort().join(",");
}

/** Every `id` attribute in the document, so a DUPLICATE is visible as a count rather than inferred. */
function documentIds(container: HTMLElement): string[] {
    return [...container.querySelectorAll("[id]")].map((el) => el.getAttribute("id") ?? "");
}

// ------------------------------------------------------------------------------------- the cases

export const panelInstanceTests: readonly TestCase[] = [
    {
        name: "instance ids: compose and decompose, and a bare id reads as its own kind",
        run: () => {
            assertEqual(makeInstanceId("builtin.problems", 1), "builtin.problems#1", "composed");
            assertEqual(panelIdOfInstance("builtin.problems#2"), "builtin.problems", "decomposed");
            // A persisted arrangement written before instances existed carries a BARE panel id, and
            // the honest restore is onto the kind itself rather than a lookup that can only fail.
            assertEqual(panelIdOfInstance("builtin.problems"), "builtin.problems", "bare id");
            // LAST separator: a panel id may contain one, and splitting on the first would resolve to
            // a kind that does not exist.
            assertEqual(panelIdOfInstance("odd#name#7"), "odd#name", "splits on the last separator");
        },
    },
    {
        name: "singleton: a second open FOCUSES the live copy and says so (it is not a failure)",
        run: async () => {
            const mounted = await mountHost([manifest("p.single", { mode: "singleton" })]);
            try {
                assertEqual(mounted.host.mounted.length, 1, "start() opened exactly one copy");
                const first = mounted.host.mounted[0] ?? "";
                assertEqual(first, "p.single#1", "the first copy takes the first ordinal");

                const again = mounted.host.openInstance("p.single");
                // ⚠ THE WHOLE POINT OF c3's OPEN RESULT. Before it, `open` answered `false` here —
                // the SAME answer it gives for "the docking root is down" — so a caller could not
                // tell "already open" from a failure and could never focus the live copy instead.
                assertEqual(again.outcome, "focused", "a second open focuses");
                assertEqual(again.instanceId, first, "and names the copy that answered it");
                assertEqual(again.diagnostic, "", "a focus is not a refusal, so it carries no reason");
                assertEqual(mounted.host.mounted.length, 1, "and no second copy was minted");
                assertEqual(arrangedIds(mounted.host), first, "the engine holds one panel");
                // ⚠ THE FOCUS ITSELF IS A `?.` CALL through an OPTIONAL declared member
                // (`DockviewPanelHandle.api`), so a wrong shape on the pinned engine would make it a
                // silent no-op that every assertion above still passes. This is the discriminator:
                // the REAL 7.0.2 handle really does expose a callable `setActive`, asserted against
                // the artifact rather than reasoned from the declaration.
                const handle = mounted.host.api?.getPanel(first);
                assert(handle !== undefined, "the engine hands back a handle for the live copy");
                assertEqual(
                    typeof handle?.api?.setActive,
                    "function",
                    "and it carries a callable setActive — without it `#focus` raises nothing and " +
                        "'a second open focuses' would be true only of the reported outcome",
                );
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "limited: opens up to max, refuses the next NAMING the limit, and a close frees a slot",
        run: async () => {
            const mounted = await mountHost([manifest("p.few", { mode: "limited", max: 2 })]);
            try {
                assertEqual(mounted.host.mounted.length, 1, "start() opened the first copy");
                const second = mounted.host.openInstance("p.few");
                assertEqual(second.outcome, "opened", "the second copy is within the ceiling");
                assertEqual(mounted.host.instancesOf("p.few").length, 2, "two live copies");

                const third = mounted.host.openInstance("p.few");
                assertEqual(third.outcome, "refused", "the third exceeds max 2");
                assertEqual(third.instanceId, "", "a refusal names no copy");
                // NAMES the limit (design 04 §3). A bare "refused" sends the human to the manifest to
                // find out what they hit — which is the whole reason the diagnostic exists.
                assert(
                    third.diagnostic.includes("2"),
                    `the refusal names the limit, got: ${third.diagnostic}`,
                );
                assertEqual(mounted.host.instancesOf("p.few").length, 2, "and nothing was minted");

                // THE HALF A SESSION-COUNTED CEILING WOULD FAIL: closing frees a slot, so `max` bounds
                // the LIVE copies rather than how many were ever opened.
                assert(mounted.host.close(second.instanceId), "the second copy closed");
                const reopened = mounted.host.openInstance("p.few");
                assertEqual(reopened.outcome, "opened", "a freed slot admits another copy");
                assertEqual(
                    reopened.instanceId,
                    "p.few#3",
                    "with a FRESH ordinal — reusing #2 would hand a stale holder the new copy",
                );
                // And the Shell was told to release the closed copy's model, which is what keeps ITS
                // ceiling counting live copies too.
                assert(
                    mounted.shell.released.includes(second.instanceId),
                    `close released the model on the wire, got ${JSON.stringify(mounted.shell.released)}`,
                );
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "unlimited: distinct copies, distinct wire addresses, and NO duplicate DOM ids",
        run: async () => {
            const mounted = await mountHost([manifest("p.many", { mode: "unlimited" })]);
            try {
                const second = mounted.host.openInstance("p.many");
                assertEqual(second.outcome, "opened", "unlimited mints on every open");
                assertEqual(
                    [...mounted.host.instancesOf("p.many")].join(","),
                    "p.many#1,p.many#2",
                    "two distinct copies of one kind",
                );
                assertEqual(
                    arrangedIds(mounted.host),
                    "p.many#1,p.many#2",
                    "and the ENGINE holds two panels — a shared id would have replaced the first",
                );

                // Both copies rendered, each addressing ITS OWN instance on the wire.
                await waitFor(
                    "both copies to render",
                    () =>
                        mounted.shell.calls.filter(
                            (c) => c.method === PANEL_RENDER_METHOD,
                        ).length >= 2,
                    5_000,
                    () => JSON.stringify(mounted.shell.calls.map((c) => c.method)),
                );
                const rendered = new Set(
                    mounted.shell.calls
                        .filter((c) => c.method === PANEL_RENDER_METHOD)
                        .map((c) => String(c.params["instanceId"])),
                );
                assert(
                    rendered.has("p.many#1") && rendered.has("p.many#2"),
                    `each copy pulled its own render, got ${JSON.stringify([...rendered])}`,
                );

                // ⚠ THE a11y-CRITICAL ASSERTION. The hydration runtime scopes model node ids by its
                // instance; with the old panel-id scope both copies would mint `p.many::root` and the
                // document would carry duplicate ids, which no other tier can see.
                await waitFor("both copies to mount their trees", () => {
                    return mounted.container.querySelectorAll("[data-node-id]").length >= 2;
                });
                const ids = documentIds(mounted.container);
                assertEqual(
                    ids.length,
                    new Set(ids).size,
                    `every id in the document is unique, got ${JSON.stringify(ids)}`,
                );
                assert(
                    ids.includes("p.many#1::root") && ids.includes("p.many#2::root"),
                    `each copy scoped its own root, got ${JSON.stringify(ids)}`,
                );
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "D6 state is PER INSTANCE: a write to one copy does not leak, and the publish is keyed by copy",
        run: async () => {
            const mounted = await mountHost([manifest("p.many", { mode: "unlimited" })]);
            try {
                const second = mounted.host.openInstance("p.many");
                assertEqual(second.instanceId, "p.many#2", "a second copy exists to leak INTO");

                // Write to ONE copy through the client the persistence layer uses.
                const wrote = await mounted.client.setState(
                    "p.many",
                    { schemaVersion: SCHEMA_VERSION, data: "only-1" },
                    "p.many#1",
                );
                assert(wrote !== null && wrote.restored, "the write landed");

                const one = await mounted.client.getState("p.many", "p.many#1");
                const two = await mounted.client.getState("p.many", "p.many#2");
                assertEqual(
                    (one as { data?: unknown }).data,
                    "only-1",
                    "the addressed copy holds what was written",
                );
                // THE LEAK ASSERTION. Equality here is the failure — and the sibling above is what
                // makes it non-vacuous: a `null` from a write that never landed would satisfy
                // "they differ" while proving nothing.
                assert(
                    (two as { data?: unknown }).data !== "only-1",
                    "and the sibling copy did NOT receive it",
                );

                // The published document keys BY COPY, which is what makes a reload restore each
                // copy's own blob instead of the last one gathered.
                const persistence = new LayoutPersistence({
                    panelHost: mounted.host,
                    panelClient: mounted.client,
                    stateClient: new EditorStateClient(mounted.shell.bridge),
                    debounceMs: 1,
                });
                persistence.attach();
                mounted.host.openInstance("p.many"); // a layout change → one debounced publish
                await waitFor(
                    "the debounced publish",
                    () => mounted.shell.published !== null,
                    5_000,
                );
                const published = mounted.shell.published ?? {};
                const keys = Object.keys(
                    (published["panels"] as Record<string, unknown> | undefined) ?? {},
                ).sort();
                assertEqual(
                    keys.join(","),
                    "p.many#1,p.many#2,p.many#3",
                    "one entry per LIVE COPY — before c3 the three collapsed onto one key",
                );
                persistence.dispose();
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "layout restore round-trips INSTANCE ids, including after a close and reopen",
        run: async () => {
            const donor = await mountHost([manifest("p.many", { mode: "unlimited" })]);
            let layout: unknown;
            try {
                donor.host.openInstance("p.many");
                assertEqual(arrangedIds(donor.host), "p.many#1,p.many#2", "the donor holds two");
                layout = donor.host.captureLayout();
                assert(layout !== null, "the donor produced an arrangement");
            } finally {
                donor.dispose();
            }

            const target = await mountHost([manifest("p.many", { mode: "unlimited" })]);
            try {
                // CLOSE AND REOPEN FIRST, so the target is NOT in the shape the arrangement names —
                // its live copy is `#2` (a reopen mints a fresh ordinal), and a restore that quietly
                // kept it would look identical to a correct one if the target still held `#1`.
                const first = target.host.mounted[0] ?? "";
                assert(target.host.close(first), "the target closed its default copy");
                const reopened = target.host.openInstance("p.many");
                assertEqual(reopened.instanceId, "p.many#2", "the reopen minted a fresh ordinal");

                assertEqual(target.host.restoreLayout(layout), true, "the arrangement restores");
                assertEqual(
                    arrangedIds(target.host),
                    "p.many#1,p.many#2",
                    "the donor's INSTANCE ids came back — a kind-keyed restore would hold one panel",
                );
                assertEqual(
                    [...target.host.mounted].sort().join(","),
                    "p.many#1,p.many#2",
                    "and the host tracks exactly the restored copies, with no stale entry left over",
                );
                // The restored ids also moved the mint counter, so the next open cannot collide with
                // a copy the arrangement just brought back.
                assertEqual(
                    target.host.openInstance("p.many").instanceId,
                    "p.many#3",
                    "the next mint is past every restored ordinal",
                );
            } finally {
                target.dispose();
            }
        },
    },
    {
        name: "tear-out/rehome moves the INTENDED copy: its state travels, its siblings' does not",
        run: async () => {
            // The e10b path, at the level this tier can drive it: the SOURCE reads the state of the
            // copy being moved (not of the kind), and the TARGET mints its own copy and restores onto
            // it. Instance ordinals are per window, so what crosses is the KIND plus that copy's blob.
            const source = await mountHost([manifest("p.many", { mode: "unlimited" })]);
            let carried: unknown;
            try {
                const second = source.host.openInstance("p.many");
                await source.client.setState(
                    "p.many",
                    { schemaVersion: SCHEMA_VERSION, data: "moved-me" },
                    second.instanceId,
                );
                await source.client.setState(
                    "p.many",
                    { schemaVersion: SCHEMA_VERSION, data: "stay-home" },
                    "p.many#1",
                );

                // The tear-out command's own two steps, in order: resolve the copy's KIND, then read
                // THAT COPY's state.
                const active = source.host.mounted[source.host.mounted.length - 1] ?? "";
                assertEqual(active, second.instanceId, "the active panel is the copy being moved");
                assertEqual(source.host.panelIdOf(active), "p.many", "and its kind is resolvable");
                carried = await source.client.getState("p.many", active);
                assertEqual(
                    (carried as { data?: unknown }).data,
                    "moved-me",
                    "the MOVED copy's blob travels — a kind-addressed read would have taken #1's",
                );
                assert(source.host.close(active), "and the copy leaves this window");
            } finally {
                source.dispose();
            }

            const target = await mountHost([manifest("p.many", { mode: "unlimited" })]);
            try {
                const opened = target.host.openInstance("p.many");
                assertEqual(opened.outcome, "opened", "the target mints a copy for the arrival");
                const restored = await target.client.setState("p.many", carried, opened.instanceId);
                assert(restored !== null && restored.restored, "and restores onto THAT copy");
                assertEqual(
                    target.shell.states.get(opened.instanceId),
                    "moved-me",
                    "the moved blob landed on the copy the rehome opened",
                );
            } finally {
                target.dispose();
            }
        },
    },
    {
        name: "layout restore RELEASES the copies it drops, not just its own tables",
        run: async () => {
            // A restore forgets the copies the arrangement does not name. Dropping them from this
            // host's tables is only HALF the job: the Shell holds a model per copy and counts LIVE
            // copies against the kind's ceiling, so a drop without the release leaves a singleton
            // permanently at its limit — the Shell then refuses the copy the restore just brought
            // back (`panel.instance_limit`) and the panel renders EMPTY. Silent, and it survives
            // until the next restore.
            //
            // ⚠ ONLY A COPY THE RESTORE ITSELF FORGETS CAN PROVE IT. `close()` releases through the
            // same `#drop`, so a version of this test that closed the doomed copy would stay green
            // with the reconcile's release deleted — which is exactly the shape the bug shipped in.
            const donor = await mountHost([manifest("p.one", { mode: "singleton" })]);
            let layout: unknown;
            try {
                assertEqual(arrangedIds(donor.host), "p.one#1", "the donor holds the default copy");
                layout = donor.host.captureLayout();
                assert(layout !== null, "the donor produced an arrangement");
            } finally {
                donor.dispose();
            }

            const target = await mountHost([manifest("p.one", { mode: "singleton" })]);
            try {
                // Put the target in a shape the arrangement does NOT name: a reopen mints a fresh
                // ordinal, so the live copy is `#2` while the donor's arrangement names `#1`. This
                // is the ordinary case after a close-and-reopen, and the pre-c3 code never saw it
                // because the ids were kind ids and a restore replayed the same ones.
                const first = target.host.mounted[0] ?? "";
                assert(target.host.close(first), "the target closed its default copy");
                const reopened = target.host.openInstance("p.one");
                assertEqual(reopened.instanceId, "p.one#2", "and reopened it as a fresh ordinal");
                assertEqual(
                    target.shell.released.join(","),
                    "p.one#1",
                    "only the explicitly CLOSED copy has been released so far — the restore's own " +
                        "release has to show up on top of this one",
                );

                assertEqual(target.host.restoreLayout(layout), true, "the arrangement restores");
                assertEqual(
                    arrangedIds(target.host),
                    "p.one#1",
                    "the restored arrangement names the donor's copy",
                );
                assert(
                    target.shell.released.includes("p.one#2"),
                    "the copy the restore DROPPED was released on the wire; without it the Shell " +
                        "still counts it against the singleton ceiling and the restored copy is " +
                        `refused, got ${JSON.stringify(target.shell.released)}`,
                );
                assertEqual(
                    [...target.host.mounted].join(","),
                    "p.one#1",
                    "and the dropped copy is gone from this host's tables too",
                );
            } finally {
                target.dispose();
            }
        },
    },
    {
        name: "makePanelDispatch resolves the KIND to a live copy and addresses THAT copy",
        run: async () => {
            // `projectPanelCommands` builds every manifest command's handler as
            // `dispatch(panel.id, command.id)` — a KIND, because that is what a roster names. Since
            // c3 the host's tables are keyed by INSTANCE, so a bare kind resolves to nothing: the
            // port lookup can only ever miss, and the `panel.command` fallback would address the
            // Shell's default copy while the port route (had it hit) addressed another. Both routes
            // must land on the SAME copy, so the kind is resolved once, up front.
            const mounted = await mountHost([manifest("p.many", { mode: "unlimited" })]);
            try {
                const second = mounted.host.openInstance("p.many");
                assertEqual(second.instanceId, "p.many#2", "the kind has two live copies");

                const dispatch = makePanelDispatch(mounted.client, mounted.host);
                const before = mounted.shell.calls.length;
                await dispatch("p.many", "cmd.go");

                const sent = mounted.shell.calls
                    .slice(before)
                    .filter((entry) => entry.method === PANEL_COMMAND_METHOD);
                assertEqual(sent.length, 1, "exactly one command reached the wire");
                assertEqual(
                    sent[0]?.params["instanceId"],
                    "p.many#1",
                    "addressed to the RESOLVED copy — a kind-addressed dispatch sends no " +
                        "instanceId at all and leans on the Shell's default-copy rule, which is " +
                        "the same miss that sends every port panel down this fallback",
                );
            } finally {
                mounted.dispose();
            }
        },
    },
];
