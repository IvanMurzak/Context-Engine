// T1 for the boot-time layout restore (issue #474): `PanelHost.restoreLayout` against the REAL
// pinned Dockview, in a real browser. The defect this file keeps dead: Dockview's `fromJSON`
// CLEARS the live grid before it parses, so a blob that is not a restorable arrangement — the
// `{}` a pre-fix Shell persisted for every fresh project, or a corrupt hand-edited grid — used
// to destroy the default dock `start()` had just opened and boot an empty window on every
// second launch. Real Dockview and not a stand-in, because the clear-before-parse order IS the
// hazard: a fake that parsed first would pass these cases while the shipped editor wiped.

import { assert, assertEqual, type TestCase } from "./harness.js";
import { ShellBridge, type BridgeQuery, type BridgeQueryFunction } from "../bridge.js";
import { detectDockview } from "../dockview.js";
import { PanelHost } from "../panelhost.js";
import { PANEL_LIST_METHOD, PanelClient } from "../panels.js";

// ------------------------------------------------------------------------------ the tiny harness
//
// The extpanel suite's mock, narrowed to what a LAYOUT assertion needs: a roster of `uitree`
// panels, every method but `panel.list` refused exactly as the real router's deny-unknown default
// does. A refused `panel.render` leaves a mounted panel with no content — which is fine, because
// these cases assert on the ARRANGEMENT, never on what a panel drew.

function uitreeManifestJson(id: string): Record<string, unknown> {
    return {
        id,
        kind: "panel",
        title: id,
        icon: "",
        contractVersion: 3,
        dock: { zone: "right", minWidth: 0, minHeight: 0 },
        instances: { mode: "singleton", max: 0 },
        path: "",
        content: { type: "uitree", entry: "" },
        state: { schemaVersion: 1 },
        capabilities: [],
        commands: [],
        hosted: true,
        gestures: false,
        persists: false,
        revision: 1,
    };
}

function mockBridge(panels: readonly Record<string, unknown>[]): ShellBridge {
    let served = 0;
    const query: BridgeQueryFunction = (request: BridgeQuery): number => {
        const parsed = JSON.parse(request.request) as { id: number; method: string };
        served += 1;
        const envelope =
            parsed.method === PANEL_LIST_METHOD
                ? { jsonrpc: "2.0", id: parsed.id, result: { contractMajor: 2, panels } }
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
    return new ShellBridge(query);
}

interface Mounted {
    readonly host: PanelHost;
    dispose(): void;
}

/** Start a REAL PanelHost over REAL Dockview hosting the given `uitree` panel ids. */
async function mountHost(ids: readonly string[]): Promise<Mounted> {
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
    const host = new PanelHost({
        container,
        client: new PanelClient(mockBridge(ids.map(uitreeManifestJson))),
        dockview: dockview as NonNullable<typeof dockview>,
    });
    const report = await host.start();
    assert(report.started, "the docking root came up");
    return {
        host,
        dispose: (): void => {
            host.dispose();
            container.remove();
        },
    };
}

/**
 * The panel ids Dockview is actually hosting, read back from the arrangement the ENGINE itself
 * serializes — the same artifact the Shell persists — so a wiped grid cannot hide behind a
 * host-side cache.
 */
function hostedPanelIds(host: PanelHost): string {
    const layout = host.captureLayout();
    if (layout === null || typeof layout !== "object") {
        return "";
    }
    const panels = (layout as { panels?: unknown }).panels;
    if (panels === null || typeof panels !== "object") {
        return "";
    }
    return Object.keys(panels as Record<string, unknown>)
        .sort()
        .join(",");
}

// ------------------------------------------------------------------------------------- the cases

export const layoutRestoreTests: readonly TestCase[] = [
    {
        name: "restoreLayout: the poisoned `{}` blob restores nothing and the default dock survives (#474)",
        run: async () => {
            const mounted = await mountHost(["builtin.problems", "builtin.scenetree"]);
            try {
                const before = hostedPanelIds(mounted.host);
                assertEqual(
                    before,
                    "builtin.problems#1,builtin.scenetree#1",
                    "start() opened both rostered panels — otherwise every surviving-dock " +
                        "assertion below is vacuous. Dockview keys its arrangement by INSTANCE id " +
                        "since editor-UX c3, so a bare panel id here would mean the rekeying never " +
                        "reached the engine",
                );
                assertEqual(
                    mounted.host.restoreLayout({}),
                    false,
                    "`{}` — what a pre-fix Shell persisted for every fresh project — is " +
                        "'nothing to restore', never an arrangement",
                );
                assertEqual(
                    hostedPanelIds(mounted.host),
                    before,
                    "and the live dock was not touched: pre-fix, fromJSON({}) cleared the " +
                        "grid before failing, which is the empty second-boot window",
                );
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "restoreLayout: an arrangement with a grid but no panels degrades to the defaults",
        run: async () => {
            // The OTHER shape that can show nothing: structurally a layout, but empty. Restoring
            // it "successfully" is indistinguishable from the #474 wipe for the human, so it is
            // refused the same way — an empty dock is never a better outcome than the defaults.
            const mounted = await mountHost(["builtin.problems"]);
            try {
                const before = hostedPanelIds(mounted.host);
                const empty = {
                    grid: {
                        root: { type: "branch", data: [] },
                        orientation: "HORIZONTAL",
                        width: 800,
                        height: 600,
                    },
                    panels: {},
                };
                assertEqual(mounted.host.restoreLayout(empty), false, "nothing to restore");
                assertEqual(hostedPanelIds(mounted.host), before, "the defaults stand");
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "restoreLayout: a corrupt blob throws AND the live dock is rolled back",
        run: async () => {
            const mounted = await mountHost(["builtin.problems", "builtin.scenetree"]);
            try {
                const before = hostedPanelIds(mounted.host);
                let threw = false;
                try {
                    mounted.host.restoreLayout({
                        grid: { root: { type: "branch", data: "garbage" } },
                        panels: { ghost: {} },
                    });
                } catch {
                    threw = true;
                }
                assert(
                    threw,
                    "the pinned Dockview rejects this corrupt grid — if a bump makes it stop " +
                        "throwing, the rollback below is no longer exercised and this case " +
                        "needs a corrupt shape the new engine still refuses",
                );
                assertEqual(
                    hostedPanelIds(mounted.host),
                    before,
                    "fromJSON wiped the grid before throwing, and restoreLayout rolled the " +
                        "wipe back — without the rollback this reads empty",
                );
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "restoreLayout: a REAL captured arrangement still round-trips",
        run: async () => {
            // The non-vacuity half: the guards above must not have made restore refuse
            // everything. A layout captured from a donor host restores into a fresh one.
            const donor = await mountHost(["builtin.problems", "builtin.scenetree"]);
            let layout: unknown;
            try {
                layout = donor.host.captureLayout();
                assert(layout !== null, "the donor produced an arrangement");
            } finally {
                donor.dispose();
            }
            const target = await mountHost(["builtin.problems", "builtin.scenetree"]);
            try {
                assertEqual(target.host.restoreLayout(layout), true, "the arrangement restores");
                assertEqual(
                    hostedPanelIds(target.host),
                    "builtin.problems#1,builtin.scenetree#1",
                    "and the restored dock hosts the donor's panels, by the INSTANCE ids it saved",
                );
            } finally {
                target.dispose();
            }
        },
    },
];
