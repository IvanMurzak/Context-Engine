// The window-chrome STRIPS (editor-window-chrome a2, target design 02 §2 / §6).
//
// The mockup's four-strip frame — titlebar (38px) / play-bar slot (40px) / dock / statusbar (24px) —
// with the titlebar's REAL content and the first REAL `regionProvider`. Three facts are load-bearing:
//
//   1. CHROME-MODE GATING IS DATA-DRIVEN. What the titlebar renders is decided by `chrome.state.mode`
//      (window.ts, fetched at boot): `custom` = full strip including the window-controls cluster
//      (Windows, once b1 makes the frame ours), `hybrid` = strip minus controls, left-padded by
//      `controlsInset` (macOS, once c1 lands), `system` = a menu-bar-only strip — no controls and NO
//      DRAG DUTY (Linux, permanently — D6: the WM owns the frame). Every live backend reports
//      `system` until b1/c1 flip theirs (interim honesty, tasks/README.md); this module implements
//      all three modes NOW and the DOM tier proves them by INJECTING states, independent of the
//      live backend.
//
//   2. THE STRIPS ARE APP CHROME IN THE BANNERS PATTERN (app.css:680-688): the strip itself is
//      styled in app.css from existing theme tokens, and every CONTROL inside it is a kit component
//      (`createButton`) — no new kit family (the twelve stay closed), no new tokens. The controls
//      dispatch over the a1 window-control surface (`window.minimize` / `window.toggle-maximize` /
//      `window.close`); the max/restore glyph flips on the `editor.ui.chrome` maximized fact
//      (uibus.ts UI_TOPIC_CHROME), never on a poll.
//
//   3. THE regionProvider IS REAL NOW (02 §6). Since e05d2 the publish channel was live end to end
//      but the provider was an empty default (editorstate.ts). This module measures the titlebar's
//      caption DRAG surface and the three window-control rects (`getBoundingClientRect` → PHYSICAL
//      px, the coordinate space the Shell's InputArbiter hit-tests in) and publishes them WHOLESALE
//      on layout change (via LayoutPersistence's regionProvider seam), on window resize, and on DPI
//      change. Controls publish AFTER the caption rect, so the Shell's back-to-front last-match-wins
//      arbitration (input.cpp) needs no carve-out token. In `system` mode the provider publishes an
//      EMPTY set — "no drag duty" is a fact the Shell must also see, and a wholesale empty publish
//      is what clears a stale rect after a mode ever changed.
//
// The native CONSUMERS of these regions are b1's Windows NC hit-test (caption → HTCAPTION, the
// controls → HT*BUTTON) and c1's macOS caption-press drag; until those land the Shell's dispatch
// arms stay honestly empty, and no live backend reports a mode that publishes anything.
//
//   4. SECONDARY WINDOWS GET THE COMPACT FRAME (editor-window-chrome f1, 02 §9 / D4). A torn-out
//      window's `chrome.state.window` is `"secondary"` (the Shell derives it from the window id the
//      boot seed already distinguishes), and the strips gate on it HERE, in the one mount: the
//      titlebar renders ONLY the panel title (set from the boot seed's roster entry once the panel
//      opens — boot.ts's seeded path) plus the mode-correct controls — the cluster in `custom`, the
//      inset padding in `hybrid` — with no brand and no palette button; and the play-bar +
//      statusbar elements are REMOVED from the document outright (no menu, no play bar, no
//      statusbar — D4), so "no such DOM exists there" is a structural fact, never a hidden node.
//      The compact strip still publishes its caption + control regions over THIS window's own
//      channel — a secondary window owns its own frame exactly like the primary.
//
// DOM ONLY, no `innerHTML`, exactly like banners.ts: every node is built with `createElement` +
// `textContent`, so a project name off the wire can never inject markup into the trusted zone.

