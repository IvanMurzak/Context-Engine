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

/** Dockview's component name for a `local` panel — one renderer type serves them all, as above. */
const LOCAL_COMPONENT = "context-local-panel";

/**
 * A panel editor-core renders ITSELF (M9 e06d, `content.type: "local"`).
 *
 * The factory is handed the panel's DOM slot and builds into it; it returns an optional disposer for
 * whatever it attached. That is the WHOLE seam — deliberately: a local panel is ordinary editor-core
 * code, and the moment this interface grew a lifecycle of its own it would become a second, weaker
 * panel model competing with the C++ one.
 */
export type LocalPanelFactory = (container: HTMLElement) => (() => void) | void;

/** A panel PanelHost is currently hosting. */
interface HostedPanel {
    readonly manifest: PanelManifest;
    readonly renderer: PanelRenderer;
}

/** What PanelHost needs from any content renderer it mounts. */
interface PanelRenderer extends DockviewContentRenderer {
    readonly suspended: boolean;
    refresh(): void;
    dispose(): void;
}

/**
 * The Dockview content renderer for a `local` panel.
 *
 * Mirrors `UitreePanelRenderer`'s lifecycle exactly (init on materialise, suspend on hide, dispose on
 * close) so PanelHost's own code never branches on which kind of panel it is holding — the ONE place
 * the distinction exists is `#create`, which is the same place Dockview's does.
 */
class LocalPanelRenderer implements PanelRenderer {
    readonly element: HTMLElement;
    readonly #factory: LocalPanelFactory;
    #teardown: (() => void) | undefined;
    #suspended = false;
    #built = false;

    constructor(panelId: string, factory: LocalPanelFactory) {
        this.element = document.createElement("div");
        this.element.className = "ctx-panel-body";
        this.element.setAttribute("data-panel-id", panelId);
        this.#factory = factory;
    }

    get suspended(): boolean {
        return this.#suspended;
    }

    /**
     * Dockview's content-initialisation hook — REQUIRED (see `UitreePanelRenderer.init` for what a
     * missing one costs). Builds exactly once: a local panel owns its own DOM, so re-running the
     * factory on every show would discard state the user is looking at (a half-typed field, a chosen
     * tab) for no gain.
     */
    init(): void {
        this.refresh();
    }

    refresh(): void {
        if (this.#built) {
            return;
        }
        this.#built = true;
        const teardown = this.#factory(this.element);
        this.#teardown = typeof teardown === "function" ? teardown : undefined;
    }

    onShow(): void {
        this.#suspended = false;
        this.refresh();
    }

    onHide(): void {
        this.#suspended = true;
    }

    dispose(): void {
        this.#teardown?.();
        this.#teardown = undefined;
        this.element.replaceChildren();
    }
}

/**
 * The Dockview content renderer for a uitree panel.
 *
 * ONE renderer class serves EVERY panel, and that is the structural form of panel-agnosticism at
 * this layer: Dockview asks for a component by name, and PanelHost answers with the same class no
 * matter which panel it is for. A per-panel renderer class — or a `switch` on the panel id inside
 * this one — is exactly what would make adding a panel a code change here.
 */
class UitreePanelRenderer implements PanelRenderer {
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

    /** Re-pull this panel's render. The uniform seam PanelHost's `refreshAll` drives. */
    refresh(): void {
        void this.#runtime.refresh();
    }

