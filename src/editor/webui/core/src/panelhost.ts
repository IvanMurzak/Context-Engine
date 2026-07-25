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
// M9 e13a-2 ADDS THE THIRD CONTENT TYPE, `iframe` — a THIRD-PARTY package panel (04 §5). It changes
// nothing about the division of labour above: a package panel docks, floats and tears out through
// exactly the same Dockview geometry calls, and its lifecycle lives here like every other panel's.
// What it adds is a TRUST TIER — see `IframePanelRenderer` — and one structural consequence: the
// per-content-type gate that used to sit inline in `start`'s loop is now the `#mountable` predicate,
// because `openById` (e10b's seed path) reached `open` without it and a gate one caller can walk
// past is not a gate. `#renderer` is fail-closed for the same reason.
//
// ⚠ WHAT IS DELIBERATELY NOT HERE: layout PERSISTENCE. `toJSON`/`fromJSON` are exposed
// (`captureLayout` / `restoreLayout`) because they are geometry operations this class owns, but
// nothing writes them anywhere. The Shell is the SINGLE WRITER of `.editor/editor-state.json`
// (C-F3, design 03 §1), and publishing layout to it over the bridge is **e05d2**'s task. A direct
// write from editor-core would be a defect even if it worked.

import type { DockviewApi, DockviewContentRenderer, DockviewModule } from "./dockview.js";
import { detectDockview } from "./dockview.js";
import {
    IFRAME_ALLOW,
    IFRAME_LOADING,
    IFRAME_REFERRER_POLICY,
    IFRAME_SANDBOX,
    parseExtPanelEntry,
} from "./extpanel.js";
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

/** Dockview's component name for an `iframe` (third-party package) panel — one type for all, as above. */
const IFRAME_COMPONENT = "context-iframe-panel";

/**
 * Dockview's component name for a panel THIS BUILD cannot draw. It exists so `componentFor` never
 * labels a drifted manifest with the sink's name: that label is what `captureLayout` PERSISTS, so
 * naming it `context-uitree-panel` would write "draw this through the hydration sink" into the saved
 * arrangement of a panel every gate in this file just refused.
 */
const UNAVAILABLE_COMPONENT = "context-unavailable-panel";

/**
 * The class the iframe panel's own DOM slot carries, so the T1 tier can find it — and so `app.css`
 * can size the frame to its docked slot. The live smoke reads no DOM at all (its observable is which
 * URLs the scheme handler served), so it is deliberately NOT a consumer of this.
 */