import { createButton, type KitButton } from "../../kit/src/index.js";
import { isRecord } from "./bridge.js";
import {
    REGION_KIND_CAPTION,
    REGION_KIND_CAPTION_CLOSE,
    REGION_KIND_CAPTION_MAX,
    REGION_KIND_CAPTION_MIN,
    type RegionProvider,
    type ShellRegion,
} from "./editorstate.js";
import { PALETTE_TOGGLE_COMMAND_ID } from "./palette.js";
import { UI_TOPIC_CHROME, type EditorUiBus, type EditorUiSubscription } from "./uibus.js";
import {
    CHROME_MODE_CUSTOM,
    CHROME_MODE_HYBRID,
    CHROME_WINDOW_SECONDARY,
    type ChromeState,
    type ToggleMaximizeResult,
} from "./window.js";

// ------------------------------------------------------------------------------- the document ids
// Mirror `app/index.html`'s strip elements, exactly as boot.ts's EDITOR_ROOT_ID / EDITOR_BANNERS_ID
// mirror theirs. The strips exist in the MARKUP (empty, with their fixed heights) so the frame is
// the document's shape even before the bundle runs; this module fills the titlebar and gates the
// play-bar slot at boot.

export const EDITOR_TITLEBAR_ID = "editor-titlebar";
export const EDITOR_PLAYBAR_ID = "editor-playbar";
export const EDITOR_STATUSBAR_ID = "editor-statusbar";

// ------------------------------------------------------------------------------- the DOM classes

export const TITLEBAR_CLASS = "ctx-titlebar";
export const TITLEBAR_DRAG_CLASS = "ctx-titlebar__drag";
export const TITLEBAR_BRAND_CLASS = "ctx-titlebar__brand";
export const TITLEBAR_TITLE_CLASS = "ctx-titlebar__title";
export const TITLEBAR_CONTROLS_CLASS = "ctx-titlebar__controls";
/** The sibling strips' classes — mirrored by `app/index.html` exactly like the titlebar's. */
export const PLAYBAR_CLASS = "ctx-playbar";
export const STATUSBAR_CLASS = "ctx-statusbar";

/** The mode the strip rendered, on the titlebar element — the DOM tier's gating observable. */
export const CHROME_MODE_ATTRIBUTE = "data-chrome-mode";
/** The window ROLE the strip rendered (`primary` / `secondary`) — the f1 gating observable (02 §9). */
export const CHROME_WINDOW_ATTRIBUTE = "data-chrome-window";
/** The glyph state, on the titlebar element — flips with the `editor.ui.chrome` maximized fact. */
export const CHROME_MAXIMIZED_ATTRIBUTE = "data-maximized";
/** Which control a cluster button is (`minimize` / `maximize` / `close`), for tests and smokes. */
export const CHROME_CONTROL_ATTRIBUTE = "data-chrome-control";
/** The `<html>` report of what the strips did — boot diagnosability, like every `data-editor-*`. */
export const CHROME_STRIPS_ATTRIBUTE = "data-editor-strips";

// The HYBRID inset custom properties (02 §1). Written as CSSOM custom properties on the titlebar
// element (the theme.ts precedent — never an inline `style="..."` in authored markup, which the
// scheme-contract gate forbids); app.css turns them into padding under [data-chrome-mode="hybrid"].
// CSS px, so the physical `controlsInset` is divided by the devicePixelRatio before it lands here.
export const CHROME_INSET_LEFT_PROPERTY = "--ctx-chrome-inset-left";
export const CHROME_INSET_RIGHT_PROPERTY = "--ctx-chrome-inset-right";

// ------------------------------------------------------------------------------- the region ids
// Grep-stable and MIRRORED by the C++ live smoke (cef_shell_smoke.cpp § the a2 region assertions),
// which finds these ids in the window's RegionMap to prove the publish end to end. A rename here
// reds that smoke rather than silently unbinding the proof.

export const CHROME_REGION_CAPTION_ID = "chrome.caption";
export const CHROME_REGION_MIN_ID = "chrome.caption-min";
export const CHROME_REGION_MAX_ID = "chrome.caption-max";
export const CHROME_REGION_CLOSE_ID = "chrome.caption-close";

// ------------------------------------------------------------------------------- the glyph texts
// Plain text glyphs (never icon fonts — nothing else here ships one). The max/restore pair flips
// with the maximized fact; the aria-label flips with it so a screen reader hears the ACTION the
// button will perform, not the state it is in.

const GLYPH_MINIMIZE = "–";
const GLYPH_MAXIMIZE = "□";
const GLYPH_RESTORE = "❐";
const GLYPH_CLOSE = "✕";

