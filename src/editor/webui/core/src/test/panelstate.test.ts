// T1 for e13d's HOST WIRING (M9 e13d, design 04 §5 / 06 §1): the two halves that make a package
// panel actually receive theme tokens and actually keep its state across a reload.
//
// WHY BOTH HALVES SHARE ONE FILE. They are one task and one seam: `IframePanelRenderer` is the object
// that registers a frame with the theme channel AND owns the state store, and `PanelHost` is the
// object that exposes the store to persistence. Splitting them would put two halves of one renderer's
// lifecycle in two files that would then have to build the same live Dockview host twice.
//
// WHAT THIS TIER PROVES THAT THE OTHERS CANNOT.
//
//   * `panelverbs.test.ts` drives the verb HANDLERS over an injected store: it proves what they
//     decide, and nothing at all about whether a real panel's store is reachable from persistence.
//   * `panelport.test.ts` drives those handlers over a REAL port: it proves the decisions survive the
//     frame boundary, and nothing about the theme channel or the editor-state document.
//   * THIS file mounts a REAL PanelHost over REAL Dockview with a REAL iframe panel, and asserts the
//     WIRING: that the frame is registered with the theme channel before its `src` is set, that it is
//     unregistered on dispose, and that a blob set through the store survives a publish -> restore
//     cycle into a SECOND host — which is what "a reload preserves panel state" means.
//
// ⚠ THE RELOAD IS MODELLED BY A SECOND HOST, NOT BY A SECOND BROWSER, and the substitution is exact
// for the property under test: the editor-state document is the only thing that crosses a reload (the
// Shell is its single writer, editorstate.ts § the header), so publishing from host A and restoring
// into a freshly-started host B exercises every line a real restart does on this side of the bridge.
// What it does NOT cover is the Shell's own store surviving process exit — that is
// `editor-cef-smoke-shell-restore`'s, and it is not claimed here.
//
// ⚠ NON-VACUITY PROVEN BY PLANTING, each reverted byte-exact — recorded inline as `⚠ PLANT (x)`.

import { assert, assertEqual, type TestCase } from "./harness.js";
import { ShellBridge, type BridgeQuery, type BridgeQueryFunction } from "../bridge.js";
import { detectDockview } from "../dockview.js";
import {
    EDITOR_STATE_GET_METHOD,
    EDITOR_STATE_PUBLISH_METHOD,
    EditorStateClient,
    LayoutPersistence,
} from "../editorstate.js";
import { PanelHost } from "../panelhost.js";
import type { PanelThemeChannel, PanelVerbBinding } from "../panelhost.js";
import { PANEL_LIST_METHOD, PanelClient } from "../panels.js";
import type { PanelVerbHandler } from "../panelport.js";
import type { PanelStateStore, PanelVerbTable } from "../panelverbs.js";
import { IframeThemeChannel, type IframeMessageTarget } from "../theme.js";

// ------------------------------------------------------------------------------ the roster fixture

const PANEL_ID = "pkg.hello";
const SCHEMA_VERSION = 3;

/** One `panel.list` entry in the WIRE shape `PanelHost::list()` emits, for an `iframe` panel. */
function iframeManifest(id = PANEL_ID): Record<string, unknown> {
    return {
        id,
        kind: "panel",
        title: "Hello",
        icon: "",
        contractVersion: 2,
        dock: { zone: "right", singleton: true, minWidth: 0, minHeight: 0 },
        content: { type: "iframe", entry: `context-ext://${id}/index.html` },
        state: { schemaVersion: SCHEMA_VERSION },
        capabilities: [],
        commands: [],
        // An iframe panel has NO Shell provider by construction — its bytes are served over
        // `context-ext://`, never over `panel.render`.
        hosted: false,
        gestures: false,
        persists: false,
        revision: 1,
    };
}

/** A `uitree` panel WITH a provider — the contrast subject for the "not a port panel" arms. */
function uitreeManifest(id: string): Record<string, unknown> {
    return { ...iframeManifest(id), content: { type: "uitree", entry: "" }, hosted: true };
}

interface MockShell {
    readonly bridge: ShellBridge;
    /** Every method the host / persistence actually called, in order. */
    readonly methods: string[];
    /** The last `editor.state.publish` params — what a reload would read back. */
    published: Record<string, unknown> | null;
    /** What `editor.state.get` answers with. A "fresh project" until a test seeds it. */
    persisted: Record<string, unknown>;
}

/**
 * A mock Shell that serves the roster AND the editor-state document.
 *
 * It stores what it is published, which is what lets one host's publish become the next host's
 * restore — the substitution the file header describes. Every other method is refused exactly as the
 * real router's default does, so a code path reaching for the C++ `panel.state.*` route is VISIBLE as
 * a refusal rather than silently absorbed.
 */
