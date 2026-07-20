// PanelHost (M9 e05d1, design 04 §2-§3) — the owner of PANEL LIFECYCLE over Dockview's geometry.
//
// THE DIVISION OF LABOUR, which is the whole reason this class exists rather than calling Dockview
// directly from the app: **Dockview manages geometry ONLY** (04 §2). Splits, tabs, floating groups,
// sash dragging, serialization of the arrangement — all Dockview's. Everything about a panel's LIFE
// — when it is created, what it mounts, when it suspends, when its state is captured, when it is
// disposed — is PanelHost's. Keeping that line sharp is what makes the D2 fallback real: swapping
// Dockview for Golden Layout or Lumino replaces the geometry calls in this file and `dockview.ts`,
// and touches no panel and no hydration code.
//
// ⚠ TEAR-OUT IS NOT DOCKVIEW'S POPOUT (B-F2, ratified at s1). Dockview v7 rejects non-http(s) popout
// URLs before `window.open` fires, and its popout is opener-owned DOM transfer, which cannot produce
// the independent per-window editor-core instances design 04 §1 requires. OS-window tear-out is a
// PanelHost/Shell mechanism arriving in **e10**; this file is built so it can land there — panel
// lifecycle and panel state already live here, not inside Dockview — but it does not attempt it, and
// nothing here names `popout`.
//
// ⚠ WHAT IS DELIBERATELY NOT HERE: layout PERSISTENCE. `toJSON`/`fromJSON` are exposed
// (`captureLayout` / `restoreLayout`) because they are geometry operations this class owns, but
// nothing writes them anywhere. The Shell is the SINGLE WRITER of `.editor/editor-state.json`
// (C-F3, design 03 §1), and publishing layout to it over the bridge is **e05d2**'s task. A direct
// write from editor-core would be a defect even if it worked.

import type { DockviewApi, DockviewContentRenderer, DockviewModule } from "./dockview.js";
import { detectDockview } from "./dockview.js";
import { HydrationRuntime } from "./hydration.js";
import type { PanelClient, PanelManifest, PanelRoster } from "./panels.js";

/** Dockview's component name for every uitree panel. One renderer type serves them all — see `#create`. */
const UITREE_COMPONENT = "context-uitree-panel";

/** Maps the manifest's dock zone onto Dockview's placement vocabulary. */
const ZONE_DIRECTION: Readonly<Record<string, "left" | "right" | "above" | "below" | "within">> = {
    left: "left",
    right: "right",
    top: "above",
    bottom: "below",
    center: "within",
};

/** A panel PanelHost is currently hosting. */
interface HostedPanel {
    readonly manifest: PanelManifest;
    readonly renderer: UitreePanelRenderer;
}

/**
 * The Dockview content renderer for a uitree panel.
 *
 * ONE renderer class serves EVERY panel, and that is the structural form of panel-agnosticism at
 * this layer: Dockview asks for a component by name, and PanelHost answers with the same class no
 * matter which panel it is for. A per-panel renderer class — or a `switch` on the panel id inside
 * this one — is exactly what would make adding a panel a code change here.
 */
class UitreePanelRenderer implements DockviewContentRenderer {
    readonly element: HTMLElement;
    readonly #runtime: HydrationRuntime;
    #suspended = false;

    constructor(panelId: string, client: PanelClient, gestures: boolean) {
        this.element = document.createElement("div");
        this.element.className = "ctx-panel-body";
        // The panel's DOM slot is a labelled landmark in its own right, so a screen reader announces
        // which panel focus moved into when the user tabs between docked groups.
        this.element.setAttribute("data-panel-id", panelId);
        this.#runtime = new HydrationRuntime(this.element, client, panelId, {
            gestures,
            // A dispatch changed the model, so pull the new render immediately rather than waiting
            // for the next poll — this is what makes a click feel instant instead of eventually
            // consistent.
            onDispatched: () => {
                void this.#runtime.refresh();
            },
        });
    }

    get runtime(): HydrationRuntime {
        return this.#runtime;
    }

    get suspended(): boolean {
        return this.#suspended;
    }

    /** Dockview's visibility hooks, mapped onto the SUSPEND half of the panel lifecycle. */
    onShow(): void {
        this.#suspended = false;
        void this.#runtime.refresh();
    }

    onHide(): void {
        // A tabbed-away panel stops refreshing but keeps its DOM and its state. That is the D6
        // purity rule staying honest: nothing is retained that could not be rebuilt from (bridge
        // state, state blob), so a suspended panel is an optimisation and never a hidden cache.
        this.#suspended = true;
    }

    dispose(): void {
        this.#runtime.dispose();
    }
}

