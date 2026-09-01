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
// because `openInstance` (e10b's seed path, `openById` before c3) reached `open` without it and a
// gate one caller can walk
// past is not a gate. `#renderer` is fail-closed for the same reason.
//
// M9 e13b-1 GIVES THAT PANEL A TRANSPORT: one authenticated `MessagePort`, owned by
// `IframePanelRenderer` and implemented in `panelport.ts`. Nothing about the division of labour moves
// — the renderer constructs the bridge before `src` and disposes it with itself, and every decision
// about WHO may hold the port, and what any verb answers, lives in that module where the T1 tier can
// drive it directly.
//
// ⚠ WHAT IS DELIBERATELY NOT HERE: layout PERSISTENCE. `toJSON`/`fromJSON` are exposed
// (`captureLayout` / `restoreLayout`) because they are geometry operations this class owns, but
// nothing writes them anywhere. The Shell is the SINGLE WRITER of `.editor/editor-state.json`
// (C-F3, design 03 §1), and publishing layout to it over the bridge is **e05d2**'s task. A direct
// write from editor-core would be a defect even if it worked.

import { BridgeError, isRecord } from "./bridge.js";
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
// VALUE import for the two D6 member names: they are byte-compared against the C++
// `kStateSchemaVersionKey` / `kStateDataKey` by `tools/check_webui_assets.py`, so spelling them as
// literals here would drift SILENTLY past that gate — the gate gets a matching constant, not a
// matching consumer (editorstate.ts § the same reasoning for its own keys).
import {
    PANEL_INSTANCE_SEPARATOR,
    STATE_DATA_KEY,
    STATE_SCHEMA_VERSION_KEY,
    makeInstanceId,
    panelIdOfInstance,
} from "./panels.js";
import type { PanelClient, PanelManifest, PanelRoster } from "./panels.js";
import { PANEL_BRIDGE_REFUSALS, PanelPortBridge } from "./panelport.js";
import type { PanelBridgeReply, PanelVerbHandler } from "./panelport.js";
// `panelverbs.ts` does not import this module, so both directions here are acyclic. The TYPE import
// keeps the table's shape with the factory that produces it; the VALUE import brings in the ONE
// bound-and-canonicalize gate (M9 e13d), shared with the verb so a blob arriving from PERSISTED
// BYTES is held to exactly the rule a blob arriving from the panel is.
import { sanitizePanelState } from "./panelverbs.js";
import type { PanelStateStore, PanelVerbTable } from "./panelverbs.js";
// TYPE-ONLY: the theme CHANNEL is injected (see `PanelThemeChannel`), only its target shape travels.
import type { IframeMessageTarget } from "./theme.js";

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

/** Issue a request DOWN one panel's port. A refusal is a reply, never a rejection. */
export type PanelPortRequest = (verb: string, params?: unknown) => Promise<PanelBridgeReply>;

/**
 * Everything the e13b-2 verb table for ONE third-party panel is built over (panelverbs.ts).
 *
 * Assembled HERE because this is the only place that holds both halves: the manifest (which package,
 * which declared capabilities, which manifest commands) and the port (`request`). The factory itself
 * knows nothing about DOM or Dockview, which is the same split `extpanel.ts` has with this file.
 */
export interface PanelVerbBinding {
    readonly panelId: string;
    /** The `context-ext://<packageId>` authority — `parseExtPanelEntry` validated it. */
    readonly packageId: string;
    /** The manifest's `capabilities`. A DECLARATION, never a grant — panelverbs.ts says why. */
    readonly declaredCapabilities: readonly string[];
    readonly manifestCommandIds: readonly string[];
    readonly request: PanelPortRequest;
    /**
     * THIS PANEL's state store (M9 e13d) — supplied by the renderer, whose lifetime is the panel's.
     *
     * Threaded through the binding rather than created by the factory because the HOST also has to
     * read and seed it (`portState` / `seedPortState`), and a store the factory minted would be
     * reachable only from inside the verb table — leaving persistence with nothing to publish.
     */
    readonly state: PanelStateStore;
}

/** Builds one panel's verb table. Absent ⇒ every panel keeps e13b-1's deny-all empty table. */
export type PanelVerbFactory = (binding: PanelVerbBinding) => PanelVerbTable;

/**
 * The renderer-side half of `PanelVerbFactory`: everything bound except the two things only the
 * renderer holds — the late `request` on the port it is about to create, and (M9 e13d) the state
 * store whose LIFETIME IS THE PANEL's.
 */
type PanelVerbBinder = (request: PanelPortRequest, state: PanelStateStore) => PanelVerbTable;

/**
 * The theme-token PUSH channel a package panel's frame registers with (M9 e13d, design 06 §1 "pushed
 * into panel iframes by the panel host").
 *
 * Structurally `IframeThemeChannel` (theme.ts) and nothing else, but named as a two-method interface
 * for the same reason `PanelClient` is injected rather than constructed here: this module owns DOM
 * and lifecycle, and a hard dependency on the theme engine would make every PanelHost test drag one
 * in. `boot.ts` passes the real channel; a host that passes none simply pushes no tokens, which is
 * what every test that does not opt in should see.
 */
export interface PanelThemeChannel {
    register(target: IframeMessageTarget): void;
    unregister(target: IframeMessageTarget): void;
}

/**
 * The persisted wrapper around one panel's state blob — the D6 shape the C++ models already use
 * (`{schemaVersion, data}`, editorstate.ts § `PersistedState`).
 *
 * REUSED RATHER THAN INVENTED so the editor-state document holds ONE shape for every panel kind: the
 * Shell stores the map opaquely, and a second wrapper spelling would mean a reader had to know which
 * kind of panel wrote each entry before it could parse it.
 */
export interface PersistedPanelState {
    readonly schemaVersion: number;
    readonly data: unknown;
}

function isPersistedPanelState(value: unknown): value is PersistedPanelState {
    return (
        // `isRecord` rather than a fourth inline copy of the plain-object test — the sibling
        // `editorstate.ts` already imports it from `bridge.js` for exactly this job, and the type
        // predicate removes the `as Record<string, unknown>` cast an inline test needs to index.
        isRecord(value) &&
        typeof value[STATE_SCHEMA_VERSION_KEY] === "number" &&
        // `data` MUST BE PRESENT, not merely typed — this is the clause that makes the TRUNCATED
        // WRITE in `seedState`'s contract degrade instead of silently "restoring". A well-formed
        // entry always carries it: `portState` builds the wrapper from a blob that has already been
        // through `sanitizePanelState`, which normalizes `undefined` to `null`, so `data` survives
        // `JSON.stringify` for every value a panel can store. Its ABSENCE therefore means the
        // document was cut short or hand-edited — the exact case the doc claims degrades, and which
        // a `schemaVersion`-only guard waved through as a restore of `undefined`.
        STATE_DATA_KEY in value
    );
}

/**
 * A panel editor-core renders ITSELF (M9 e06d, `content.type: "local"`).
 *
 * The factory is handed the panel's DOM slot and builds into it; it returns an optional disposer for
 * whatever it attached. That is the WHOLE seam — deliberately: a local panel is ordinary editor-core
 * code, and the moment this interface grew a lifecycle of its own it would become a second, weaker
 * panel model competing with the C++ one.
 */
export type LocalPanelFactory = (container: HTMLElement) => (() => void) | void;

/**
 * Mark one panel's DOM slot (editor-UX c3).
 *
 * TWO ATTRIBUTES, NOT ONE, and the split is the point. `data-panel-id` stays the KIND — it is what
 * every existing selector, smoke and stylesheet means by "the Problems panel", and re-pointing it at
 * an instance id would silently break each of them. `data-panel-instance` names the COPY, which is
 * the only way a test (or a human with devtools) can tell two viewports apart once a kind can exist
 * twice.
 */
function markPanelSlot(element: HTMLElement, panelId: string, instanceId: string): void {
    element.setAttribute("data-panel-id", panelId);
    element.setAttribute("data-panel-instance", instanceId);
}