export const LABEL_MINIMIZE = "Minimize";
export const LABEL_MAXIMIZE = "Maximize";
export const LABEL_RESTORE = "Restore";
export const LABEL_CLOSE = "Close";
export const LABEL_PALETTE = "Command palette";

/** The product name the title falls back to when no project name is known (the welcome screen). */
export const DEFAULT_TITLE = "Context Editor";

// ------------------------------------------------------------------------------- the fact parser

/** The `editor.ui.chrome` payload (chrome_facts.cpp): which window, and its new maximized state. */
export interface ChromeFact {
    readonly windowId: number;
    readonly maximized: boolean;
}

/**
 * Parse an `editor.ui.chrome` payload, TOTAL like every wire parser here: `null` for anything that
 * carries no readable window id, so a malformed mirrored envelope can never throw in a subscriber.
 */
export function parseChromeFact(payload: unknown): ChromeFact | null {
    if (!isRecord(payload)) {
        return null;
    }
    const windowId = payload["windowId"];
    if (typeof windowId !== "number" || !Number.isFinite(windowId)) {
        return null;
    }
    return { windowId, maximized: payload["maximized"] === true };
}

/**
 * Wire the titlebar's max/restore glyph to the `editor.ui.chrome` maximized fact (02 §1).
 *
 * Called by boot once the window mechanism is up — the fact arrives over that window's bus (the
 * Shell's placement poll unicasts it over the ui.mirror relay, and the poll drains it onto the
 * bus), and the filter needs the window id `window.list` reported. The windowId filter is
 * belt-and-braces on top of the unicast: the payload names its subject, so a relay that ever
 * broadened to broadcast could not flip a peer's glyph. Extracted (and exported) so the DOM tier
 * drives THIS function against a real bus rather than a copy of it — the makeSessionActions rule.
 */
export function subscribeChromeFacts(
    bus: EditorUiBus,
    windowId: number,
    mount: ChromeMount,
): EditorUiSubscription {
    return bus.subscribe(UI_TOPIC_CHROME, (event): void => {
        const fact = parseChromeFact(event.payload);
        if (fact !== null && fact.windowId === windowId) {
            mount.setMaximized(fact.maximized);
        }
    });
}

// ------------------------------------------------------------------------------- the strip mount

/** The window-control half of the a1 surface the cluster dispatches over (WindowClient satisfies it). */
export interface ChromeWindowControls {
    minimize(): Promise<unknown>;
    toggleMaximize(): Promise<ToggleMaximizeResult>;
    close(): Promise<unknown>;
}

/** The three strip elements from `app/index.html`. */
export interface ChromeStripElements {
    readonly titlebar: HTMLElement;
    readonly playbar: HTMLElement;
    readonly statusbar: HTMLElement;
}

/**
 * Locate the strip elements in `doc`. `null` when the titlebar is absent — an older served document
 * or a bare harness, in which case the editor renders exactly what it did before a2 (no strips, the
 * empty-default region provider): honest degradation, never an error.
 */
export function findChromeStripElements(doc: Document): ChromeStripElements | null {
    const titlebar = doc.getElementById(EDITOR_TITLEBAR_ID);
    const playbar = doc.getElementById(EDITOR_PLAYBAR_ID);
    const statusbar = doc.getElementById(EDITOR_STATUSBAR_ID);
    if (titlebar === null || playbar === null || statusbar === null) {
        return null;
    }
    return { titlebar, playbar, statusbar };
}

export interface MountChromeOptions {
    readonly state: ChromeState;
    /** The project's display name; `""` renders the product name (the welcome screen's state). */
    readonly projectName: string;
    /** True on the welcome screen: the play-bar slot hides (no session to control — 02 §2). */
    readonly welcome: boolean;
    readonly controls: ChromeWindowControls;
    /** Dispatch a command id through the late-bound command registry (boot.ts closes over it). */
    readonly executeCommand: (commandId: string) => void;
    /** Injected by the DOM tier to pin the physical-px arithmetic; defaults to the live ratio. */
    readonly devicePixelRatio?: () => number;
}