export interface PanelHostOptions {
    /** Where the docking root mounts. */
    readonly container: HTMLElement;
    readonly client: PanelClient;
    /** Injected for testability; defaults to the UMD global the staged script publishes. */
    readonly dockview?: DockviewModule;
}

/** Why PanelHost could not start, when it could not. Empty on success. */
export interface PanelHostStartReport {
    readonly started: boolean;
    readonly mounted: number;
    /** Rostered but not mountable in this build (`hosted: false`) — reported, never silently skipped. */
    readonly unavailable: readonly string[];
    readonly error: string;
}

export class PanelHost {
    readonly #container: HTMLElement;
    readonly #client: PanelClient;
    readonly #dockview: DockviewModule | undefined;
    readonly #panels = new Map<string, HostedPanel>();
    #api: DockviewApi | null = null;
    #roster: PanelRoster | null = null;

    constructor(options: PanelHostOptions) {
        this.#container = options.container;
        this.#client = options.client;
        this.#dockview = options.dockview ?? detectDockview();
    }

    get api(): DockviewApi | null {
        return this.#api;
    }

    get roster(): PanelRoster | null {
        return this.#roster;
    }

    /** The ids currently mounted, in mount order. */
    get mounted(): readonly string[] {
        return Array.from(this.#panels.keys());
    }

    /**
     * Read the roster, create the docking root, and open every panel this build can host.
     *
     * NEVER THROWS. Every failure — no docking engine, an unreadable roster, a panel that refused to
     * render — is a REPORTED state, because the alternative in a renderer whose only diagnostic
     * channel is a DOM attribute is an unhandled rejection nobody sees.
     */
    async start(): Promise<PanelHostStartReport> {
        if (this.#dockview === undefined) {
            return {
                started: false,
                mounted: 0,
                unavailable: [],
                error: "the docking engine did not load",
            };
        }
        const roster = await this.#client.list();
        if (roster === null) {
            return {
                started: false,
                mounted: 0,
                unavailable: [],
                error: "the Shell returned no readable panel roster",
            };
        }
        this.#roster = roster;

        this.#api = this.#dockview.createDockview(this.#container, {
            createComponent: (request) => this.#create(request.id),
            theme: this.#dockview.themeDark,
            // Floating groups stay ON — they are Dockview's in-window floating, which is geometry
            // and therefore in scope. What is off is POPOUT (a separate OS window), which this file
            // never calls; see the header.
            disableFloatingGroups: false,
        });

        const unavailable: string[] = [];
        let mounted = 0;
        for (const manifest of roster.panels) {
            if (!manifest.hosted) {
                // The D10-blocked panels (Scene tree, Inspector) land here until e05d3. They are
                // NAMED in the report rather than dropped: "the editor is missing a panel" must be
                // an observable fact, not something a user discovers by its absence.
                unavailable.push(manifest.id);
                continue;
            }
            if (manifest.contentType !== "uitree") {
                // THE SECURITY BOUNDARY, ENFORCED RATHER THAN ASSUMED. `UitreePanelRenderer` mounts
                // its payload through `innerHTML`, which is safe ONLY because a `uitree` payload
                // came from `render_html`'s escaping contract. The contract already carries a
                // second content type (`iframe` — a third-party web panel, 04 §5), whose content is
                // NOT ours and belongs in a sandboxed iframe on a different origin. Nothing
                // registers an iframe provider today, which is exactly why the guard must exist
                // now: the first one to appear would otherwise be routed straight into that sink.
                // `parsePanelManifest` fails closed to `unknown` for an unrecognised token, so a
                // drifted or malformed manifest lands here too rather than defaulting into it.
                unavailable.push(manifest.id);
                continue;
            }
            if (this.open(manifest)) {
                mounted += 1;
            }
        }
        // Mount and first render are separate passes so a slow panel cannot delay the arrangement
        // appearing — the layout is up, then the content fills in.
        await Promise.all(
            Array.from(this.#panels.values()).map((panel) => panel.renderer.runtime.refresh()),
        );
        return { started: true, mounted, unavailable, error: "" };
    }