    /**
     * Dockview's content-initialisation hook — and the panel-agnostic home of the FIRST hydration
     * render.
     *
     * REQUIRED, not optional. Dockview-core@7 calls `content.init(params)` UNCONDITIONALLY from
     * inside `addPanel` (the sole lifecycle method it invokes on a content renderer); a renderer
     * without it throws `TypeError: this.content.init is not a function` SYNCHRONOUSLY, which aborts
     * `PanelHost.start()` right after `panel.list` and before any `panel.render` — the editor then
     * comes up docked-but-empty with no build error and no console throw the boot path surfaces. No
     * local test can catch it: there is no TS/DOM test tier, and the C++ suites drive the Shell-side
     * host, never this renderer against real Dockview — only the live CEF smoke does.
     *
     * Kicking the first `refresh()` HERE (rather than from a separate pass in `start`) makes the
     * render fire exactly when Dockview has materialised the panel's DOM slot, so it is correct
     * whether Dockview creates a panel eagerly or only when it becomes visible, and it needs no panel
     * id — the property e05d3 depends on. Fire-and-forget on purpose: a slow render must not delay
     * the arrangement appearing (the layout is up, the content fills in). `params` is unused — the
     * manifest is looked up from the roster, never taken from Dockview.
     */
    init(): void {
        void this.#runtime.refresh();
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
    /**
     * Factories for the `content.type: "local"` panels this build can render (M9 e06d).
     *
     * KEYED BY ROSTER ID, and the roster still decides what exists: a factory with no roster entry is
     * never mounted (nothing asks for it), and a local roster entry with no factory is reported
     * `unavailable` exactly like an unhosted panel. So this map cannot introduce a panel behind the
     * manifest's back — it only says which of the manifest's panels THIS bundle knows how to draw.
     */
    readonly localPanels?: ReadonlyMap<string, LocalPanelFactory>;
}

/** Options for `PanelHost.start`. */
export interface PanelHostStartOptions {
    /**
     * When set, open ONLY the panel with this id instead of the whole default roster (M9 e10b) — the
     * TEAR-OUT target's boot path: a new window seeded with a single moved panel shows exactly it, not
     * a fresh default arrangement. An unhosted / unknown id opens nothing (reported like any other
     * unavailable panel), so a stale seed cannot force an empty-but-broken window.
     */
    readonly only?: string;
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
    readonly #localPanels: ReadonlyMap<string, LocalPanelFactory>;
    readonly #panels = new Map<string, HostedPanel>();
    #api: DockviewApi | null = null;
    #roster: PanelRoster | null = null;

    constructor(options: PanelHostOptions) {
        this.#container = options.container;
        this.#client = options.client;
        this.#dockview = options.dockview ?? detectDockview();
        this.#localPanels = options.localPanels ?? new Map<string, LocalPanelFactory>();
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
    async start(options: PanelHostStartOptions = {}): Promise<PanelHostStartReport> {
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
            // e10b: a seeded (torn-out) window opens ONLY its one moved panel. Every other rostered
            // panel is skipped here — not reported unavailable, because it IS hostable, just not part
            // of this window's arrangement.
            if (options.only !== undefined && manifest.id !== options.only) {
                continue;
            }
            if (manifest.contentType === "local") {
                // A local panel has no Shell provider, so `hosted` is false for it BY CONSTRUCTION —
                // this branch must therefore come BEFORE the hosted gate below, or the one panel
                // editor-core renders itself would be reported unavailable in every build.
                if (!this.#localPanels.has(manifest.id)) {
                    unavailable.push(manifest.id);
                    continue;
                }
                if (this.open(manifest)) {
                    mounted += 1;
                }
                continue;
            }
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
        // The FIRST render is driven per panel by `UitreePanelRenderer.init` — Dockview's own
        // lifecycle hook, fired as each `addPanel` above materialises the panel. Mount and
        // first-render therefore stay separate passes (a slow render never delays the arrangement)
        // without this method reaching into the panel set Dockview populates, and without depending
        // on `createComponent` having run synchronously. See `UitreePanelRenderer.init`.
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
            component: manifest.contentType === "local" ? LOCAL_COMPONENT : UITREE_COMPONENT,
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

    /**
     * Open one panel BY ID, looked up from the roster (M9 e10b). The seed-open path: a torn-out or
     * rehomed panel arrives as an id + a D6 state blob, and the target window opens it here, then the
     * caller restores the blob over `panel.state.set`. Returns false when the id is unknown to this
     * build's roster or already open — the same honest outcomes `open` reports.
     */
    openById(panelId: string): boolean {
        const manifest = this.#roster?.panels.find((entry) => entry.id === panelId);
        return manifest !== undefined && this.open(manifest);
    }

    /**
     * Float a mounted panel's group inside THIS window (M9 e10b) — the LOUD degradation home when a
     * secondary-window create fails (03 §7). The panel does not silently stay where it was: it becomes
     * a floating Dockview group, which `dockview-core` renders with an inline `position:absolute` the
     * user (and the live smoke) can SEE. Returns false when the panel is not mounted or the docking
     * root is down (nothing to float), so the caller can report the degrade honestly.
     */
    floatPanel(panelId: string): boolean {
        const panel = this.#api?.getPanel(panelId);
        if (this.#api === null || panel === undefined) {
            return false;
        }
        this.#api.addFloatingGroup(panel);
        return true;
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
                .map(async (panel) => {
                    panel.renderer.refresh();
                }),
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
        const localFactory = this.#localPanels.get(panelId);
        const renderer: PanelRenderer =
            manifest?.contentType === "local" && localFactory !== undefined
                ? new LocalPanelRenderer(panelId, localFactory)
                : new UitreePanelRenderer(panelId, this.#client, manifest?.gestures ?? false);
        if (manifest !== undefined) {
            this.#panels.set(panelId, { manifest, renderer });
        }
        return renderer;
    }
}