/** What `mountChrome` produced — the handle boot.ts keeps and the DOM tier asserts on. */
export interface ChromeMount {
    /** True while the strip renders the RESTORE glyph. */
    isMaximized(): boolean;
    /**
     * Flip the max/restore glyph. Driven at mount from the boot snapshot, then by the
     * `editor.ui.chrome` fact (boot.ts subscribes; unicast, but the payload's windowId still names
     * the subject) and by the window's own toggle result — never by a poll.
     */
    setMaximized(maximized: boolean): void;
    /**
     * Rename the strip's title. The f1 seeded-boot seam (02 §9): a torn-out window mounts before
     * its boot seed's panel opens, so boot.ts sets the PANEL title here once the roster names it.
     * `""` restores the product-name fallback — the strip never renders an empty title.
     */
    setTitle(text: string): void;
    /**
     * Measure the strip's regions in PHYSICAL px (02 §6): the caption drag surface FIRST, then the
     * control rects — the publish order the Shell's back-to-front last-match-wins relies on. Empty
     * in `system` mode (no drag duty) and for any rect that is not currently laid out.
     */
    regions(): readonly ShellRegion[];
}

function el(doc: Document, tag: string, className: string, text = ""): HTMLElement {
    const node = doc.createElement(tag);
    if (className !== "") {
        node.className = className;
    }
    if (text !== "") {
        node.textContent = text;
    }
    return node;
}

/** The live devicePixelRatio, 1 when there is no window to ask (a documentless host). */
function defaultDevicePixelRatio(): number {
    if (typeof window === "undefined") {
        return 1;
    }
    const ratio = window.devicePixelRatio;
    return typeof ratio === "number" && Number.isFinite(ratio) && ratio > 0 ? ratio : 1;
}

/** One measured region in PHYSICAL px, or `null` for an element that is not laid out. */
function physicalRegion(
    element: HTMLElement,
    id: string,
    kind: ShellRegion["kind"],
    dpr: number,
): ShellRegion | null {
    const rect = element.getBoundingClientRect();
    // Round the EDGES and derive the extents, never left+width independently: with fractional CSS
    // coordinates (text-sized buttons, fractional ratios) `round(left·dpr) + round(width·dpr)` can
    // land a physical pixel away from `round(right·dpr)` — a region overhanging the window edge or
    // biting a pixel out of its neighbour in the Shell's hit-test, for an element whose true edge
    // never moved.
    const x0 = Math.round(rect.left * dpr);
    const y0 = Math.round(rect.top * dpr);
    const width = Math.round(rect.right * dpr) - x0;
    const height = Math.round(rect.bottom * dpr) - y0;
    if (width <= 0 || height <= 0) {
        return null;
    }
    return { id, kind, rect: { x: x0, y: y0, width, height } };
}

/**
 * Render the titlebar's content and gate the strips on the chrome mode (02 §2).
 *
 * Replaces the titlebar's children wholesale (a re-mount is a re-render, like `mountBanners`), sets
 * the play-bar slot's visibility for the boot path taken, and leaves the statusbar to its own
 * module (d2 — statusbar.ts fills it from boot). Never touches the bridge itself: dispatch goes
 * through the injected `controls`
 * and `executeCommand`, which is what lets the DOM tier prove every mode with plain spies.
 */