/** A panel PanelHost is currently hosting — ONE live copy, keyed in `#panels` by its instance id. */
interface HostedPanel {
    readonly manifest: PanelManifest;
    /** This copy's id (`<panelId>#<n>`) — also the Dockview panel id and the DOM-id scope. */
    readonly instanceId: string;
    readonly renderer: PanelRenderer;
}

/** A hosted panel known to be a live third-party (`iframe`) one — see `PanelHost.#portPanel`. */
type HostedIframePanel = HostedPanel & { readonly renderer: IframePanelRenderer };

/**
 * The outcome of one `PanelHost.deliverToPackage` fan-out (M9 e13c-2).
 *
 * TWO NUMBERS, NOT ONE, because they answer different questions and only the first can be asserted in
 * a tier that cannot grant a port: `addressed` is WHO the host decided belongs to the package (the
 * discriminating artifact), `delivered` is how many of them actually took the message.
 */
export interface PackageDelivery {
    /** The panel ids the host addressed — `panelsForPackage`'s answer, verbatim. */
    readonly addressed: readonly string[];
    /** How many of those panels' ports accepted the push. 0 is ordinary (no port granted yet). */
    readonly delivered: number;
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

    constructor(panelId: string, instanceId: string, factory: LocalPanelFactory) {
        this.element = document.createElement("div");
        this.element.className = "ctx-panel-body";
        markPanelSlot(this.element, panelId, instanceId);
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
 * Build the theme-push target for one panel frame (M9 e13d) — the object `IframeThemeChannel` holds.
 *
 * ⚠ LAZY BY CONSTRUCTION: the window is resolved at POST time, never captured. A frame that is not in
 * a document has a `null` `contentWindow`, and the renderer registers BEFORE Dockview mounts its
 * element, so a captured value would be `null` forever and the panel would never be themed — a
 * failure that is total, silent, and indistinguishable from "the package ignores theme events".
 *
 * EXTRACTED FROM THE RENDERER SO THE LAZINESS IS TESTABLE. Inline, the only witnesses were a real
 * frame's real `contentWindow` and a channel post that `IframeThemeChannel.#post` swallows on the way
 * out — so eager capture was a mutation NO assertion could see, which is precisely the shape of a
 * non-vacuity claim that is itself vacuous. As a pure function of a resolver it is discriminated by
 * one DOM-free case (`panelstate.test.ts` § the target): resolve `null` first, then a recorder, and
 * an eager implementation posts to neither.
 */
export function makeIframeThemeTarget(resolveWindow: () => Window | null): IframeMessageTarget {
    return {
        postMessage: (message: unknown, targetOrigin: string): void => {
            resolveWindow()?.postMessage(message, targetOrigin);
        },
    };
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
 * ⚠ EXACTLY ONE PORT, AND THIS CLASS OWNS ITS LIFETIME (M9 e13b-1). e13a-2 handed the frame NO
 * transport at all; the port now arrives through `PanelPortBridge` (panelport.ts), which is
 * constructed HERE — once per renderer, before `src` — and disposed with the renderer. The renderer's
 * one-way `#frame` latch is what makes "exactly one" structural rather than a rule: a second frame is
 * never built, so a second bridge is never constructed.
 *
 * WHAT THIS CLASS DELIBERATELY DOES NOT KNOW. It does not inspect the port, does not authenticate the
 * handshake, and does not decide what any verb answers — all of that is panelport.ts, for the same
 * reason `extpanel.ts` holds the entry grammar rather than this file: a security decision inside a
 * renderer class is a security decision with no unit tests. What lives here is the DOM and the
 * lifecycle, which is all a renderer should own.
 */
class IframePanelRenderer implements PanelRenderer {
    readonly element: HTMLElement;
    readonly #url: string;
    readonly #panelId: string;
    /** This copy's id (c3) — the frame's `data-panel-instance`, and its accessible title. */
    readonly #instanceId: string;
    /** The `context-ext://<packageId>` authority this panel belongs to (M9 e13c-2 routes on it). */
    readonly #packageId: string;
    readonly #verbs: PanelVerbTable;
    readonly #themeChannel: PanelThemeChannel | undefined;
    #themeTarget: IframeMessageTarget | undefined;
    #frame: HTMLIFrameElement | undefined;
    #bridge: PanelPortBridge | undefined;
    #suspended = false;
    /**
     * THIS PANEL's opaque state blob (M9 e13d). `null` until the panel stores one or a persisted blob
     * seeds it.
     *
     * HELD ON THE RENDERER, whose lifetime IS the panel's — the same lifetime a `uitree` panel's C++
     * model has, which is what makes this the port-side ANALOGUE of that model rather than a cache
     * beside it. It follows that a closed panel's blob is gone from memory exactly when the panel is,
     * and that what survives is what `LayoutPersistence` already published.
     */
    #state: unknown = null;
    /**
     * Has the PANEL itself written state? A persisted seed never overwrites a live write.
     *
     * THE ORDERING THIS CLOSES. `LayoutPersistence.restore()` seeds after `PanelHost.start()` has
     * mounted the frame, while the panel's first `bridge.state.set` can only arrive after its
     * document has loaded over `context-ext://` AND completed the port handshake — so the seed wins
     * by a wide margin in every real boot. "A wide margin" is not an invariant, though, and the
     * failure it would leave is silent and total: a seed landing late would overwrite what the user
     * is already looking at with yesterday's blob. This flag makes the outcome ORDER-INDEPENDENT
     * instead of merely likely, which is the difference between a race that cannot bite and one that
     * has not yet.
     */
    #stateFromPanel = false;

    constructor(
        panelId: string,
        instanceId: string,
        url: string,
        packageId: string,
        verbs?: PanelVerbBinder,
        themeChannel?: PanelThemeChannel,
    ) {
        this.#panelId = panelId;
        this.#instanceId = instanceId;
        this.#url = url;
        // CARRIED, NOT RE-DERIVED FROM `#url` (M9 e13c-2). `parseExtPanelEntry` already validated the
        // `context-ext://<packageId>` authority, and a second parse here would be a second notion of
        // "which package is this" — one the daemon fan-out routes EVENTS on. Two spellings of a
        // package id are two mailboxes, which is exactly the drift `PackageSessionHost::forward`
        // validates against on its own side.
        this.#packageId = packageId;
        this.#themeChannel = themeChannel;
        // THE VERB TABLE IS BUILT HERE, IN THE CONSTRUCTOR, and it is handed a `request` closure that
        // resolves `this.#bridge` LAZILY (through the method below, so there is ONE refusal for a
        // portless panel rather than two spellings of it). The knot it unties is real: the table's
        // `commands.invoke` dispatch needs the bridge, and the bridge needs the table. Deferring
        // through the field means the only way to observe the `undefined` window is to fire a verb
        // before `refresh()` ran — and a verb can only ARRIVE on a port that does not exist yet.
        this.#verbs = verbs?.(
            (verb: string, params?: unknown): Promise<PanelBridgeReply> => this.request(verb, params),
            {
                read: (): unknown => this.#state,
                write: (value: unknown): void => {
                    this.#state = value;
                    this.#stateFromPanel = true;
                },
            },
        ) ?? { verbs: new Map<string, PanelVerbHandler>(), dispose: (): void => {} };
        this.element = document.createElement("div");
        this.element.className = `ctx-panel-body ${IFRAME_PANEL_CLASS}`;
        markPanelSlot(this.element, panelId, instanceId);
    }

    get suspended(): boolean {
        return this.#suspended;
    }

    /** The package this panel belongs to — the identity the daemon fan-out routes on (M9 e13c-2). */
    get packageId(): string {
        return this.#packageId;
    }

    /** This panel's current blob (M9 e13d), or `null` when it has none to persist. */
    get stateBlob(): unknown {
        return this.#state;
    }

    /**
     * Apply a PERSISTED blob to this panel's store (M9 e13d) — the reload half of the round trip.
     * `true` when the panel now holds usable state, `false` when it was degraded to its defaults.
     *
     * DEGRADES, NEVER THROWS, and mirrors the C++ `restore_panel_state` contract clause for clause so
     * `LayoutPersistence` needs no second notion of what a bad blob is: a wrong `schemaVersion` is a
     * package that changed its own state shape across versions, and giving it yesterday's data would
     * hand a package a shape its current code does not parse — the exact bug the version exists to
     * prevent. A structurally unusable blob (a hand-edited editor-state file, a truncated write)
     * degrades the same way.
     *
     * The blob is RE-SANITIZED rather than trusted because it arrives from a FILE, not from the port:
     * `.editor/editor-state.json` is editable by a human and by any tool, and a blob restored past
     * the bound would be re-published at the next layout change — an amplification loop seeded from
     * disk. One gate, both directions (panelverbs.ts § `sanitizePanelState`).
     */
    seedState(persisted: unknown, schemaVersion: number): boolean {
        if (this.#stateFromPanel) {
            // The panel already wrote state, so it has restored itself (or moved on). Reported as
            // restored because the panel IS holding state — see `#stateFromPanel` for why the seed
            // yields rather than clobbering it.
            return true;
        }
        if (!isPersistedPanelState(persisted) || persisted.schemaVersion !== schemaVersion) {
            return false;
        }
        const verdict = sanitizePanelState(persisted.data);
        if (verdict.diagnostic !== "") {
            return false;
        }
        this.#state = verdict.state;
        return true;
    }

    /**
     * Issue a bridge request on this panel's port (M9 e13b-2) — the HOST -> PANEL direction.
     *
     * Exposed so `PanelHost.portRequest` can route a panel-manifest command to the PACKAGE instead of
     * to a `panel.command` the Shell has no C++ model to answer. A refusal is a REPLY here, exactly as
     * it is on `PanelPortBridge.request`, so no caller has to catch.
     */
    request(verb: string, params?: unknown): Promise<PanelBridgeReply> {
        const bridge = this.#bridge;
        return bridge === undefined
            ? Promise.resolve({
                  ok: false,
                  error: {
                      code: PANEL_BRIDGE_REFUSALS.portUnavailable,
                      verb,
                      message: "this panel has no port",
                  },
              })
            : bridge.request(verb, params);
    }

    /**
     * Push one ONE-WAY FACT into this panel's frame (M9 e13c-2) — `true` when it was posted.
     *
     * The delivery half of the daemon fan-out: `PackageEventPump` drains the Shell's bounded buffer
     * and calls this for every live port of the package. `false` (no port yet, or a revoked one) is an
     * ORDINARY outcome, not an error — a panel whose document has not finished handshaking simply has
     * not started receiving, and the fact was already counted as dropped-or-kept by the Shell's buffer,
     * which is the one place that accounting belongs.
     */
    deliver(verb: string, params?: unknown): boolean {
        return this.#bridge?.deliver(verb, params) ?? false;
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
        // TITLED BY THE INSTANCE (c3): a screen reader announcing "Tilemap Painter" twice for two
        // copies of one package panel tells the user nothing about which one focus moved into.
        frame.setAttribute("title", this.#instanceId);
        markPanelSlot(frame, this.#panelId, this.#instanceId);
        // A literal, like `ctx-panel-body` in the three sibling renderers: `app.css` carries the
        // matching rule and CSS cannot import a TS constant, so naming it here would buy a symbol on
        // editor-core's public surface and no enforcement. `IFRAME_PANEL_CLASS` is a constant only
        // because another MODULE (the T1 tier) selects on it.
        frame.className = "ctx-panel-frame-element";
        // THE PORT BRIDGE, BEFORE `src` — for the SAME reason the sandbox attribute is set before it,
        // and it is not merely symmetry. The Shell splices the port bootstrap in as the panel
        // document's FIRST script, so the handshake can be posted before this method's next statement
        // has run; a bridge constructed after `src` would miss the one grant a frame ever offers and
        // the panel would come up portless with nothing naming the cause. `PanelPortBridge`'s
        // constructor attaches both listeners, which is why constructing it IS the installation.
        this.#bridge = new PanelPortBridge({
            frame,
            panelId: this.#panelId,
            // e13b-2's verbs, or the empty table (= e13b-1's deny-all) when the host wired none.
            verbs: this.#verbs.verbs,
        });
        // THE THEME PUSH TARGET (M9 e13d), registered with the frame — the wiring that makes
        // `IframeThemeChannel` live. Every theme SWITCH from here on re-posts into this frame, which
        // is the "re-tokened on theme change" half of the task.
        //
        // ⚠ WHAT REGISTERING HERE DOES NOT BUY: the channel's LATE-REGISTRATION REPLAY. `register`
        // does post the current theme immediately, but this method runs before Dockview has mounted
        // `element`, so at that instant `frame.contentWindow` is `null` and the replay resolves to
        // nothing (the target is lazy — see `makeIframeThemeTarget`). A mid-session mount is
        // therefore themed by the PULL (`bridge.theme.tokens`), not by the replay, which is the
        // concrete reason the pull exists at all rather than a redundancy beside the push
        // (theme.ts § `last`). Registering early is still right: it is the one place holding the
        // frame, and it costs a no-op post rather than a missed switch.
        //
        // ⚠ AND IT DELIBERATELY DOES NOT FOLLOW THE PORT'S REVOCATION. A `WindowProxy` is stable
        // across same-slot navigations (panelport.ts § the file header), so a package that
        // re-navigates its own frame keeps receiving pushes after its PORT has been revoked. That is
        // correct FOR THIS PAYLOAD AND ONLY FOR IT: the theme is one public, identical value for
        // every panel in the window, so the second document learns nothing it could not have asked
        // for by being mounted legitimately. State never travels this way for exactly the reason
        // this sentence has to be written (panelverbs.ts § the cross-package property).
        if (this.#themeChannel !== undefined) {
            const target = makeIframeThemeTarget((): Window | null => frame.contentWindow);
            this.#themeTarget = target;
            this.#themeChannel.register(target);
        }
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
        // THE BRIDGE FIRST, AND BEFORE THE ELEMENT GOES. Disposing it closes the port and detaches the
        // window `message` listener; doing it after `replaceChildren` would leave a listener alive for
        // a frame that is already gone, and — because a detached frame's `contentWindow` becomes
        // `null` — the bridge's own "is this my frame?" check would start comparing `null` against the
        // `event.source` of every OTHER panel's messages. `PanelPortBridge.dispose` is idempotent, so
        // a second dispose (or one after a revocation) is a no-op.
        this.#bridge?.dispose();
        // THEN THE THEME TARGET. The channel outlives this renderer exactly as the command registry
        // does below, so a target left registered is a closure holding a dead frame that every future
        // theme switch posts into — a leak whose only symptom is a slowly growing broadcast fan-out.
        // Its `postMessage` would not throw either (`contentWindow` is `null` on a detached frame, and
        // the optional call swallows it), so nothing would ever surface it.
        if (this.#themeTarget !== undefined) {
            this.#themeChannel?.unregister(this.#themeTarget);
            this.#themeTarget = undefined;
        }
        // THEN THE COMMANDS. The registry outlives this renderer, so anything the panel registered
        // over `bridge.commands.register` has to be withdrawn HERE or it outlives the panel: a ghost
        // palette row dispatching over the port just closed above, and — because a reopened panel
        // builds a FRESH verb table — an orphan that beats the panel's own re-registration under
        // incumbent-wins, permanently costing it that command for the life of the window. After the
        // bridge, so no in-flight invoke can re-enter a half-withdrawn table.
        this.#verbs.dispose();
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

    constructor(panelId: string, instanceId: string) {
        this.element = document.createElement("div");
        this.element.className = "ctx-panel-body";
        markPanelSlot(this.element, panelId, instanceId);
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

    constructor(panelId: string, instanceId: string, client: PanelClient, gestures: boolean) {
        this.element = document.createElement("div");
        this.element.className = "ctx-panel-body";
        // The panel's DOM slot is a labelled landmark in its own right, so a screen reader announces
        // which panel focus moved into when the user tabs between docked groups.
        markPanelSlot(this.element, panelId, instanceId);
        this.#runtime = new HydrationRuntime(this.element, client, panelId, {
            gestures,
            // THE COPY, and therefore also this runtime's DOM-id scope — two instances of one kind
            // in one document would otherwise both mint `<panelId>::root` (hydration.ts states it).
            instanceId,
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
    /**
     * Builds the bridge verb table for each THIRD-PARTY (`iframe`) panel's port (M9 e13b-2).
     *
     * OPTIONAL, and its absence is a real configuration rather than an oversight: a host that wires no
     * factory gives every package panel the empty table, i.e. e13b-1's deny-all, which is exactly what
     * every test that does not opt in should see. `boot.ts` supplies `makePanelBridgeVerbs`.
     */
    readonly panelVerbs?: PanelVerbFactory;
    /**
     * The theme-token push channel every `iframe` panel's frame registers with (M9 e13d).
     *
     * OPTIONAL, and its absence is a real configuration: a host with no theme engine (a T1 case, a
     * harness) pushes no tokens and package panels simply keep whatever their own stylesheet says.
     * `boot.ts` supplies `ThemeEngine.iframes`.
     */
    readonly themeChannel?: PanelThemeChannel;
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

/**
 * What one `open` / `openInstance` did (editor-UX c3, design 04 §3's open semantics).
 *
 * ⚠ A RESULT OBJECT, NOT A BOOLEAN, and that is the whole D6 correction. `open` used to answer
 * `false` for "already open", which is the same answer it gives for "the docking root is down" and
 * for "this build cannot mount it" — three different facts collapsed onto one, so a second open of a
 * `singleton` was indistinguishable from a failure and no caller could focus the live copy instead.
 * A `singleton` re-open now reports `focused` WITH the instance id, which is an honest success.
 */
export type PanelOpenOutcome = "opened" | "focused" | "refused";

/** The outcome of one open, plus the copy it names and — on a refusal — why. */
export interface PanelOpenResult {
    readonly outcome: PanelOpenOutcome;
    /** The live copy this open opened or focused; `""` when refused. */
    readonly instanceId: string;
    /** Empty unless refused. NAMES the limit when a mode refused it (design 04 §3). */
    readonly diagnostic: string;
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
    readonly #panelVerbs: PanelVerbFactory | undefined;
    readonly #themeChannel: PanelThemeChannel | undefined;
    /**
     * The live copies, keyed by INSTANCE id (c3) — not by panel id, which is the rekeying D6's
     * imperative half is. Insertion order is mount order, which `mounted` and `panelsForPackage`
     * both rely on.
     */
    readonly #panels = new Map<string, HostedPanel>();
    /**
     * The next ordinal each KIND will mint (c3). Kept here rather than derived from `#panels`,
     * because a derived counter reuses the ordinal of a CLOSED copy: close `p#2` of three, mint
     * again, and a `size + 1` rule hands the new copy `p#3`, which is live. Monotonic per kind, and
     * advanced past any id a restore or a rehome named — see `#reserveInstanceId`.
     */
    readonly #ordinals = new Map<string, number>();
    /**
     * The panel revision each mounted panel was last REFRESHED for (M9 e09e-3) — `pollRevisions`'
     * whole state, and deliberately not the roster's own numbers: what matters is what this host has
     * already acted on, not what the Shell last said.
     *
     * Empty at start, so the first poll refreshes every mounted panel exactly once. That is not
     * waste, it is the fix for a REAL race: `UitreePanelRenderer.init` renders the moment Dockview
     * materialises the slot, which can be BEFORE the panel's C++ model has any data (an Inspector
     * whose selection arrives a frame later), and until this driver existed nothing ever asked again.
     */
    readonly #revisions = new Map<string, number>();
    #api: DockviewApi | null = null;
    #roster: PanelRoster | null = null;

    constructor(options: PanelHostOptions) {
        this.#container = options.container;
        this.#client = options.client;
        this.#dockview = options.dockview ?? detectDockview();
        this.#localPanels = options.localPanels ?? new Map<string, LocalPanelFactory>();
        this.#panelVerbs = options.panelVerbs;
        this.#themeChannel = options.themeChannel;
    }

    /**
     * The persisted-shape state blob for a live third-party (`iframe`) panel (M9 e13d).
     *
     * THREE ANSWERS, because the caller has three things to do — and they are in the SIGNATURE, not
     * only in this paragraph: an `unknown` return would let a caller collapse two of them (exactly
     * the mutation the tests plant) with no complaint from the compiler. `undefined` = NOT a port panel, so
     * take the C++ `panel.state.get` route. `null` = a port panel holding nothing, so publish neither
     * an entry nor a round trip. Otherwise the D6 wrapper to publish. Collapsing the first two onto
     * `undefined` reads as "ask the Shell" for a panel the Shell has no model for, which is a
     * guaranteed refusal bought at the price of a bridge round trip (see the `null` arm below).
     *
     * WHY THE HOST ANSWERS THIS AT ALL. An iframe panel has no C++ model, so `panel.state.get` has
     * nothing to ask (`panel_host.cpp` answers for models only). Its state lives on this side of the
     * bridge, and this is the ONE place holding both the renderer that owns the blob and the manifest
     * that names its `schemaVersion`.
     */
    portState(panelId: string): PersistedPanelState | null | undefined {
        const hosted = this.#portPanel(panelId);
        if (hosted === undefined) {
            return undefined; // not a port panel — the caller's signal to take the C++ route
        }
        const data = hosted.renderer.stateBlob;
        if (data === null || data === undefined) {
            // NULL, NOT `undefined` — a port panel that simply holds nothing. The two answers are
            // DIFFERENT INSTRUCTIONS to `LayoutPersistence.#publish`: `undefined` means "ask the
            // Shell", and answering it here would send a `panel.state.get` for a panel `panel_host.cpp`
            // can only refuse (an iframe panel has no model, so it is `hosted:false`) — one
            // guaranteed-refused round trip per stateless package panel per publish, worst on the
            // `pagehide` flush the gather is ordered to keep short. `null` means "port panel, nothing
            // to persist": skip it and publish neither entry nor round trip.
            return null;
        }
        return {
            [STATE_SCHEMA_VERSION_KEY]: hosted.manifest.schemaVersion,
            [STATE_DATA_KEY]: data,
        } satisfies PersistedPanelState;
    }

    /**
     * The hosted entry for a live third-party (`iframe`) panel, or `undefined` for anything else.
     *
     * ONE PREDICATE FOR THE THREE PORT ACCESSORS. `portState`, `seedPortState` and `portRequest` must
     * agree on what a port panel IS — `LayoutPersistence` routes a whole persisted document on that
     * one answer, and the two routes are exclusive only for as long as they agree — so the test is
     * written once rather than three times in three spellings.
     */
    #portPanel(panelId: string): HostedIframePanel | undefined {
        const hosted = this.#panels.get(panelId);
        return hosted !== undefined && hosted.renderer instanceof IframePanelRenderer
            ? (hosted as HostedIframePanel)
            : undefined;
    }

    /**
     * Seed a live third-party (`iframe`) panel's blob from persisted state (M9 e13d).
     *
     * `undefined` means "not a port panel" — the signal `LayoutPersistence.restore` uses to fall
     * through to the C++ `panel.state.set` route. The two routes are EXCLUSIVE by construction (a
     * panel is either an iframe or it is not), so no blob is ever applied twice. `true`/`false` are
     * the restored / degraded verdict, matching what the C++ route reports.
     */
    seedPortState(panelId: string, persisted: unknown): boolean | undefined {
        const hosted = this.#portPanel(panelId);
        return hosted?.renderer.seedState(persisted, hosted.manifest.schemaVersion);
    }

    /**
     * Issue a bridge request on a MOUNTED third-party panel's port (M9 e13b-2).
     *
     * `undefined` for anything that is not a live `iframe` panel — which is the signal a caller uses to
     * fall back to the `panel.command` route a `uitree` panel takes. Kept as a lookup rather than a
     * dispatch method so `boot.ts` can decide the ROUTE while this class stays the thing that owns the
     * renderers.
     */
    portRequest(panelId: string): PanelPortRequest | undefined {
        const renderer = this.#portPanel(panelId)?.renderer;
        return renderer === undefined
            ? undefined
            : (verb: string, params?: unknown): Promise<PanelBridgeReply> =>
                  renderer.request(verb, params);
    }

    /**
     * The packages with at least one MOUNTED port panel, deduplicated and in mount order (M9 e13c-2).
     *
     * ⚠ DEDUPLICATED IS THE LOAD-BEARING WORD. The Shell buffers per PACKAGE (its daemon session is
     * pooled per package), and `panel.events.poll` DRAINS — so polling once per PANEL would give a
     * package's first panel every event and its second panel nothing. That bug is invisible until a
     * user opens a package's second panel, which is why the deduplication lives here, at the one place
     * that can see the panel→package mapping, rather than in the pump's loop.
     */
    packagesWithPorts(): readonly string[] {
        const seen = new Set<string>();
        for (const hosted of this.#panels.values()) {
            if (hosted.renderer instanceof IframePanelRenderer) {
                seen.add(hosted.renderer.packageId);
            }
        }
        return [...seen];
    }

    /**
     * The MOUNTED port panels belonging to `packageId`, in mount order (M9 e13c-2).
     *
     * ⚠ EXTRACTED FROM `deliverToPackage` RATHER THAN INLINED IN IT, AND A PLANT IS WHY. The delivery
     * count alone cannot pin the ADDRESSING: `deliver` answers `false` for any panel whose port has
     * not been granted, so in every tier that cannot load a `context-ext://` document the count is 0
     * for the right package AND for the wrong one — a plant that dropped the `packageId` comparison
     * entirely came back GREEN against a count-only assertion (MEASURED, this task). Naming the
     * addressed panels is the POSITIVE artifact that discriminates, and `deliverToPackage` is written
     * over this one predicate so the two can never disagree about who belongs to a package.
     */
    panelsForPackage(packageId: string): readonly string[] {
        const ids: string[] = [];
        for (const [panelId, hosted] of this.#panels) {
            if (
                hosted.renderer instanceof IframePanelRenderer &&
                hosted.renderer.packageId === packageId
            ) {
                ids.push(panelId);
            }
        }
        return ids;
    }

    /**
     * Push one ONE-WAY FACT into EVERY mounted port panel of `packageId` (M9 e13c-2).
     *
     * ⚠ RETURNS THE ADDRESSING, NOT ONLY THE COUNT, AND THAT IS WHAT MAKES IT TESTABLE. `delivered`
     * alone cannot discriminate: `deliver` answers `false` for any panel whose port is not granted, so
     * in every tier that cannot load a `context-ext://` document the count is 0 for the RIGHT package
     * and the WRONG one alike — replacing the loop's source with `this.#panels.keys()` (i.e. fanning a
     * package's events out to EVERY panel) leaves a count-only assertion green. `addressed` is the
     * POSITIVE artifact that reddens it, which is the same reason `panelsForPackage` exists.
     *
     * A 0 `delivered` is an ordinary answer: a package whose panel has not finished its port handshake
     * yet is not an error, and the Shell's buffer — not this host — is where "was it kept or dropped"
     * is decided.
     */
    deliverToPackage(packageId: string, verb: string, params?: unknown): PackageDelivery {
        const addressed = this.panelsForPackage(packageId);
        let delivered = 0;
        for (const panelId of addressed) {
            // `#portPanel` is the ONE port-panel predicate this class declares (see its comment):
            // re-spelling `instanceof` here is what lets the fan-out route and the LayoutPersistence
            // route drift about what a port panel IS.
            if (this.#portPanel(panelId)?.renderer.deliver(verb, params) === true) {
                delivered += 1;
            }
        }
        return { addressed, delivered };
    }

    get api(): DockviewApi | null {
        return this.#api;
    }

    get roster(): PanelRoster | null {
        return this.#roster;
    }

    /**
     * The roster manifest for `panelId`, or undefined when this build's roster does not name it —
     * the ONE kind-to-manifest lookup `openInstance`, `manifestForInstance` and boot.ts's seeded-title
     * path all share, so an id-matching change lands in every caller at once.
     */
    manifest(panelId: string): PanelManifest | undefined {
        return this.#roster?.panels.find((entry) => entry.id === panelId);
    }

    /**
     * The manifest behind an INSTANCE id (c3) — the lookup every caller holding a mounted id needs.
     *
     * Reads the live entry first and falls back to decomposing the id, so it answers for a copy this
     * host does not (yet) hold: `#create` is driven by Dockview during a layout restore BEFORE any
     * entry exists, and that path has nothing but the id.
     */
    manifestForInstance(instanceId: string): PanelManifest | undefined {
        const hosted = this.#panels.get(instanceId);
        return hosted?.manifest ?? this.manifest(panelIdOfInstance(instanceId));
    }

    /** The KIND an instance id names — the shared decomposition, re-exported for callers. */
    panelIdOf(instanceId: string): string {
        return this.#panels.get(instanceId)?.manifest.id ?? panelIdOfInstance(instanceId);
    }

    /** The live copies of one kind, in mount order (c3). */
    instancesOf(panelId: string): readonly string[] {
        const ids: string[] = [];
        for (const [instanceId, hosted] of this.#panels) {
            if (hosted.manifest.id === panelId) {
                ids.push(instanceId);
            }
        }
        return ids;
    }

    /** The INSTANCE ids currently mounted, in mount order. */
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
            if (this.open(manifest).outcome === "opened") {
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
     * Open one copy of a panel, HONOURING ITS DECLARED INSTANCE MODE (editor-UX c3, design 04 §3).
     *
     * `singleton` — a second open FOCUSES the live copy and reports `focused`, which is a success:
     * before c3 this method refused a second open of EVERY panel whatever its mode declared, which
     * is precisely why `dock.singleton` was decorative (there was no path on which it could differ).
     * `limited` — opens up to `instances.max`; the next is `refused` with the limit NAMED in the
     * diagnostic. `unlimited` — mints a new copy every time.
     *
     * `requestedInstanceId` is the RESTORE / REHOME channel: a caller replaying a persisted
     * arrangement asks for the exact ids it saved. An id already live reports `focused` for it,
     * never a silently different copy.
     */
    open(manifest: PanelManifest, requestedInstanceId?: string): PanelOpenResult {
        if (this.#api === null) {
            return refusal("the docking root is not up");
        }
        // THE SINK GATE, APPLIED AT THE ONE CHOKEPOINT rather than in `start`'s loop (M9 e13a-2).
        // `start` is not the only caller: `openInstance` (e10b's tear-out seed path) reaches here with a
        // roster manifest and no content-type check of its own, so a gate that lived only in the
        // loop above would be bypassed by a seeded window — and for an `iframe` manifest that means
        // a third-party entry URL routed into `#create`. Refusing here is what makes the gate a
        // property of the class instead of a property of one code path.
        if (!this.#mountable(manifest)) {
            return refusal(`this build cannot mount '${manifest.id}'`);
        }
        if (requestedInstanceId !== undefined && this.#panels.has(requestedInstanceId)) {
            // ALREADY OPEN, AND THAT IS AN ANSWER. A restore replaying an id it already replayed gets
            // that copy back rather than a refusal it would have to tell apart from a real one.
            return this.#focus(requestedInstanceId);
        }
        const live = this.instancesOf(manifest.id);
        const admission = admits(manifest, live.length);
        if (admission !== "") {
            // A `singleton` whose copy is already open FOCUSES it — the one behaviour the old
            // boolean could not express. Every other refusal (a `limited` ceiling, a drifted mode)
            // is reported with the limit named.
            //
            // ⚠ ONLY WHEN NO SPECIFIC COPY WAS ASKED FOR. A caller naming an id it does not already
            // hold — a restore replaying a saved arrangement — wants THAT copy or an honest refusal;
            // answering with a different live one would attach the restored state to the wrong panel,
            // silently, which is worse than not restoring it. (The already-live case returned
            // `focused` above, before this branch.)
            const first = live[0];
            if (
                requestedInstanceId === undefined &&
                manifest.instances.mode === "singleton" &&
                first !== undefined
            ) {
                return this.#focus(first);
            }
            return refusal(admission);
        }
        const instanceId = this.#reserveInstanceId(manifest.id, requestedInstanceId);
        const previous = this.mounted[this.mounted.length - 1];
        this.#api.addPanel({
            // THE DOCKVIEW PANEL ID IS THE INSTANCE ID (c3). Dockview keys its whole arrangement —
            // and the blob `captureLayout` persists — on this, so two copies of one kind need two
            // ids here or the second `addPanel` silently replaces the first.
            id: instanceId,
            component: componentFor(manifest),
            title: manifest.title,
            ...rendererFor(manifest),
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
        return this.#panels.has(instanceId)
            ? { outcome: "opened", instanceId, diagnostic: "" }
            : refusal(`the docking root did not create '${instanceId}'`);
    }

    /**
     * Open one copy of a panel BY KIND, looked up from the roster (M9 e10b, rekeyed by c3 — this is
     * `openById` under its instance-aware name). The seed-open path: a torn-out or rehomed panel
     * arrives as a panel id + a D6 state blob, the target window opens a copy here, and the caller
     * restores the blob over `panel.state.set` addressed to the id this returns.
     *
     * ⚠ A REHOME MINTS A FRESH COPY rather than carrying the source window's instance id across.
     * Instance ordinals are per WINDOW (each window runs its own editor-core with its own counters),
     * so a moved `p#2` landing in a window that already holds `p#2` would collide with an unrelated
     * panel. What travels is the KIND and the STATE, which is what "moves an instance" means once the
     * copy is on the other side of an OS window boundary.
     */
    openInstance(panelId: string, requestedInstanceId?: string): PanelOpenResult {
        const manifest = this.manifest(panelId);
        return manifest === undefined
            ? refusal(`'${panelId}' is not in this build's roster`)
            : this.open(manifest, requestedInstanceId);
    }

    /**
     * Focus a live copy — the `singleton` second-open answer, and the only place Dockview's
     * per-panel activation is used.
     *
     * `setActive` is OPTIONAL on the declared handle (dockview.ts) so a harness need not implement
     * it; the outcome is `focused` either way, because "which copy answers this open" is the fact the
     * caller acts on and a host that cannot raise a tab has still answered it correctly.
     */
    #focus(instanceId: string): PanelOpenResult {
        this.#api?.getPanel(instanceId)?.api?.setActive();
        return { outcome: "focused", instanceId, diagnostic: "" };
    }

    /**
     * Mint (or accept) an instance id and keep the kind's counter ahead of it.
     *
     * The counter must move past an id that came from OUTSIDE — a persisted arrangement, a rehome —
     * or the next mint could collide with a live copy and hand it Dockview's existing slot.
     */
    #reserveInstanceId(panelId: string, requestedInstanceId?: string): string {
        if (requestedInstanceId !== undefined && requestedInstanceId !== "") {
            this.#noteInstanceId(panelId, requestedInstanceId);
            return requestedInstanceId;
        }
        let ordinal = this.#ordinals.get(panelId) ?? 1;
        // Skip anything already live. The counter alone is enough in every ordinary sequence; this
        // closes the case where a restore reserved a HIGHER id and a lower one is still open.
        while (this.#panels.has(makeInstanceId(panelId, ordinal))) {
            ordinal += 1;
        }
        this.#ordinals.set(panelId, ordinal + 1);
        return makeInstanceId(panelId, ordinal);
    }

    /** Advance a kind's mint counter past an id minted elsewhere. A non-numeric tail is ignored. */
    #noteInstanceId(panelId: string, instanceId: string): void {
        const prefix = `${panelId}${PANEL_INSTANCE_SEPARATOR}`;
        if (!instanceId.startsWith(prefix)) {
            return;
        }
        const tail = instanceId.slice(prefix.length);
        if (tail === "" || !/^[0-9]+$/.test(tail)) {
            return;
        }
        const ordinal = Number(tail);
        if (Number.isSafeInteger(ordinal) && ordinal + 1 > (this.#ordinals.get(panelId) ?? 1)) {
            this.#ordinals.set(panelId, ordinal + 1);
        }
    }

    /**
     * Float a mounted panel's group inside THIS window (M9 e10b) — the LOUD degradation home when a
     * secondary-window create fails (03 §7). The panel does not silently stay where it was: it becomes
     * a floating Dockview group, which `dockview-core` renders with an inline `position:absolute` the
     * user (and the live smoke) can SEE. Returns false when the panel is not mounted or the docking
     * root is down (nothing to float), so the caller can report the degrade honestly.
     */
    floatPanel(instanceId: string): boolean {
        const panel = this.#api?.getPanel(instanceId);
        if (this.#api === null || panel === undefined) {
            return false;
        }
        this.#api.addFloatingGroup(panel);
        return true;
    }

    /**
     * Close one COPY, disposing its hydration runtime and RELEASING its Shell-side model (c3).
     *
     * ⚠ THE RELEASE IS THE HALF THAT IS EASY TO OMIT, and omitting it is silent: the Shell's instance
     * table would only ever grow, so a `limited` panel would exhaust its ceiling after `max` opens
     * over the whole session and the next open would be refused for a copy the user closed an hour
     * ago. Fire-and-forget on purpose — a released model is not something the DOM waits on, and
     * blocking a close on a round trip would make tearing a panel down feel slow. Only `uitree`
     * panels are released: they are exactly the kinds with a C++ model, and `local`/`iframe` panels
     * would spend a guaranteed-refused round trip per close (the same reasoning `portState`'s three
     * answers turn on).
     */
    close(instanceId: string): boolean {
        const panel = this.#api?.getPanel(instanceId);
        if (this.#api === null || panel === undefined) {
            return false;
        }
        const hosted = this.#panels.get(instanceId);
        this.#api.removePanel(panel);
        hosted?.renderer.dispose();
        if (hosted !== undefined && hosted.manifest.contentType === "uitree") {
            void this.#client.closeInstance(hosted.manifest.id, instanceId);
        }
        // FORGET THE REVISION TOO (e09e-3). A reopened panel gets a FRESH renderer whose mounted
        // revision is `-1`, so a remembered number here would make `pollRevisions` skip it until the
        // model happened to move again — the reopened panel would sit on whatever its own `init`
        // render caught, which is the very race this driver exists to close.
        this.#revisions.delete(instanceId);
        return this.#panels.delete(instanceId);
    }

    /**
     * THE MODEL-CHANGE REFRESH DRIVER (M9 e09e-3): re-render exactly the mounted panels whose C++
     * model has MOVED since this host last rendered them. Returns how many it refreshed.
     *
     * WHY THIS EXISTS AT ALL — the gap it closes was a structural hole in design 05 §8's tail. Before
     * it, re-renders were driven only by LOCAL interaction: `UitreePanelRenderer.init` on mount,
     * `onShow` on becoming visible, and `onDispatched` after a command this window sent. So a fact
     * arriving from the DAEMON — a diagnostic landing in the Problems feed, or the
     * `derivation.settled` that carries another window's edit — reached the C++ model and moved its
     * revision, and the human saw NOTHING until they happened to click. §8's "all subscribed clients
     * (window 1, window 2, CLI, agents) update" was therefore false of the only surface a human
     * looks at, and the effect was worst in a SECONDARY window, which sends no commands at all.
     *
     * WHY IT POLLS `panel.list` RATHER THAN RE-RENDERING EVERYTHING. `panel.render` makes the Shell
     * BUILD the panel (`entry->provider.build()` + `render_html`) on every call — for a Scene tree
     * over a large project that is real work, and a blind `refreshAll` on a tick would pay it for
     * every panel forever. `panel.list` builds nothing: the revision rides the ROSTER (panel_host.cpp
     * § `list`), which is why it is there. So the steady-state cost of this driver is ONE round trip
     * per tick no matter how many panels are open, and a `panel.render` + DOM patch only when a model
     * genuinely changed. That is the same bargain `HydrationRuntime.apply`'s revision no-op strikes
     * one layer down, moved to where it can also skip the round trip.
     *
     * A SUSPENDED (tabbed-away) panel is skipped AND NOT RECORDED, so it refreshes on the first poll
     * after `onShow` puts it back on screen rather than staying stale behind its own tab.
     *
     * NEVER REJECTS FOR A ROSTER IT CANNOT READ: an unreadable roster is `0`, exactly as everywhere
     * else in this class, because its caller is a `setInterval` tick in a renderer with no console.
     * That covers BOTH shapes the read can fail in — see the guard in the body for why the `await`
     * is not enough on its own. A genuine programming error still propagates, as it does from every
     * other method here.
     */
    async pollRevisions(): Promise<number> {
        if (this.#api === null || this.#panels.size === 0) {
            return 0;
        }
        // GUARDED, not merely awaited. `PanelClient.list` is the one panel verb that does NOT swallow
        // a transport failure the way `render` and `WindowClient.rehomed` do — it rethrows
        // `BridgeError` (panels.ts § render states the rule: a refusal is returned, a transport
        // failure throws) — and this method's only caller is a bare `void host.pollRevisions()` on
        // editor-core's 500 ms tick. An unguarded reject there is an unhandled rejection EVERY TICK,
        // in a renderer whose only diagnostic channel is a DOM attribute: precisely the outcome the
        // contract above exists to rule out, and unreachable by the `roster === null` check because
        // a throw never produces a value to test. A bridge that cannot answer is an ORDINARY state
        // here (a window tearing down, a daemon that went away), so report 0 refreshed and ask again
        // next tick — the same bargain `rehomed()` strikes when it reports `[]`.
        //
        // Folded onto the EXISTING `null` channel rather than given its own `return 0`: both mean
        // "no usable roster this tick", and one exit is one thing to keep correct. The guard stays
        // HERE rather than inside `PanelClient.list` deliberately — panels.ts documents the
        // distinction it draws on purpose ("A REFUSAL IS RETURNED, NOT THROWN ... A transport failure
        // still rejects — that is not an ordinary state"), and `list`'s other caller, `start`, wants
        // exactly that. This tick is the one place where it IS ordinary.
        const roster = await this.#client.list().catch((error: unknown): null => {
            if (error instanceof BridgeError) {
                return null;
            }
            throw error;
        });
        if (roster === null) {
            return 0;
        }
        // ⚠ KEYED BY INSTANCE, DRIVEN BY THE KIND'S REVISION (c3). The roster reports ONE revision
        // per kind — `panel.list` builds nothing, which is the whole reason this driver polls it
        // rather than re-rendering (see above) — so every live copy of a kind whose model moved is
        // refreshed. For a kind whose copies share one model (a `provide()` binding) that is exactly
        // right; for a factory-bound kind it can refresh a sibling whose own model did not move,
        // which costs one render and patches nothing, because `HydrationRuntime.apply` no-ops on an
        // unchanged revision one layer down. Paying that is deliberate: the alternative is a
        // per-instance revision on the wire, i.e. a `panel.list` whose size grows with the open set.
        const revisions = new Map<string, number>();
        for (const manifest of roster.panels) {
            revisions.set(manifest.id, manifest.revision);
        }
        let refreshed = 0;
        for (const [instanceId, hosted] of this.#panels) {
            const revision = revisions.get(hosted.manifest.id);
            if (revision === undefined || hosted.renderer.suspended) {
                continue;
            }
            if (this.#revisions.get(instanceId) === revision) {
                continue;
            }
            this.#revisions.set(instanceId, revision);
            hosted.renderer.refresh();
            refreshed += 1;
        }
        return refreshed;
    }

    /**
     * Re-render every mounted, non-suspended panel, unconditionally.
     *
     * The UNGATED sibling of `pollRevisions` above, which is the actual driver since M9 e09e-3. Kept
     * as the seam for a caller that must force a re-render without a revision having moved (a
     * post-restore repaint); nothing calls it today, and a tick MUST NOT — see `pollRevisions` on why
     * an unconditional refresh pays for a full Shell-side panel build per panel per tick.
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

    /**
     * Restore an arrangement Dockview produced. False when there is no docking root to restore
     * into, or when the blob is not an arrangement that could show anything: no `grid` record, or
     * no panels (issue #474 — a pre-fix Shell persisted `{}` for every fresh project, and an empty
     * arrangement is indistinguishable from that poisoned blob, so both degrade to the defaults
     * `start` opened rather than booting an empty window).
     *
     * NON-DESTRUCTIVE ON FAILURE. `fromJSON` CLEARS the live grid before parsing, so without the
     * rollback a corrupt blob destroys the defaults first and throws second — the caller's degrade
     * path then keeps a wiped dock while believing it kept the defaults. The error still propagates
     * for the caller's accounting; only the wipe is undone.
     */
    restoreLayout(state: unknown): boolean {
        if (this.#api === null || !isRecord(state) || !isRecord(state["grid"])) {
            return false;
        }
        const panels = state["panels"];
        if (!isRecord(panels) || Object.keys(panels).length === 0) {
            return false;
        }
        const previous = this.#api.toJSON();
        try {
            this.#api.fromJSON(state);
            // RECONCILE THE LIVE SET (c3). `fromJSON` clears the grid and re-creates every panel it
            // names, driving `#create` — which registers the restored copies — but nothing removes
            // the entries `start()` opened whose ids the arrangement does NOT name. Before instances
            // that was invisible: the ids were panel ids and a restore replayed the same ones, so
            // every entry was overwritten. A restored arrangement can now legitimately name
            // `p#2, p#3` where `start()` opened `p#1`, and a stale entry left behind is a renderer
            // Dockview no longer shows, still registered for theme pushes and still counted against
            // its kind's instance ceiling.
            for (const [instanceId, hosted] of [...this.#panels]) {
                if (this.#api.getPanel(instanceId) === undefined) {
                    hosted.renderer.dispose();
                    this.#panels.delete(instanceId);
                    this.#revisions.delete(instanceId);
                }
            }
        } catch (error) {
            try {
                this.#api.fromJSON(previous);
            } catch {
                // The rollback source is the arrangement that was LIVE a moment ago, so this is not
                // expected to throw; if it somehow does, the wipe already happened and there is
                // nothing better left to restore.
            }
            throw error;
        }
        return true;
    }

    /** Dispose every panel and the docking root. Idempotent. */
    dispose(): void {
        for (const panel of this.#panels.values()) {
            panel.renderer.dispose();
        }
        this.#panels.clear();
        this.#revisions.clear();
        // The mint counters go with the panels. A host that kept them would hand the FIRST copy of a
        // re-started editor an ordinal from the previous session, so a persisted `p#1` arrangement
        // would no longer match what a fresh `start()` opens.
        this.#ordinals.clear();
        this.#api?.dispose();
        this.#api = null;
    }

    /**
     * Dockview's `createComponent` callback: build the ONE renderer type for whichever panel it
     * asked for. The manifest is looked up from the roster rather than passed in, because Dockview
     * hands us only an id.
     */
    #create(instanceId: string): DockviewContentRenderer {
        // ⚠ DOCKVIEW HANDS US AN INSTANCE ID AND NOTHING ELSE (c3), including on the restore path,
        // where `fromJSON` re-creates every persisted panel BEFORE this host has registered any of
        // them. That is exactly why an instance id is composed rather than opaque: the KIND has to be
        // recoverable from the id alone or a restore cannot know which renderer to build.
        const manifest = this.manifestForInstance(instanceId);
        const renderer = this.#renderer(instanceId, manifest);
        if (manifest !== undefined) {
            // Keep the kind's mint counter ahead of an id Dockview restored, so the next open cannot
            // collide with a copy the arrangement just brought back.
            this.#noteInstanceId(manifest.id, instanceId);
            // DISPOSE THE ONE BEING REPLACED, IF THERE IS ONE. Dockview calls `init` and NOTHING
            // else on a content renderer (dockview.ts § the lifecycle note) — it never disposes ours
            // — so overwriting this map entry WITHOUT disposing would orphan the previous renderer
            // while every registration it made stayed live: a theme push target holding a dead frame
            // that each future switch posts into, a window `message` listener, and the panel's
            // registered commands. None of those surfaces a symptom, which is why the teardown is
            // written here rather than left to whoever notices.
            //
            // ⚠ HONEST SCOPE — NO KNOWN CALLER REACHES IT TODAY, and this note exists so the next
            // reader does not re-derive that. `#renderer`'s comment names `restoreLayout` →
            // `fromJSON` → `createComponent` as a real caller of the CONSTRUCTION path, which it is;
            // it is NOT evidence that Dockview re-creates a component for an id it is ALREADY
            // hosting. Measured against the pinned 7.0.2 bundle in the T1 tier (M9 e13d refine): a
            // `restoreLayout` of a captured layout, and of a DONOR host's layout into a host whose
            // `start()` had already opened the same panel — the real boot shape — each left the
            // theme channel holding exactly ONE target either way. So this line is a guard on an
            // unreached path, and DELIBERATELY ships without a test: a case asserting it would be
            // vacuous, and a vacuous case carrying a non-vacuity claim is worse than no case at all.
            // Kept because it costs one lookup on a path that runs once per panel creation and is
            // correct by construction — dropping an entry from `#panels` without disposing it can
            // never be right — not because a leak was observed.
            this.#panels.get(instanceId)?.renderer.dispose();
            this.#panels.set(instanceId, { manifest, instanceId, renderer });
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
    #renderer(instanceId: string, manifest: PanelManifest | undefined): PanelRenderer {
        // Narrowed through an `if` rather than `switch (manifest?.contentType)` so TS keeps the
        // narrowing inside each arm; the shape then matches `#mountable` and `componentFor`, which
        // switch on the same discriminant directly below.
        if (manifest === undefined) {
            return new UnavailablePanelRenderer(panelIdOfInstance(instanceId), instanceId);
        }
        const panelId = manifest.id;
        switch (manifest.contentType) {
            case "local": {
                // KEYED BY THE KIND, not the copy: `localPanels` says which of the manifest's panels
                // THIS bundle knows how to draw, which is a fact about the kind. A per-instance key
                // would need one factory registration per copy, i.e. a registry that grows as the
                // user opens panels.
                const localFactory = this.#localPanels.get(panelId);
                return localFactory === undefined
                    ? new UnavailablePanelRenderer(panelId, instanceId)
                    : new LocalPanelRenderer(panelId, instanceId, localFactory);
            }
            case "iframe": {
                // An unparseable entry is UNAVAILABLE, not an `about:blank` frame. Both spell "this
                // panel cannot be drawn", and one spelling is enough — the `about:blank` form also
                // contradicted `IframePanelRenderer`'s own invariant that a validated
                // `context-ext://…` URL is the ONLY string it accepts.
                const entry = parseExtPanelEntry(manifest.contentEntry);
                if (entry === null) {
                    return new UnavailablePanelRenderer(panelId, instanceId);
                }
                // Bind everything the verb table needs from the MANIFEST here; the renderer supplies
                // the one thing only it can (a `request` on the port it is about to create).
                const factory = this.#panelVerbs;
                const binder: PanelVerbBinder | undefined =
                    factory === undefined
                        ? undefined
                        : (request: PanelPortRequest, state: PanelStateStore): PanelVerbTable =>
                              factory({
                                  // ⚠ THE INSTANCE, not the kind (c3). A package panel's verb table
                                  // is the identity its own port answers under, and two copies of one
                                  // package panel must be two addressees — a table built for the kind
                                  // would make the second copy's `commands.register` collide with the
                                  // first's under incumbent-wins.
                                  panelId: instanceId,
                                  packageId: entry.packageId,
                                  declaredCapabilities: manifest.capabilities,
                                  manifestCommandIds: manifest.commands.map(
                                      (command) => command.id,
                                  ),
                                  request,
                                  state,
                              });
                return new IframePanelRenderer(
                    panelId,
                    instanceId,
                    entry.url,
                    entry.packageId,
                    binder,
                    this.#themeChannel,
                );
            }
            case "uitree":
                return new UitreePanelRenderer(
                    panelId,
                    instanceId,
                    this.#client,
                    manifest.gestures,
                );
            default:
                return new UnavailablePanelRenderer(panelId, instanceId);
        }
    }

    /**
     * Can THIS build mount this panel? The ONE predicate `start` reports `unavailable` from and
     * `open` refuses on (M9 e13a-2) — previously three separate checks inside `start`'s loop, which
     * is how `openById` (c3's `openInstance`) came to bypass them.
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

/** A refusal, with the reason. The one shape every `open` refusal is built from. */
function refusal(diagnostic: string): PanelOpenResult {
    return { outcome: "refused", instanceId: "", diagnostic };
}

/**
 * May a kind with `live` copies open ANOTHER? `""` = yes; otherwise the reason, NAMING the limit.
 *
 * The TS half of design 04 §3's open semantics, and the mirror of the Shell's `may_open`. Both sides
 * enforce it deliberately: this one is what the human meets (an honest refusal instead of a panel
 * that does not appear), the Shell's is the backstop over an untrusted renderer, and neither is a
 * substitute for the other.
 */
function admits(manifest: PanelManifest, live: number): string {
    switch (manifest.instances.mode) {
        case "singleton":
            return live < 1 ? "" : `'${manifest.id}' is a singleton panel and is already open`;
        case "limited":
            return live < manifest.instances.max
                ? ""
                : `'${manifest.id}' allows at most ${String(manifest.instances.max)} open copies` +
                      ` and ${String(live)} are open`;
        case "unlimited":
            return "";
        default:
            // Unreachable for a parsed manifest (`readInstances` fails closed to `singleton`), and
            // deny-by-default if it ever is not: an unknown mode must read as the MOST restrictive
            // answer, exactly as the C++ `instance_mode_token` fallback does.
            return `'${manifest.id}' declares an instance mode this build cannot read`;
    }
}

/**
 * Dockview's RENDERING STRATEGY for a manifest — the sibling of `componentFor`, and enumerated as a
 * switch for the same reason: this is the content type picking behaviour, so a new content type must
 * confront the choice rather than inherit it.
 *
 * ⚠ AN IFRAME PANEL MUST NEVER BE DETACHED (M9 e13b-1). Dockview's default `onlyWhenVisible` removes
 * an inactive panel's element from the DOM; for a frame that discards its browsing context and
 * re-navigates `src` on return, which (a) throws away the third-party document's state — the promise
 * `IframePanelRenderer.onHide` makes and cannot keep on its own — and (b) fires a SECOND `load`, which
 * `PanelPortBridge` cannot tell from the package navigating itself, so it revokes the port. The grant
 * is one-shot, so that is permanent: without `always`, the FIRST tab round-trip leaves every package
 * panel portless. `local` and `uitree` own their content and rebuild it cheaply and losslessly, so
 * they keep the default and its memory behaviour.
 *
 * Returned as a SPREADABLE fragment rather than `renderer | undefined` because
 * `exactOptionalPropertyTypes` forbids passing an explicit `undefined` for an optional field.
 */
function rendererFor(manifest: PanelManifest): { renderer?: "always" } {
    switch (manifest.contentType) {
        case "iframe":
            return { renderer: "always" };
        case "local":
        case "uitree":
            return {};
        default:
            // An unknown content type never reaches `addPanel` (`open` refuses it), and if that ever
            // changes it gets the safe default rather than a silently inherited one.
            return {};
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