function mockShell(panels: readonly Record<string, unknown>[]): MockShell {
    const methods: string[] = [];
    let served = 0;
    // The two mutable cells the query closes over. Held apart from the returned object so `bridge`
    // can stay `readonly` there — the query needs them before the bridge exists, and a
    // `null as ShellBridge` placeholder would be a lie the type system then stops checking.
    const store: { published: Record<string, unknown> | null; persisted: Record<string, unknown> } = {
        published: null,
        persisted: { layout: null, panels: {} },
    };
    const query: BridgeQueryFunction = (request: BridgeQuery): number => {
        const parsed = JSON.parse(request.request) as {
            id: number;
            method: string;
            params?: Record<string, unknown>;
        };
        methods.push(parsed.method);
        served += 1;
        let result: unknown;
        let known = true;
        if (parsed.method === PANEL_LIST_METHOD) {
            result = { contractMajor: 2, panels };
        } else if (parsed.method === EDITOR_STATE_GET_METHOD) {
            result = store.persisted;
        } else if (parsed.method === EDITOR_STATE_PUBLISH_METHOD) {
            store.published = parsed.params ?? {};
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
        methods,
        get published(): Record<string, unknown> | null {
            return store.published;
        },
        get persisted(): Record<string, unknown> {
            return store.persisted;
        },
        set persisted(value: Record<string, unknown>) {
            store.persisted = value;
        },
    };
}

// ------------------------------------------------------------------------------ the mounted host

interface Mounted {
    readonly host: PanelHost;
    readonly shell: MockShell;
    readonly container: HTMLElement;
    /** The state store the verb factory was handed for each panel, by panel id. */
    readonly stores: Map<string, PanelStateStore>;
    dispose(): void;
}

interface MountOptions {
    readonly themeChannel?: PanelThemeChannel;
    readonly panels?: readonly Record<string, unknown>[];
    readonly shell?: MockShell;
}

/**
 * Start a REAL PanelHost over REAL Dockview.
 *
 * The verb factory returns an EMPTY table and captures the store it was handed. That is deliberate:
 * this file is about the wiring, and driving the real verbs here would re-test `panelverbs.test.ts`
 * while adding a way for a verb bug to red a wiring case (and vice versa). What matters is that the
 * store the factory receives is the SAME object `portState` / `seedPortState` read — which is exactly
 * what these cases assert by writing through one and reading through the other.
 */
async function mountHost(options: MountOptions = {}): Promise<Mounted> {
    const dockview = detectDockview();
    assert(
        dockview !== undefined,
        "the pinned dockview-core UMD global is loaded — harness.html must load " +
            "dockview-core.min.js before the test bundle, or this whole file passes vacuously",
    );
    const dv = dockview as NonNullable<typeof dockview>;
    const container = window.document.createElement("div");
    container.style.width = "800px";
    container.style.height = "600px";
    window.document.body.appendChild(container);
    const shell = options.shell ?? mockShell(options.panels ?? [iframeManifest()]);
    const stores = new Map<string, PanelStateStore>();
    const host = new PanelHost({
        container,
        client: new PanelClient(shell.bridge),
        dockview: dv,
        panelVerbs: (binding: PanelVerbBinding): PanelVerbTable => {
            stores.set(binding.panelId, binding.state);
            return { verbs: new Map<string, PanelVerbHandler>(), dispose: (): void => {} };
        },
        ...(options.themeChannel === undefined ? {} : { themeChannel: options.themeChannel }),
    });
    await host.start();
    return {
        host,
        shell,
        container,
        stores,
        dispose: (): void => {
            host.dispose();
            container.remove();
        },
    };
}

/** A theme channel that RECORDS registrations — the witness for the wiring half. */
class RecordingChannel implements PanelThemeChannel {
    readonly registered: IframeMessageTarget[] = [];
    readonly unregistered: IframeMessageTarget[] = [];

    register(target: IframeMessageTarget): void {
        this.registered.push(target);
    }

    unregister(target: IframeMessageTarget): void {
        this.unregistered.push(target);
    }
}

/**
 * Poll until `predicate` holds, or fail the case naming what was being waited for.
 *
 * The publish path is `void this.#publish()` off a DOM event — genuinely asynchronous, with no
 * promise to await — so a poll is the honest shape rather than a fixed sleep that would either flake
 * or waste time. The budget is generous because this tier shares one document with ~300 earlier cases.
 */
async function waitFor(what: string, predicate: () => boolean, timeoutMs = 4000): Promise<void> {
    const deadline = Date.now() + timeoutMs;
    while (!predicate()) {
        if (Date.now() > deadline) {
            throw new Error(`timed out after ${String(timeoutMs)}ms waiting for ${what}`);
        }
        await new Promise((resolve) => setTimeout(resolve, 10));
    }
}

/** The one `editor.ui.theme-changed` envelope shape the channel broadcasts. */
function themeEvent(themeId: string, panelColour: string): Parameters<IframeThemeChannel["broadcast"]>[0] {
    return {
        seq: 1,
        topic: "editor.ui.theme-changed",
        origin: "local",
        payload: {
            themeId,
            name: themeId,
            appearance: "dark",
            highContrast: false,
            reducedMotion: false,
            variables: { "--ctx-colors-panel": panelColour },
        },
    };
}

// ------------------------------------------------------------------------------------------ cases

export const panelStateTests: readonly TestCase[] = [
    // -------------------------------------------------------------- theme delivery (the wiring)
    {
        // THE WHOLE POINT OF e13d's THEME HALF. `IframeThemeChannel` shipped fully written at e06b and
        // was wired to NOTHING: its only non-test caller was `ThemeEngine`'s own constructor. So the
        // mechanism worked in isolation and no panel had ever received a token.
        //
        // ⚠ PLANT (a): drop the `this.#themeChannel?.register(target)` line in
        //   `IframePanelRenderer.refresh` — RED here, and GREEN across every pre-existing theme case,
        //   which is precisely how the gap survived three tasks.
        name: "panelstate: a mounted iframe panel's frame is REGISTERED with the theme channel",
        run: async (): Promise<void> => {
            const channel = new RecordingChannel();
            const mounted = await mountHost({ themeChannel: channel });
            try {
                assertEqual(
                    channel.registered.length,
                    1,
                    "the one mounted package panel registered exactly one push target",
                );
                assertEqual(
                    channel.unregistered.length,
                    0,
                    "…and nothing was unregistered while it is alive",
                );
            } finally {
                mounted.dispose();
            }
            assertEqual(
                channel.unregistered.length,
                1,
                "closing the panel unregisters it — the channel outlives the renderer, so a target " +
                    "left behind is a closure holding a dead frame that every future switch posts into",
            );
            assertEqual(
                channel.unregistered[0],
                channel.registered[0],
                "…and it is the SAME target, not a look-alike the channel would keep forever",
            );
        },
    },
    {
        // The target must resolve `contentWindow` AT POST TIME. A frame that is not yet in a document
        // has a `null` contentWindow, and the renderer registers before Dockview has mounted its
        // element — so a target that captured the value would post to `null` forever and the panel
        // would never be themed, with nothing anywhere reporting it.
        //
        // ⚠ PLANT (b): capture `frame.contentWindow` into a const at registration and post to that —
        //   RED here (the post never reaches the frame's real window).
        name: "panelstate: the push target resolves contentWindow LAZILY, so a real frame is posted to",
        run: async (): Promise<void> => {
            const channel = new IframeThemeChannel();
            const mounted = await mountHost({ themeChannel: channel });
            try {
                assertEqual(channel.size, 1, "the frame registered with the REAL channel");
                const frame = mounted.container.querySelector<HTMLIFrameElement>(
                    `iframe[data-panel-id="${PANEL_ID}"]`,
                );
                assert(frame !== null, "the panel really rendered a frame");
                assert(
                    frame?.contentWindow !== null && frame?.contentWindow !== undefined,
                    "and it is in the document, so it HAS a contentWindow — the value a capturing " +
                        "target would have missed by registering before Dockview mounted the element",
                );

                // A `context-ext://` document cannot load in this tier (no scheme handler), so what is
                // asserted is that the broadcast REACHES a live window without throwing — the
                // property the lazy target exists for. Whether the package's own listener applies it
                // is `editor-cef-smoke-shell-iframe`'s, and is not claimed here.
                channel.broadcast(themeEvent("builtin.dark", "#0a0a0a"));
                assertEqual(
                    channel.last?.payload.themeId,
                    "builtin.dark",
                    "the broadcast completed and is retained for the pull half",
                );
            } finally {
                mounted.dispose();
            }
            assertEqual(channel.size, 0, "and dispose really removed it from the real channel");
        },
    },

    // ------------------------------------------------------- state blob: the host-side accessors
    {
        // ⚠ PLANT (c): return `undefined` unconditionally from `PanelHost.portState` — RED. This is
        //   the accessor persistence publishes THROUGH, so without it every package panel's blob is
        //   silently dropped at every publish and the reload preserves nothing.
        name: "panelstate: PanelHost.portState wraps the panel's blob in the D6 {schemaVersion,data} shape",
        run: async (): Promise<void> => {
            const mounted = await mountHost();
            try {
                assertEqual(
                    mounted.host.portState(PANEL_ID),
                    undefined,
                    "a panel that stored nothing publishes nothing — the same signal a modelless " +
                        "C++ panel gives, so persistence needs no second rule",
                );
                const store = mounted.stores.get(PANEL_ID);
                assert(store !== undefined, "the verb factory was handed this panel's store");
                store?.write({ scroll: 7 });
                assertEqual(
                    mounted.host.portState(PANEL_ID),
                    { schemaVersion: SCHEMA_VERSION, data: { scroll: 7 } },
                    "the blob is published in the SAME wrapper the C++ models use, so the Shell " +
                        "never has to know which kind of panel wrote an entry",
                );
                assertEqual(
                    mounted.host.portState("nope.missing"),
                    undefined,
                    "an unknown id answers undefined rather than throwing",
                );
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        // The EXCLUSIVITY that lets `LayoutPersistence` use `undefined` as its routing signal. If a
        // `uitree` panel answered anything but `undefined`, its blob would be taken from an empty port
        // store and its REAL C++ state would never be published.
        //
        // ⚠ PLANT (d): drop the `instanceof IframePanelRenderer` guard from both accessors — RED (the
        //   uitree panel starts answering, and its C++ route is skipped).
        name: "panelstate: a uitree panel is NOT a port panel, so both accessors answer undefined",
        run: async (): Promise<void> => {
            const mounted = await mountHost({
                panels: [iframeManifest(), uitreeManifest("builtin.problems")],
            });
            try {
                assertEqual(
                    mounted.host.portState("builtin.problems"),
                    undefined,
                    "a C++-modeled panel has no port store to publish from",
                );
                assertEqual(
                    mounted.host.seedPortState("builtin.problems", {
                        schemaVersion: SCHEMA_VERSION,
                        data: { a: 1 },
                    }),
                    undefined,
                    "…and none to seed, so persistence falls through to panel.state.set for it",
                );
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        // The D6 degrade contract, mirrored from `restore_panel_state` clause for clause. A package
        // that changed its own state shape across versions must NOT be handed yesterday's data.
        //
        // ⚠ PLANT (e): drop the `persisted.schemaVersion !== schemaVersion` comparison — RED.
        // ⚠ PLANT (f): skip the `sanitizePanelState` re-check in `seedState` — RED on the oversize row.
        name: "panelstate: seedPortState DEGRADES a stale, malformed or over-budget persisted blob",
        run: async (): Promise<void> => {
            const mounted = await mountHost();
            try {
                assertEqual(
                    mounted.host.seedPortState(PANEL_ID, {
                        schemaVersion: SCHEMA_VERSION + 1,
                        data: { a: 1 },
                    }),
                    false,
                    "a blob written by a DIFFERENT state schema degrades rather than being applied",
                );
                assertEqual(
                    mounted.host.seedPortState(PANEL_ID, { data: { a: 1 } }),
                    false,
                    "so does one with no schemaVersion at all (a hand-edited editor-state file)",
                );
                assertEqual(
                    mounted.host.seedPortState(PANEL_ID, "not an envelope"),
                    false,
                    "…and one that is not an object",
                );
                assertEqual(
                    mounted.host.seedPortState(PANEL_ID, {
                        schemaVersion: SCHEMA_VERSION,
                        data: { blob: "x".repeat(70 * 1024) },
                    }),
                    false,
                    "an OVER-BUDGET blob is refused on the way IN from disk too — otherwise it would " +
                        "be re-published at the next layout change, an amplification loop seeded from " +
                        "a file a human can edit",
                );
                assertEqual(
                    mounted.host.portState(PANEL_ID),
                    undefined,
                    "and not one of those degrades left anything in the store",
                );

                assertEqual(
                    mounted.host.seedPortState(PANEL_ID, {
                        schemaVersion: SCHEMA_VERSION,
                        data: { scroll: 9 },
                    }),
                    true,
                    "a matching, in-budget blob IS applied",
                );
                assertEqual(
                    mounted.stores.get(PANEL_ID)?.read(),
                    { scroll: 9 },
                    "…and the panel reads it back through its own store — which is what " +
                        "bridge.state.get answers with",
                );
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        // THE ORDER-INDEPENDENCE GUARD. A seed that landed after the panel had already written would
        // overwrite what the user is looking at with yesterday's blob, silently and totally. In a real
        // boot the seed wins by a wide margin (the panel's first write needs a full document load plus
        // a port handshake), but "by a wide margin" is not an invariant.
        //
        // ⚠ PLANT (g): remove the `#stateFromPanel` early-return from `seedState` — RED.
        name: "panelstate: a persisted seed never clobbers state the PANEL has already written",
        run: async (): Promise<void> => {
            const mounted = await mountHost();
            try {
                mounted.stores.get(PANEL_ID)?.write({ live: true });
                assertEqual(
                    mounted.host.seedPortState(PANEL_ID, {
                        schemaVersion: SCHEMA_VERSION,
                        data: { live: false, stale: true },
                    }),
                    true,
                    "the seed reports restored — the panel IS holding state, and its own is fresher",
                );
                assertEqual(
                    mounted.stores.get(PANEL_ID)?.read(),
                    { live: true },
                    "…but the panel's own write STANDS; the outcome is order-independent, not merely " +
                        "usually right",
                );
            } finally {
                mounted.dispose();
            }
        },
    },

    // ------------------------------------------------------------------ the full reload round trip
    {
        // THE DoD LINE, end to end on this side of the bridge: "state blob round-trips (reload
        // preserves state)". Host A stores a blob and publishes; host B starts fresh against the SAME
        // editor-state document and restores it.
        //
        // ⚠ PLANT (h): drop the `portState` branch from `LayoutPersistence.#publish` — RED, and the
        //   symptom is exactly the silent one this case exists to catch: the SET succeeded, the panel
        //   read it back all session, and the state was simply not in the published document.
        // ⚠ PLANT (i): drop the `seedPortState` branch from `LayoutPersistence.restore` — RED at the
        //   restore assertion, with the publish half still perfect.
        name: "panelstate: a package panel's blob survives publish -> restore into a FRESH host (reload)",
        run: async (): Promise<void> => {
            const shell = mockShell([iframeManifest()]);
            const first = await mountHost({ shell });
            // DECLARED OUTSIDE THE TRY so `finally` can always reach it. `attach` registers a
            // window-level `pagehide` listener and a Dockview layout subscription; a case that threw
            // an assertion before disposing would leak both into every LATER case in this shared
            // document — the kind of harness leak that surfaces as an unrelated suite going
            // intermittent, which is exactly what this tier must not acquire.
            let persistence: LayoutPersistence | undefined;
            try {
                first.stores.get(PANEL_ID)?.write({ filter: "mesh", scroll: 12 });
                persistence = new LayoutPersistence({
                    panelHost: first.host,
                    panelClient: new PanelClient(shell.bridge),
                    stateClient: new EditorStateClient(shell.bridge),
                });
                // THE REAL TRIGGER, not a private method reached through a cast: `attach` registers
                // the production `pagehide` listener and this dispatches the production event. A
                // publish forced any other way would prove the gather works and leave the thing that
                // actually calls it untested.
                persistence.attach();
                window.dispatchEvent(new Event("pagehide"));
                await waitFor("the editor-state publish", () => shell.published !== null);
                const published = shell.published;
                assert(published !== null, "the editor-state document was published");
                assertEqual(
                    (published?.["panels"] as Record<string, unknown>)[PANEL_ID],
                    { schemaVersion: SCHEMA_VERSION, data: { filter: "mesh", scroll: 12 } },
                    "the PACKAGE panel's blob is in the published document — the half a Shell " +
                        "round trip could never have supplied, since an iframe panel has no C++ model",
                );
            } finally {
                persistence?.dispose();
                first.dispose();
            }

            // THE RELOAD. A fresh host against the SAME document — every line a real restart runs on
            // this side of the bridge.
            shell.persisted = shell.published as Record<string, unknown>;
            const second = await mountHost({ shell });
            persistence = undefined;
            try {
                assertEqual(
                    second.stores.get(PANEL_ID)?.read(),
                    null,
                    "the fresh panel starts with nothing, as it must before a restore",
                );
                persistence = new LayoutPersistence({
                    panelHost: second.host,
                    panelClient: new PanelClient(shell.bridge),
                    stateClient: new EditorStateClient(shell.bridge),
                });
                const report = await persistence.restore();
                assertEqual(
                    report.panelsRestored,
                    1,
                    "the restore counts the package panel exactly as it counts a built-in",
                );
                assertEqual(report.degraded.length, 0, "…with nothing degraded");
                assertEqual(
                    second.stores.get(PANEL_ID)?.read(),
                    { filter: "mesh", scroll: 12 },
                    "AND the panel reads its own blob back after the reload — which is what " +
                        "bridge.state.get answers with, so the package resumes where it left off",
                );
            } finally {
                persistence?.dispose();
                second.dispose();
            }
        },
    },
];