export function mountChrome(elements: ChromeStripElements, options: MountChromeOptions): ChromeMount {
    const doc = elements.titlebar.ownerDocument;
    const mode = options.state.mode;
    // f1 (02 §9): a torn-out window renders the COMPACT frame — see module fact 4. The role is the
    // Shell's own `chrome.state.window` (derived from the window id, never self-reported), so the
    // gating is data-driven exactly like the mode's and the DOM tier proves it by injection.
    const secondary = options.state.window === CHROME_WINDOW_SECONDARY;
    const dpr = options.devicePixelRatio ?? defaultDevicePixelRatio;

    const titlebar = elements.titlebar;
    titlebar.replaceChildren();
    titlebar.setAttribute(CHROME_MODE_ATTRIBUTE, mode);
    titlebar.setAttribute(CHROME_WINDOW_ATTRIBUTE, options.state.window);

    // The HYBRID inset (02 §1): physical px from the Shell, CSS px on the element. Written (and
    // cleared) as custom properties so the padding rule lives in app.css with every other strip rule.
    if (mode === CHROME_MODE_HYBRID) {
        const ratio = dpr();
        titlebar.style.setProperty(
            CHROME_INSET_LEFT_PROPERTY,
            `${String(options.state.controlsInset.left / ratio)}px`,
        );
        titlebar.style.setProperty(
            CHROME_INSET_RIGHT_PROPERTY,
            `${String(options.state.controlsInset.right / ratio)}px`,
        );
    } else {
        titlebar.style.removeProperty(CHROME_INSET_LEFT_PROPERTY);
        titlebar.style.removeProperty(CHROME_INSET_RIGHT_PROPERTY);
    }

    // --- the caption drag surface: brand + title, and the strip's flexible middle ---------------
    // Its OWN element rather than "the whole bar": the caption region is measured from exactly this
    // rect, so the palette button and the controls cluster sit OUTSIDE the drag surface and need no
    // carve-out at all — a click on them is client input on every platform.
    const drag = el(doc, "div", TITLEBAR_DRAG_CLASS);
    if (!secondary) {
        // The brand mark is the FULL frame's — the compact secondary strip is panel title +
        // controls and nothing else (02 §9).
        const brand = el(doc, "span", TITLEBAR_BRAND_CLASS);
        brand.setAttribute("aria-hidden", "true");
        drag.append(brand);
    }
    const title = el(
        doc,
        "span",
        TITLEBAR_TITLE_CLASS,
        options.projectName !== "" ? options.projectName : DEFAULT_TITLE,
    );
    drag.append(title);
    titlebar.append(drag);

    // --- the palette button (01 §7: the first reliable, non-programmatic palette opener) ---------
    // A kit button dispatching `workbench.palette.toggle` through the ONE command registry — the
    // boot.ts:claimPaletteToggle pattern's public face. Late-bound: before the command layer is up
    // (and on the welcome screen, which has none) the dispatch is a no-op refusal, never a throw.
    // NOT rendered in a secondary window (02 §9): the compact strip carries no app chrome beyond
    // the title and the window controls.
    if (!secondary) {
        const palette = createButton({
            label: "›_",
            accessibleLabel: LABEL_PALETTE,
            commandId: PALETTE_TOGGLE_COMMAND_ID,
            onActivate: (): void => {
                options.executeCommand(PALETTE_TOGGLE_COMMAND_ID);
            },
        });
        titlebar.append(palette.element);
    }

    // --- the window-controls cluster (custom mode ONLY — 02 §1) ----------------------------------
    let maximized = false;
    let maxButton: KitButton | null = null;
    let minButton: KitButton | null = null;
    let closeButton: KitButton | null = null;
    const applyMaximized = (next: boolean): void => {
        maximized = next;
        titlebar.setAttribute(CHROME_MAXIMIZED_ATTRIBUTE, String(next));
        if (maxButton !== null) {
            maxButton.setLabel(next ? GLYPH_RESTORE : GLYPH_MAXIMIZE);
            const label = next ? LABEL_RESTORE : LABEL_MAXIMIZE;
            maxButton.element.setAttribute("aria-label", label);
            maxButton.element.title = label;
        }
    };
    if (mode === CHROME_MODE_CUSTOM) {
        const cluster = el(doc, "div", TITLEBAR_CONTROLS_CLASS);
        // A labelled GROUP, per the banners worked example's ARIA discipline: three one-glyph
        // buttons are legible to a screen reader only when the cluster says what they belong to.
        cluster.setAttribute("role", "group");
        cluster.setAttribute("aria-label", "Window controls");
        minButton = createButton({
            label: GLYPH_MINIMIZE,
            accessibleLabel: LABEL_MINIMIZE,
            onActivate: (): void => {
                void options.controls.minimize();
            },
        });
        minButton.element.setAttribute(CHROME_CONTROL_ATTRIBUTE, "minimize");
        maxButton = createButton({
            label: GLYPH_MAXIMIZE,
            accessibleLabel: LABEL_MAXIMIZE,
            onActivate: (): void => {
                // The glyph follows the TOGGLE RESULT immediately (the Shell writes the new state
                // into the reply); the `editor.ui.chrome` fact then confirms it — same value, so
                // applying both is idempotent, and an OS-driven flip (Win+Up) still arrives alone.
                void options.controls.toggleMaximize().then((result): void => {
                    if (result.accepted) {
                        applyMaximized(result.maximized);
                    }
                });
            },
        });
        maxButton.element.setAttribute(CHROME_CONTROL_ATTRIBUTE, "maximize");
        closeButton = createButton({
            label: GLYPH_CLOSE,
            accessibleLabel: LABEL_CLOSE,
            onActivate: (): void => {
                // `window.close` already carries the primary-vs-secondary policy Shell-side.
                void options.controls.close();
            },
        });
        closeButton.element.setAttribute(CHROME_CONTROL_ATTRIBUTE, "close");
        cluster.append(minButton.element, maxButton.element, closeButton.element);
        titlebar.append(cluster);
    }
    applyMaximized(options.state.maximized);

    // --- the sibling strips ----------------------------------------------------------------------
    // The play-bar SLOT hides on the welcome screen — no session to control (02 §2); d1's
    // playbar.ts fills it on the editor path. The statusbar renders in BOTH modes (it is part of
    // the frame) and is filled by d2's statusbar.ts from boot. The slot gating lives here, in one
    // place, so the welcome and project boots cannot drift apart on what the frame shows.
    //
    // f1 (02 §9 / D4): a SECONDARY window REMOVES both siblings from the document — no play bar,
    // no statusbar in a torn-out window, structurally (a hidden node would still be DOM for d1/d2
    // to fill later; an absent one cannot be). The dock then takes the freed rows through the same
    // flex column, no stylesheet change needed.
    if (secondary) {
        elements.playbar.remove();
        elements.statusbar.remove();
    } else {
        elements.playbar.hidden = options.welcome;
    }

    return {
        isMaximized: (): boolean => maximized,
        setMaximized: (next: boolean): void => {
            applyMaximized(next);
        },
        setTitle: (text: string): void => {
            title.textContent = text !== "" ? text : DEFAULT_TITLE;
        },
        regions: (): readonly ShellRegion[] => {
            // NO DRAG DUTY in `system` mode (D6): the WM owns the frame, so the honest region set
            // is empty — and publishing that emptiness wholesale is what clears a stale rect.
            if (mode !== CHROME_MODE_CUSTOM && mode !== CHROME_MODE_HYBRID) {
                return [];
            }
            const ratio = dpr();
            const regions: ShellRegion[] = [];
            // CAPTION FIRST, controls after — the publish order 02 §6 pins so the Shell's
            // back-to-front last-match-wins arbitration resolves a control above the drag surface.
            const caption = physicalRegion(drag, CHROME_REGION_CAPTION_ID, REGION_KIND_CAPTION, ratio);
            if (caption !== null) {
                regions.push(caption);
            }
            const controlRegions: readonly (readonly [
                KitButton | null,
                string,
                ShellRegion["kind"],
            ])[] = [
                [minButton, CHROME_REGION_MIN_ID, REGION_KIND_CAPTION_MIN],
                [maxButton, CHROME_REGION_MAX_ID, REGION_KIND_CAPTION_MAX],
                [closeButton, CHROME_REGION_CLOSE_ID, REGION_KIND_CAPTION_CLOSE],
            ];
            for (const [button, id, kind] of controlRegions) {
                if (button === null) {
                    continue;
                }
                const region = physicalRegion(button.element, id, kind, ratio);
                if (region !== null) {
                    regions.push(region);
                }
            }
            return regions;
        },
    };
}