    /**
     * Open one panel. Returns false when it is already open and `singleton` (the manifest's own rule)
     * or when the docking root is not up.
     */
    open(manifest: PanelManifest): boolean {
        if (this.#api === null) {
            return false;
        }
        if (this.#panels.has(manifest.id)) {
            // A second open of a singleton FOCUSES the existing one rather than duplicating it —
            // the manifest's `dock.singleton` contract (04 §3).
            return false;
        }
        const previous = this.mounted[this.mounted.length - 1];
        this.#api.addPanel({
            id: manifest.id,
            component: UITREE_COMPONENT,
            title: manifest.title,
            // Placement follows the manifest's declared zone. `referencePanel` is only set once
            // something is already mounted — Dockview has nothing to place relative to otherwise.
            ...(previous === undefined
                ? {}
                : {
                      position: {
                          referencePanel: previous,
                          direction: ZONE_DIRECTION[manifest.dock.zone] ?? "within",
                      },
                  }),
        });
        return this.#panels.has(manifest.id);
    }

    /** Close one panel, disposing its hydration runtime. */
    close(panelId: string): boolean {
        const panel = this.#api?.getPanel(panelId);
        if (this.#api === null || panel === undefined) {
            return false;
        }
        this.#api.removePanel(panel);
        this.#panels.get(panelId)?.renderer.dispose();
        return this.#panels.delete(panelId);
    }

    /**
     * Re-render every mounted, non-suspended panel.
     *
     * NOTHING CALLS THIS YET — stated plainly rather than described as "the host's poll tick",
     * which would claim a driver that does not exist. Re-renders today are event-driven per panel:
     * `onDispatched` after a local command, and `onShow` when a panel becomes visible. So a change
     * arriving from the daemon (a diagnostic landing in the live feed) reaches the C++ model and
     * moves its revision, but no DOM update follows until the user next interacts. Wiring a driver
     * — a tick, or a bridge-side change event — is a later task; this method is the seam it will
     * call, and it is already correct for that use.
     */
    async refreshAll(): Promise<void> {
        await Promise.all(
            Array.from(this.#panels.values())
                .filter((panel) => !panel.renderer.suspended)
                .map((panel) => panel.renderer.runtime.refresh()),
        );
    }

    /**
     * The arrangement as Dockview serialises it.
     *
     * Returned, NOT persisted — see the header: the Shell is the single writer of the editor-state
     * document, and publishing this to it is e05d2's task.
     */
    captureLayout(): unknown {
        return this.#api?.toJSON() ?? null;
    }

    /** Restore an arrangement Dockview produced. False when there is no docking root to restore into. */
    restoreLayout(state: unknown): boolean {
        if (this.#api === null || state === null || state === undefined) {
            return false;
        }
        this.#api.fromJSON(state);
        return true;
    }

    /** Dispose every panel and the docking root. Idempotent. */
    dispose(): void {
        for (const panel of this.#panels.values()) {
            panel.renderer.dispose();
        }
        this.#panels.clear();
        this.#api?.dispose();
        this.#api = null;
    }

    /**
     * Dockview's `createComponent` callback: build the ONE renderer type for whichever panel it
     * asked for. The manifest is looked up from the roster rather than passed in, because Dockview
     * hands us only an id.
     */
    #create(panelId: string): DockviewContentRenderer {
        const manifest = this.#roster?.panels.find((entry) => entry.id === panelId);
        const renderer = new UitreePanelRenderer(
            panelId,
            this.#client,
            manifest?.gestures ?? false,
        );
        if (manifest !== undefined) {
            this.#panels.set(panelId, { manifest, renderer });
        }
        return renderer;
    }
}