export const IFRAME_PANEL_CLASS = "ctx-panel-frame";

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
 * The Dockview content renderer for a THIRD-PARTY package panel (M9 e13a-2, `content.type: "iframe"`,
 * design 04 §5 / 08 §1-§2).
 *
 * THE TRUST INVERSION THAT MAKES THIS CLASS DIFFERENT FROM ITS TWO SIBLINGS. `UitreePanelRenderer`
 * mounts bytes the EDITOR produced (`render_html`'s escaping contract) and `LocalPanelRenderer` runs
 * editor-core's OWN code. This one hosts code we did not write, cannot review, and — from e13b
 * onward — did not even ship. So it mounts NOTHING into the editor's document: it creates one
 * `<iframe>`, points it at the package's own origin, and never touches its contents again. There is
 * no `innerHTML` anywhere in this class and there must never be one; a panel's HTML is loaded BY THE
 * BROWSER over `context-ext://`, under the strict response CSP the Shell serves (ext_scheme.h), in a
 * document whose origin is opaque to us and to every other package.
 *
 * WHAT ISOLATES IT, in the order the browser applies them:
 *
 *   1. `sandbox="allow-scripts"` WITHOUT `allow-same-origin` — an OPAQUE origin. The panel cannot
 *      read this document, cannot reach another package's origin, and holds no storage keyed to the
 *      package. See `extpanel.ts` `IFRAME_SANDBOX` for why each omitted token is omitted. This class
 *      SETS the attribute from that constant; what checks the value a browser actually parsed off
 *      the RENDERED element is the T1 tier (`extpanel.test.ts` drives `isSandboxSafe` over the live
 *      DOM). There is deliberately no runtime re-read here: it would be a branch no test can reach
 *      without stubbing a module constant, and the tier that CAN reach it already does.
 *   2. The `src` is a validated `context-ext://<package-id>/…` URL — the ONLY string this class
 *      accepts, and the ONLY layer at which a `javascript:` / `data:` / `https:` entry can be
 *      refused at all (the Shell's resolver never sees a URL the browser did not route to it).
 *   3. The Shell's response CSP (`default-src 'none'`, `connect-src 'none'`, `script-src 'self'`,
 *      `frame-ancestors context-editor://app`) governs the loaded document, and editor-core's own
 *      `frame-src context-ext:` is the embedder-side backstop.
 *
 * ⚠ NO BRIDGE IS HANDED TO THE FRAME — e13a-2 lands the HOST, not the transport. A sandboxed frame
 * has origin `"null"`, so `event.origin` is not authentication and the port must be handed over at
 * creation (B-F6); that is e13b's MessageChannel work. Until it exists this class deliberately
 * installs NO `message` listener: an editor that listened now would be accepting unauthenticated
 * postMessages from untrusted code for no capability in return. `ext_scheme.h`'s e13a-2 note about
 * a frame re-navigating ITSELF to another package's origin is why the eventual port must key off the
 * handshake's verified origin rather than off this element — recorded here so the class that will
 * hold the port carries the warning.
 */
class IframePanelRenderer implements PanelRenderer {
    readonly element: HTMLElement;
    readonly #url: string;
    readonly #panelId: string;
    #frame: HTMLIFrameElement | undefined;
    #suspended = false;

    constructor(panelId: string, url: string) {
        this.#panelId = panelId;
        this.#url = url;
        this.element = document.createElement("div");
        this.element.className = `ctx-panel-body ${IFRAME_PANEL_CLASS}`;
        this.element.setAttribute("data-panel-id", panelId);
    }

    get suspended(): boolean {
        return this.#suspended;
    }

    /**
     * Dockview's content-initialisation hook — REQUIRED (see `UitreePanelRenderer.init`). Builds the
     * frame exactly ONCE, like the local renderer and for a stronger reason: re-creating it would
     * reload the package's document and discard whatever the user was doing inside it, and a frame
     * rebuilt on every show is a frame whose sandbox attribute is re-derived on every show.
     */
    init(): void {
        this.refresh();
    }

    refresh(): void {
        // ONE-WAY, exactly like `LocalPanelRenderer.#built`: `dispose` deliberately LEAVES this set,
        // so the single field means "already built OR torn down — never build again" rather than
        // needing a second flag to say the second half. Without that, a `refresh`/`onShow` arriving
        // after `dispose` would build a SECOND frame and re-navigate to the package URL inside an
        // element no longer in the document — a load nobody can see, in a document nobody will tear
        // down.
        if (this.#frame !== undefined) {
            return;
        }
        const frame = document.createElement("iframe");
        // SANDBOX FIRST, BEFORE `src`. Attribute order is load-bearing here: the sandbox flags are
        // computed when the frame's document begins loading, and `src` is what starts that load, so
        // setting `src` first opens a window in which the frame is momentarily un-sandboxed. The
        // element is not in the document yet either (it is appended last), which closes the same gap
        // a second way — belt and braces, because only one of the two survives a careless edit.
        frame.setAttribute("sandbox", IFRAME_SANDBOX);
        frame.setAttribute("allow", IFRAME_ALLOW);
        frame.setAttribute("referrerpolicy", IFRAME_REFERRER_POLICY);
        frame.setAttribute("loading", IFRAME_LOADING);
        // A labelled frame is a labelled landmark: a screen reader announces WHICH panel focus moved
        // into, exactly as the uitree renderer's `data-panel-id` slot does (R-A11Y-001).
        frame.setAttribute("title", this.#panelId);
        frame.setAttribute("data-panel-id", this.#panelId);
        // A literal, like `ctx-panel-body` in the three sibling renderers: `app.css` carries the
        // matching rule and CSS cannot import a TS constant, so naming it here would buy a symbol on
        // editor-core's public surface and no enforcement. `IFRAME_PANEL_CLASS` is a constant only
        // because another MODULE (the T1 tier) selects on it.
        frame.className = "ctx-panel-frame-element";
        frame.setAttribute("src", this.#url);
        this.#frame = frame;
        this.element.replaceChildren(frame);
    }

    onShow(): void {
        this.#suspended = false;
        this.refresh();
    }

    onHide(): void {
        // A tabbed-away package panel keeps its frame and therefore its state. Dropping the frame
        // would be a reload disguised as an optimisation — the D6 purity rule cuts the other way for
        // a document we do not own: we cannot rebuild it, so we must not discard it.
        this.#suspended = true;
    }

    dispose(): void {
        // Removing the element is what actually tears the package's document down; nulling `src`
        // first would navigate the frame to `about:blank` (a load in a frame we are discarding) for
        // no benefit. `#frame` is deliberately NOT cleared — see `refresh`: leaving it set is what
        // keeps the latch one-way, and the renderer itself is dropped from `#panels` right after.
        this.element.replaceChildren();
    }
}

/**
 * The renderer for a panel this build must NOT draw — an inert element and nothing else.
 *
 * It exists so `#renderer` can be fail-closed for EVERY content type rather than only for the
 * `iframe` sink: the fall-through used to be `UitreePanelRenderer`, which means a manifest this
 * build does not understand (`unknown`, or a `local` whose factory this bundle does not carry) got
 * drawn through the `innerHTML` hydration sink — precisely what `#mountable`'s own doc says must
 * never happen. It mounts nothing, calls no client method, and holds no runtime, so a panel that
 * reaches it is visibly and honestly empty.
 */
class UnavailablePanelRenderer implements PanelRenderer {
    readonly element: HTMLElement;

    constructor(panelId: string) {
        this.element = document.createElement("div");
        this.element.className = "ctx-panel-body";
        this.element.setAttribute("data-panel-id", panelId);
        // MARKED, not merely empty. Without this the slot is byte-identical to a healthy panel that
        // has not drawn yet, and "the editor is missing a panel" would be something a user discovers
        // by its absence — the failure `start`'s own `unavailable` report exists to prevent. This is
        // that report's equivalent for the one path that does not produce one (`restoreLayout`), and
        // it is what the T1 tier asserts on.
        this.element.setAttribute("data-panel-unavailable", "");
    }

    get suspended(): boolean {
        return true;
    }

    /** REQUIRED by Dockview even here — see `UitreePanelRenderer.init` for what omitting it costs. */
    init(): void {}

    refresh(): void {}

    dispose(): void {}
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
            if (!this.#mountable(manifest)) {
                // Rostered but not mountable in THIS build. NAMED in the report rather than
                // dropped: "the editor is missing a panel" must be an observable fact, not
                // something a user discovers by its absence. Three distinct causes land here — a
                // D10-blocked uitree panel with no provider, a `local` panel this bundle has no
                // factory for, and an `iframe` panel whose entry is not a URL editor-core may load
                // — and `#mountable` is where each is decided, once, for both this loop and `open`.
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
        // THE SINK GATE, APPLIED AT THE ONE CHOKEPOINT rather than in `start`'s loop (M9 e13a-2).
        // `start` is not the only caller: `openById` (e10b's tear-out seed path) reaches here with a
        // roster manifest and no content-type check of its own, so a gate that lived only in the
        // loop above would be bypassed by a seeded window — and for an `iframe` manifest that means
        // a third-party entry URL routed into `#create`. Refusing here is what makes the gate a
        // property of the class instead of a property of one code path.
        if (!this.#mountable(manifest)) {
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
            component: componentFor(manifest),
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
        const renderer = this.#renderer(panelId, manifest);
        if (manifest !== undefined) {
            this.#panels.set(panelId, { manifest, renderer });
        }
        return renderer;
    }

    /**
     * Build the renderer a manifest calls for.
     *
     * FAIL-CLOSED ON THE SINK, and — since e13a-2's review — fail-closed for EVERY content type,
     * not just for `iframe`. `UitreePanelRenderer` mounts its payload through `innerHTML`, so it is
     * the sink, and it is now reached only by a manifest that explicitly says `uitree`. Everything
     * else (an `iframe` entry that does not parse, a `local` whose factory this bundle does not
     * carry, an `unknown` token `parsePanelManifest` failed closed on, or a panel id that is not in
     * the roster at all) gets `UnavailablePanelRenderer` — because "defaulting a drifted manifest
     * into ANY renderer is how a future content type ends up in today's HTML sink" is `#mountable`'s
     * own rule, and a fall-through `else` into the sink is exactly that default.
     *
     * WHY THE GUARD IS DUPLICATED AT ALL — `open` already refuses every one of these via
     * `#mountable`, so in practice this is unreachable FROM `open`. It is not unreachable from
     * Dockview: `restoreLayout` hands a persisted arrangement to `fromJSON`, which drives
     * `createComponent` → `#create` → here WITHOUT passing `open`. That is the "third place" this
     * comment used to speak of hypothetically, and it is a real caller. The two guards stay
     * independent on purpose — this is the construction chokepoint, `#mountable` is the policy — but
     * neither is a copy of the other, which is what makes them safe to keep both.
     */
    #renderer(panelId: string, manifest: PanelManifest | undefined): PanelRenderer {
        // Narrowed through an `if` rather than `switch (manifest?.contentType)` so TS keeps the
        // narrowing inside each arm; the shape then matches `#mountable` and `componentFor`, which
        // switch on the same discriminant directly below.
        if (manifest === undefined) {
            return new UnavailablePanelRenderer(panelId);
        }
        switch (manifest.contentType) {
            case "local": {
                const localFactory = this.#localPanels.get(panelId);
                return localFactory === undefined
                    ? new UnavailablePanelRenderer(panelId)
                    : new LocalPanelRenderer(panelId, localFactory);
            }
            case "iframe": {
                // An unparseable entry is UNAVAILABLE, not an `about:blank` frame. Both spell "this
                // panel cannot be drawn", and one spelling is enough — the `about:blank` form also
                // contradicted `IframePanelRenderer`'s own invariant that a validated
                // `context-ext://…` URL is the ONLY string it accepts.
                const entry = parseExtPanelEntry(manifest.contentEntry);
                return entry === null
                    ? new UnavailablePanelRenderer(panelId)
                    : new IframePanelRenderer(panelId, entry.url);
            }
            case "uitree":
                return new UitreePanelRenderer(panelId, this.#client, manifest.gestures);
            default:
                return new UnavailablePanelRenderer(panelId);
        }
    }

    /**
     * Can THIS build mount this panel? The ONE predicate `start` reports `unavailable` from and
     * `open` refuses on (M9 e13a-2) — previously three separate checks inside `start`'s loop, which
     * is how `openById` came to bypass them.
     *
     * Per content type:
     *   * `uitree` — needs a Shell PROVIDER (`hosted`). The D10-blocked panels were unavailable here
     *     until e05d3 bound theirs.
     *   * `local` — has no Shell provider BY CONSTRUCTION (`hosted` is always false for it), so it
     *     is gated on THIS bundle carrying its factory instead.
     *   * `iframe` — likewise has no Shell provider by construction: its bytes are served over
     *     `context-ext://`, not over `panel.render`. It is gated on its `content.entry` being a URL
     *     `parseExtPanelEntry` accepts, which is the whole security decision (extpanel.ts).
     *   * `unknown` — an unrecognised token `parsePanelManifest` failed closed on. Never mountable:
     *     defaulting a drifted manifest into ANY renderer is how a future content type ends up in
     *     today's HTML sink.
     */
    #mountable(manifest: PanelManifest): boolean {
        switch (manifest.contentType) {
            case "uitree":
                return manifest.hosted;
            case "local":
                return this.#localPanels.has(manifest.id);
            case "iframe":
                return parseExtPanelEntry(manifest.contentEntry) !== null;
            default:
                return false;
        }
    }
}

/** Dockview's component name for a manifest — the ONE place the content type picks a renderer type. */
function componentFor(manifest: PanelManifest): string {
    switch (manifest.contentType) {
        case "local":
            return LOCAL_COMPONENT;
        case "iframe":
            return IFRAME_COMPONENT;
        case "uitree":
            return UITREE_COMPONENT;
        default:
            // `unknown` names the UNAVAILABLE component, not the uitree one. `open` refuses such a
            // manifest before this is ever reached, so today this is inert — but this function's
            // result is what `captureLayout` PERSISTS, and a fall-through into the sink's name is
            // the same default `#mountable` and `#renderer` both refuse to make.
            return UNAVAILABLE_COMPONENT;
    }
}