// ------------------------------------------------------------------------------- the publisher

/** The slice of a MediaQueryList the DPI re-arm needs (injectable for the DOM tier). */
export interface MediaQueryListLike {
    addEventListener?: (type: "change", listener: () => void) => void;
    removeEventListener?: (type: "change", listener: () => void) => void;
}

/** The slice of `window` the resize trigger needs (injectable for the DOM tier). */
export interface ResizeTargetLike {
    addEventListener(type: "resize", listener: () => void): void;
    removeEventListener(type: "resize", listener: () => void): void;
}

export interface ChromeRegionPublisherOptions {
    /** The ONE region provider (mount.regions today; e11's viewports extend the same closure). */
    readonly provider: RegionProvider;
    /** `EditorStateClient.publishRegions`, bound — refusal-tolerant on that side already. */
    readonly publish: (regions: readonly ShellRegion[]) => Promise<boolean>;
    /** Collapses a resize storm into one publish. The smoke's wait loop outlasts it comfortably. */
    readonly debounceMs?: number;
    readonly devicePixelRatio?: () => number;
    /** Defaults to `window`; `undefined` there (a documentless host) arms no resize trigger. */
    readonly resizeTarget?: ResizeTargetLike;
    /** Defaults to the global `matchMedia`; absent (an old harness) arms no DPI trigger. */
    readonly matchMedia?: (query: string) => MediaQueryListLike | null;
    /** Fire-on-publish report hook (boot writes the `data-editor-strips` attribute from it). */
    readonly onPublish?: (regionCount: number, publishes: number) => void;
}

const DEFAULT_PUBLISH_DEBOUNCE_MS = 100;

/**
 * Publishes the region map on the two triggers Dockview's layout-change signal cannot see (02 §6):
 * a window RESIZE (the strip rects track the client width) and a DPI CHANGE (physical px moved
 * under every CSS rect). The third trigger — layout change — stays LayoutPersistence's, which is
 * handed the SAME provider, so the two paths can never publish different region sets.
 *
 * The DPI trigger is the re-arming `matchMedia("(resolution: <dpr>dppx)")` idiom: a resolution
 * query matches exactly the current ratio, so its ONE `change` fires precisely when the ratio
 * moves, after which the listener re-arms against the new value.
 */
export class ChromeRegionPublisher {
    readonly #provider: RegionProvider;
    readonly #publish: (regions: readonly ShellRegion[]) => Promise<boolean>;
    readonly #debounceMs: number;
    readonly #dpr: () => number;
    readonly #resizeTarget: ResizeTargetLike | undefined;
    readonly #matchMedia: ((query: string) => MediaQueryListLike | null) | undefined;
    readonly #onPublish: ((regionCount: number, publishes: number) => void) | undefined;
    #onResize: (() => void) | null = null;
    #dpiList: { list: MediaQueryListLike; listener: () => void } | null = null;
    #timer: ReturnType<typeof setTimeout> | null = null;
    #publishes = 0;
    #disposed = false;

    constructor(options: ChromeRegionPublisherOptions) {
        this.#provider = options.provider;
        this.#publish = options.publish;
        this.#debounceMs = options.debounceMs ?? DEFAULT_PUBLISH_DEBOUNCE_MS;
        this.#dpr = options.devicePixelRatio ?? defaultDevicePixelRatio;
        this.#resizeTarget =
            options.resizeTarget ??
            (typeof window !== "undefined" ? (window as ResizeTargetLike) : undefined);
        this.#matchMedia =
            options.matchMedia ??
            (typeof matchMedia === "function"
                ? (query: string): MediaQueryListLike | null => matchMedia(query)
                : undefined);
        this.#onPublish = options.onPublish;
    }

    /** How many publishes have completed (the DOM tier's trigger observable). */
    get publishes(): number {
        return this.#publishes;
    }

    /** Arm both triggers and perform the initial publish (awaited, so boot's report is settled). */
    async start(): Promise<void> {
        if (this.#resizeTarget !== undefined) {
            const onResize = (): void => {
                this.#schedule();
            };
            this.#resizeTarget.addEventListener("resize", onResize);
            this.#onResize = onResize;
        }
        this.#armDpi();
        await this.publishNow();
    }

    /** Publish the provider's current set immediately (the debounced triggers land here too). */
    async publishNow(): Promise<void> {
        if (this.#disposed) {
            return;
        }
        const regions = this.#provider();
        await this.#publish(regions);
        this.#publishes += 1;
        this.#onPublish?.(regions.length, this.#publishes);
    }

    #schedule(): void {
        if (this.#disposed) {
            return;
        }
        if (this.#timer !== null) {
            clearTimeout(this.#timer);
        }
        this.#timer = setTimeout((): void => {
            this.#timer = null;
            void this.publishNow();
        }, this.#debounceMs);
    }

    /**
     * (Re-)arm the DPI trigger against the CURRENT ratio. Re-armed from its own listener because a
     * resolution query matches one exact ratio: after a change the old query is permanently false,
     * so listening on it again would hear nothing forever.
     */
    #armDpi(): void {
        if (this.#matchMedia === undefined) {
            return;
        }
        this.#disarmDpi();
        const list = this.#matchMedia(`(resolution: ${String(this.#dpr())}dppx)`);
        if (list === null || typeof list.addEventListener !== "function") {
            return;
        }
        const listener = (): void => {
            this.#armDpi();
            this.#schedule();
        };
        list.addEventListener("change", listener);
        this.#dpiList = { list, listener };
    }

    #disarmDpi(): void {
        if (this.#dpiList !== null) {
            this.#dpiList.list.removeEventListener?.("change", this.#dpiList.listener);
            this.#dpiList = null;
        }
    }

    /** Release every listener and cancel a pending publish. Idempotent. */
    dispose(): void {
        this.#disposed = true;
        if (this.#timer !== null) {
            clearTimeout(this.#timer);
            this.#timer = null;
        }
        if (this.#onResize !== null && this.#resizeTarget !== undefined) {
            this.#resizeTarget.removeEventListener("resize", this.#onResize);
            this.#onResize = null;
        }
        this.#disarmDpi();
    }
}

// ------------------------------------------------------------------------------- the boot glue

export interface StartChromeStripsOptions extends MountChromeOptions {
    readonly publishRegions: (regions: readonly ShellRegion[]) => Promise<boolean>;
    readonly debounceMs?: number;
    readonly resizeTarget?: ResizeTargetLike;
    readonly matchMedia?: (query: string) => MediaQueryListLike | null;
}

/** What `startChromeStrips` wired — boot.ts threads these into the panel/window layers. */
export interface ChromeStrips {
    readonly mount: ChromeMount;
    /** The ONE provider: handed to LayoutPersistence too, so both publish paths agree (02 §6). */
    readonly provider: RegionProvider;
    readonly publisher: ChromeRegionPublisher;
}

/**
 * Mount the strips and start the region publisher — the whole a2 bring-up, kept here so boot.ts
 * adds one call. Awaits the INITIAL publish so the Shell's region map is populated (and the live
 * smoke's assertion is race-free) before boot proceeds to the panels.
 */
export async function startChromeStrips(
    elements: ChromeStripElements,
    options: StartChromeStripsOptions,
): Promise<ChromeStrips> {
    const mount = mountChrome(elements, options);
    // `mount.regions` is a `this`-free closure already — it IS the provider, no wrapper needed.
    const provider: RegionProvider = mount.regions;
    const doc = elements.titlebar.ownerDocument;
    // Everything but the two counters is fixed at mount (the cluster never gains or loses buttons
    // afterwards), so the prefix is computed ONCE instead of re-queried on every republish. The
    // play-bar token reads the DOM truth: `removed` is the f1 secondary state (the element left the
    // document), distinct from `hidden` (the welcome screen's still-present slot).
    const playbarState = !elements.playbar.isConnected
        ? "removed"
        : elements.playbar.hidden
          ? "hidden"
          : "visible";
    const prefix = `mode=${options.state.mode} window=${options.state.window} controls=${String(
        elements.titlebar.querySelectorAll(`[${CHROME_CONTROL_ATTRIBUTE}]`).length,
    )} playbar=${playbarState}`;
    const report = (regionCount: number, publishes: number): void => {
        doc.documentElement.setAttribute(
            CHROME_STRIPS_ATTRIBUTE,
            `${prefix} regions=${String(regionCount)} publishes=${String(publishes)}`,
        );
    };
    // The publisher's optional knobs are declared with the SAME optionality on both option types,
    // so the spread forwards exactly the keys the caller supplied (`exactOptionalPropertyTypes`
    // stays satisfied); the mount-only keys ride along inert.
    const publisher = new ChromeRegionPublisher({
        ...options,
        provider,
        publish: options.publishRegions,
        onPublish: report,
    });
    await publisher.start();
    return { mount, provider, publisher };
}
