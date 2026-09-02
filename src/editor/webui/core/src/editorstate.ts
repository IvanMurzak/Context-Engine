// Layout persistence + region maps, JS side (M9 e05d2, design 03 §1 / §6 / 04 §2).
//
// This is the TS mirror of `src/editor/shell/include/context/editor/shell/editor_state_bridge.h`, and
// the two facts that make it load-bearing are the same two panels.ts states:
//
//   1. THE VOCABULARY IS CROSS-LANGUAGE AND GATED. The method names and region-kind tokens below are
//      byte-compared against the C++ constants by `tools/check_webui_assets.py --panel-contract`
//      (ctest `webui-panel-contract`), reading the values out of the BUILT bundle. A rename on either
//      side would otherwise unbind persistence SILENTLY — the editor would call a method the Shell no
//      longer routes, layout would stop saving, and NOTHING would report an error anywhere.
//
//   2. THE SHELL IS THE SINGLE WRITER of `.editor/editor-state.json` (C-F3, design 03 §1). editor-core
//      PUBLISHES its arrangement over the bridge and READS the persisted blob back through
//      `editor.state.get`; it never opens, writes, or locks that file. There is no `fetch`, no
//      `localStorage`, no file API here — only the bridge. A direct write would be a defect even if it
//      worked.
//
// The three persistence triggers design 04 §2 requires are all here: DEBOUNCED during interaction
// (a sash drag is one publish, not one per frame), ON-EXIT (a final publish on `pagehide`), and
// CRASH-RESTORE (the debounced publishes land in the Shell's atomic-write store, so the last one is a
// complete last-known-good file a non-graceful exit leaves intact — the Shell's guarantee, not ours).

import type { DockviewDisposable } from "./dockview.js";
import type { PanelClient } from "./panels.js";
import type { PanelHost } from "./panelhost.js";
import type { ShellBridge } from "./bridge.js";
import { BridgeError, isRecord } from "./bridge.js";

// --------------------------------------------------------------------------- the wire vocabulary
// MUST match editor_state_bridge.h's kEditor* / kRegionKind*. See note 1 above.

export const EDITOR_STATE_GET_METHOD = "editor.state.get";
export const EDITOR_STATE_PUBLISH_METHOD = "editor.state.publish";
export const EDITOR_REGIONS_PUBLISH_METHOD = "editor.regions.publish";
// M9 e05d4: the boot-time restore OUTCOME report. One-way, best-effort — editor-core tells the Shell
// what its restore did so the restart smoke can distinguish a boot that reapplied an arrangement from
// a fresh one. MUST match editor_state_bridge.h's kEditorLayoutRestoredMethod (note 1 above).
export const EDITOR_LAYOUT_RESTORED_METHOD = "editor.layout.restored";

/**
 * The closed RegionKind vocabulary (03 §6), mirroring `shell::RegionKind`'s wire tokens: a viewport
 * content rect (camera / picking / gizmo gestures), a non-viewport native-interaction region, and —
 * since editor-window-chrome a1 (target design 02 §6) — the titlebar's four caption chrome regions:
 * the caption DRAG surface and the three window-control rects a2's regionProvider publishes.
 * Controls publish AFTER the caption rect, so back-to-front last-match-wins needs no carve-out
 * token. The vocabulary moves in all FOUR mirror sites in one commit (input.h,
 * editor_state_bridge.h, here, and the `webui-panel-contract` gate) and stays CLOSED: the Shell
 * REFUSES a kind outside this set at parse.
 */
export const REGION_KIND_VIEWPORT = "viewport";
export const REGION_KIND_NATIVE = "native";
export const REGION_KIND_CAPTION = "caption";
export const REGION_KIND_CAPTION_MIN = "caption-min";
export const REGION_KIND_CAPTION_MAX = "caption-max";
export const REGION_KIND_CAPTION_CLOSE = "caption-close";
export type RegionKind =
    | typeof REGION_KIND_VIEWPORT
    | typeof REGION_KIND_NATIVE
    | typeof REGION_KIND_CAPTION
    | typeof REGION_KIND_CAPTION_MIN
    | typeof REGION_KIND_CAPTION_MAX
    | typeof REGION_KIND_CAPTION_CLOSE;

// -------------------------------------------------------------------------------- the wire shapes

/** A region rect in PHYSICAL client pixels — matching what the OS reports and what the Shell hit-tests. */
export interface ShellRegionRect {
    readonly x: number;
    readonly y: number;
    readonly width: number;
    readonly height: number;
}

/** One native region the Shell arbitrates input against (03 §6). */
export interface ShellRegion {
    readonly id: string;
    readonly kind: RegionKind;
    readonly rect: ShellRegionRect;
}

/**
 * The persisted editor-state blob the Shell round-trips.
 *
 * `layout` is Dockview's `toJSON()` output — opaque to the Shell, `null` for a fresh project.
 * `panels` maps a panel id to its D6 state blob (`{schemaVersion, data}`), assembled by editor-core
 * from TWO sources it deliberately keeps in ONE shape: a C++-modeled panel's blob comes from
 * `panel.state.get` and is applied back via `panel.state.set`, while a third-party `iframe` panel's
 * comes from its PORT store (`PanelHost.portState` / `seedPortState`, M9 e13d) because it has no
 * model to ask. The Shell stores the map opaquely either way, so it never has to know which.
 */
export interface PersistedState {
    readonly layout: unknown;
    readonly panels: Readonly<Record<string, unknown>>;
}

/** Parse an `editor.state.get` result, total against a malformed or partial envelope. */
export function parsePersistedState(value: unknown): PersistedState {
    if (!isRecord(value)) {
        return { layout: null, panels: {} };
    }
    const panels = isRecord(value["panels"]) ? value["panels"] : {};
    return { layout: value["layout"] ?? null, panels };
}

// ---------------------------------------------------------------- the ONE physical-px measurement
//
// THE SINGLE DPI SEAM ON THIS SIDE OF THE BRIDGE (editor-window-chrome a2). `ShellRegion::rect` is
// PHYSICAL client pixels — that is what the C++ `ShellRegion` documents, what the OS reports for a
// pointer, and what `ViewportLayer::content_rect` composites against — while the DOM measures in CSS
// pixels (DIP). Exactly one conversion may therefore exist, and this is it.
//
// It lived in chrome.ts through editor-UX e2, private to the titlebar. editor-UX e3 gave it a SECOND
// caller (viewport.ts, measuring each Scene viewport's dock rect), and the task's own instruction was
// "take the DPI scale from wherever a2 put it; never open a second source" — so the function moved
// HERE, beside the `ShellRegion` it produces and the `publishRegions` that sends it, rather than
// being copied. A copy is the bug: two roundings that agree today and diverge the first time one is
// touched, invisible at scale 1.0 where physical and CSS pixels coincide.

/** The live devicePixelRatio, 1 when there is no window to ask (a documentless host). */
export function defaultDevicePixelRatio(): number {
    if (typeof window === "undefined") {
        return 1;
    }
    const ratio = window.devicePixelRatio;
    return typeof ratio === "number" && Number.isFinite(ratio) && ratio > 0 ? ratio : 1;
}

/**
 * One measured element as a region in PHYSICAL px, or `null` for an element that is not laid out.
 *
 * ⚠ ROUND THE EDGES AND DERIVE THE EXTENTS, never left+width independently. With fractional CSS
 * coordinates (text-sized buttons, a dock sash at a fractional offset, a non-integral ratio)
 * `round(left·dpr) + round(width·dpr)` can land a physical pixel away from `round(right·dpr)` — a
 * region overhanging the window edge or biting a pixel out of its neighbour in the Shell's
 * hit-test, for an element whose true edge never moved. For a VIEWPORT that same pixel is also the
 * render target's width, so the error resamples the whole scene rather than merely mis-routing a
 * click.
 */
export function physicalRegion(
    element: Element,
    id: string,
    kind: ShellRegion["kind"],
    dpr: number,
): ShellRegion | null {
    const rect = element.getBoundingClientRect();
    const x0 = Math.round(rect.left * dpr);
    const y0 = Math.round(rect.top * dpr);
    const width = Math.round(rect.right * dpr) - x0;
    const height = Math.round(rect.bottom * dpr) - y0;
    if (width <= 0 || height <= 0) {
        return null;
    }
    return { id, kind, rect: { x: x0, y: y0, width, height } };
}

// ------------------------------------------------------------------------------- the bridge client

/**
 * The thin typed client over the `editor.*` bridge methods.
 *
 * Holds no state — the editor-state document lives in the Shell (D18), so a cache here would be a
 * second source of truth free to disagree with it. A persistence FAILURE is returned, not thrown: a
 * full disk or a read-only project must not take down a working editor, and the caller degrades.
 */
export class EditorStateClient {
    readonly #bridge: ShellBridge;

    constructor(bridge: ShellBridge) {
        this.#bridge = bridge;
    }

    /** Read the persisted `{layout, panels}` blob (the boot restore path). */
    async get(): Promise<PersistedState> {
        return parsePersistedState(await this.#bridge.call(EDITOR_STATE_GET_METHOD));
    }

    /** Publish the current arrangement + per-panel blobs. `false` when the Shell refused to store it. */
    async publish(layout: unknown, panels: Readonly<Record<string, unknown>>): Promise<boolean> {
        return this.#callTolerant(EDITOR_STATE_PUBLISH_METHOD, { layout, panels });
    }

    /** Publish the window's region map (03 §6). `false` when the Shell refused it. */
    async publishRegions(regions: readonly ShellRegion[]): Promise<boolean> {
        return this.#callTolerant(EDITOR_REGIONS_PUBLISH_METHOD, {
            regions: regions.map((region) => ({
                id: region.id,
                kind: region.kind,
                rect: {
                    x: region.rect.x,
                    y: region.rect.y,
                    width: region.rect.width,
                    height: region.rect.height,
                },
            })),
        });
    }

    /**
     * Report the boot-time restore OUTCOME to the Shell (M9 e05d4). One-way and best-effort: an
     * older Shell that does not route `editor.layout.restored` refuses with `unknown_method`, which
     * `#callTolerant` maps to `false` and the caller ignores — reporting the restore result must
     * never affect whether the editor is up. The restart smoke asserts the Shell saw
     * `layoutRestored:true` on a boot that reapplied a persisted arrangement.
     */
    async reportRestore(report: LayoutRestoreReport): Promise<boolean> {
        return this.#callTolerant(EDITOR_LAYOUT_RESTORED_METHOD, {
            layoutRestored: report.layoutRestored,
            panelsRestored: report.panelsRestored,
            degraded: [...report.degraded],
        });
    }

    /**
     * Call a persistence method, tolerating a Shell REFUSAL. A `BridgeError` (a full disk, a
     * read-only project) resolves to `false` so a persistence failure never takes down a working
     * editor; any other error is a real bug and propagates. This is the single home of that
     * `false`-on-refusal policy — every publish path shares it, so the contract cannot drift between
     * them.
     */
    async #callTolerant(method: string, params: Record<string, unknown>): Promise<boolean> {
        try {
            await this.#bridge.call(method, params);
            return true;
        } catch (error) {
            if (error instanceof BridgeError) {
                return false;
            }
            throw error;
        }
    }
}

// ------------------------------------------------------------------------------- the coordinator

/**
 * Supplies the window's native regions on each layout change (03 §6).
 *
 * REAL since editor-window-chrome a2: boot hands this the titlebar's chrome provider (chrome.ts),
 * which measures the caption drag surface + window-control rects in custom/hybrid chrome — the
 * DEFAULT here stays the empty set, for a document with no strips (an older page, a bare harness).
 * The publish fires on every layout change either way, so a removed region's stale rect is cleared,
 * and e11's viewport rects extend the SAME provider rather than building a second channel.
 */
export type RegionProvider = () => readonly ShellRegion[];

export interface LayoutPersistenceOptions {
    readonly panelHost: PanelHost;
    readonly panelClient: PanelClient;
    readonly stateClient: EditorStateClient;
    /** The quiet period a burst of layout changes waits out before one publish. */
    readonly debounceMs?: number;
    readonly regionProvider?: RegionProvider;
}

/** What a restore did — returned so a caller (and the e05d4 smoke) can assert on it. */
export interface LayoutRestoreReport {
    readonly layoutRestored: boolean;
    readonly panelsRestored: number;
    /** Panels whose persisted blob was stale (schemaVersion mismatch) and rebuilt from defaults. */
    readonly degraded: readonly string[];
}

/** Long enough that a sash drag is one publish, short enough that little is lost on an abrupt close. */
const DEFAULT_DEBOUNCE_MS = 400;

/**
 * Persists the editor's arrangement and republishes its region map, and restores both on boot.
 *
 * Owns NO layout geometry (that is PanelHost's) and NO panel state (that is the C++ models', reached
 * through PanelClient — or, for a third-party `iframe` panel, its port store, reached through
 * PanelHost) — it is the coordinator that moves them across the bridge on the three triggers. Every
 * method is failure-tolerant: a persistence error must never undo a working editor.
 */
export class LayoutPersistence {
    readonly #panelHost: PanelHost;
    readonly #panelClient: PanelClient;
    readonly #stateClient: EditorStateClient;
    readonly #debounceMs: number;
    readonly #regionProvider: RegionProvider;
    #timer: ReturnType<typeof setTimeout> | null = null;
    #layoutSub: DockviewDisposable | null = null;
    #onPageHide: (() => void) | null = null;
    #disposed = false;

    constructor(options: LayoutPersistenceOptions) {
        this.#panelHost = options.panelHost;
        this.#panelClient = options.panelClient;
        this.#stateClient = options.stateClient;
        this.#debounceMs = options.debounceMs ?? DEFAULT_DEBOUNCE_MS;
        this.#regionProvider = options.regionProvider ?? ((): readonly ShellRegion[] => []);
    }

    /**
     * Apply the persisted arrangement + per-panel state on boot, OVER the defaults `PanelHost.start`
     * opened. A fresh project restores nothing and the defaults stand.
     *
     * NEVER THROWS on a single panel: a schemaVersion mismatch degrades THAT panel to its defaults
     * (the C++ PanelHost answers `restored: false` + a diagnostic) and must not abort the whole
     * layout restore — that is the D6 contract e05b built and this loop depends on.
     */
    async restore(): Promise<LayoutRestoreReport> {
        const persisted = await this.#stateClient.get();
        let layoutRestored = false;
        if (isRecord(persisted.layout)) {
            try {
                layoutRestored = this.#panelHost.restoreLayout(persisted.layout);
            } catch {
                // A CORRUPT layout blob (Dockview's `fromJSON` throws on an invalid grid — a downgrade,
                // a hand-edit, a schema change) degrades to the defaults `start` already opened, exactly
                // like the per-panel degrade below — REAL only because `restoreLayout` rolls the wipe
                // back: `fromJSON` clears the grid before it throws, so without that rollback this
                // catch would keep an empty dock while claiming to have kept the defaults (#474).
                // It must NOT abort the whole restore: an abort here
                // propagates to boot's catch, `attach` never runs, and persistence is dead for the
                // session with the bad blob never overwritten. Degrading instead lets `attach` run and
                // the next publish replace the blob — self-healing, per this module's failure-tolerant
                // contract.
                layoutRestored = false;
            }
        }
        let panelsRestored = 0;
        const degraded: string[] = [];
        // ⚠ THE KEYS ARE INSTANCE IDS SINCE editor-UX c3, and the C++ route needs BOTH halves of the
        // pair: `panel.state.set` takes the KIND plus the copy. `PanelHost.panelIdOf` answers from
        // the live entry when there is one and decomposes the id when there is not — which is the
        // case that matters here, because a blob may name a copy this build no longer opens (a panel
        // dropped from the roster, a `limited` kind whose ceiling shrank). A bare id written before
        // instances existed decomposes to itself, so an old document still restores onto the kind.
        for (const [id, blob] of Object.entries(persisted.panels)) {
            // A THIRD-PARTY `iframe` PANEL HAS NO C++ MODEL (M9 e13d), so `panel.state.set` has
            // nothing to apply for it — its blob is seeded into the PORT store instead, which is what
            // `bridge.state.get` then answers with. `undefined` means this is not a port panel and
            // the C++ route below is the right one; the two are exclusive by construction, so no blob
            // is applied twice. The restored/degraded accounting is identical either way, so a
            // package panel's stale blob shows up in the same `degraded` list a built-in's does.
            const seeded = this.#panelHost.seedPortState(id, blob);
            if (seeded !== undefined) {
                if (seeded) {
                    panelsRestored += 1;
                } else {
                    degraded.push(id);
                }
                continue;
            }
            const result = await this.#panelClient.setState(this.#panelHost.panelIdOf(id), blob, id);
            if (result === null) {
                // The panel is unknown to this build or persists no state — an ordinary outcome for a
                // blob left over from a panel this build cannot host (e05d3's Scene tree / Inspector).
                continue;
            }
            if (result.restored) {
                panelsRestored += 1;
            } else {
                degraded.push(id);
            }
        }
        if (layoutRestored || panelsRestored > 0) {
            // Reflect the restored model state in the mounted DOM (setState moved the C++ models; the
            // hydration runtime re-renders from them).
            await this.#panelHost.refreshAll();
        }
        return { layoutRestored, panelsRestored, degraded };
    }

    /**
     * Start persisting FUTURE changes. Call AFTER `restore` so restoring the layout does not
     * immediately re-publish it back.
     */
    attach(): void {
        const api = this.#panelHost.api;
        if (api !== null) {
            // Dock, split, tab, float, resize, panel add/remove all surface as one layout-change
            // event; the publish is debounced so a drag is one write.
            this.#layoutSub = api.onDidLayoutChange((): void => this.#schedule());
        }
        if (typeof window !== "undefined") {
            const onPageHide = (): void => this.#flush();
            window.addEventListener("pagehide", onPageHide);
            this.#onPageHide = onPageHide;
        }
    }

    /** TRIGGER 1 — debounce a burst of layout changes into one publish. */
    #schedule(): void {
        if (this.#disposed) {
            return;
        }
        if (this.#timer !== null) {
            clearTimeout(this.#timer);
        }
        this.#timer = setTimeout((): void => {
            this.#timer = null;
            void this.#publish();
        }, this.#debounceMs);
    }

    /**
     * TRIGGER 2 — on exit, cancel the pending debounce and publish immediately.
     *
     * Best-effort: the bridge call is async and a `pagehide` may cut it short. The DURABILITY
     * guarantee is the Shell's — its store `flush_now()` on shutdown writes whatever was last
     * published, and the debounced publishes already landed are a crash-safe last-known-good.
     */
    #flush(): void {
        if (this.#timer !== null) {
            clearTimeout(this.#timer);
            this.#timer = null;
        }
        void this.#publish();
    }

    async #publish(): Promise<void> {
        if (this.#disposed) {
            return;
        }
        const layout = this.#panelHost.captureLayout();
        if (layout === null || layout === undefined) {
            return; // no docking root up: nothing to persist
        }
        // Gather every mounted panel's D6 blob CONCURRENTLY: each getState is an independent bridge
        // round-trip and the results land in an order-independent map, so N sequential round-trip
        // latencies collapse to ~1 — which matters most on the `pagehide` flush, where a serialized
        // chain is the likeliest to be cut short before the last panel is read.
        const panels: Record<string, unknown> = {};
        await Promise.all(
            // `mounted` names COPIES (c3), so the published map is keyed by instance id and a
            // reload restores each copy's own blob — which is what "state is persisted per instance"
            // means. Two viewports save two entries; before c3 the second would have overwritten the
            // first under one shared key.
            [...this.#panelHost.mounted].map(async (id): Promise<void> => {
                // The PORT store first (M9 e13d) — a third-party `iframe` panel's blob lives on this
                // side of the bridge, so asking the Shell for it would answer `null` for every
                // package panel and the reload would preserve nothing. Synchronous and local, so a
                // package panel costs this gather no round trip AT ALL: `undefined` is the only
                // answer that falls through to one, and it means "not a port panel".
                const port = this.#panelHost.portState(id);
                if (port !== undefined) {
                    if (port !== null) {
                        // `null` = a port panel holding nothing. Publishing no entry for it matches
                        // what the C++ arm below does with a `null` state, and skipping the
                        // round trip is the whole reason the two are distinguishable at all
                        // (panelhost.ts § `portState`, the three answers).
                        panels[id] = port;
                    }
                    return;
                }
                const state = await this.#panelClient.getState(this.#panelHost.panelIdOf(id), id);
                if (state !== null && state !== undefined) {
                    panels[id] = state;
                }
            }),
        );
        // The layout blob and the region map ride the SAME layout-change signal (03 §6) but are two
        // independent publishes, so send both at once rather than one after the other. The Shell
        // replaces its per-window region map wholesale, so a removed viewport's rect never outlives
        // it. Since a2 the provider is the titlebar's chrome regions (chrome.ts — empty only when
        // no strips mounted or the mode has no drag duty); e11's viewport rects join the same set.
        await Promise.all([
            this.#stateClient.publish(layout, panels),
            this.#stateClient.publishRegions(this.#regionProvider()),
        ]);
    }

    /** Stop persisting and release every listener. Idempotent. */
    dispose(): void {
        this.#disposed = true;
        if (this.#timer !== null) {
            clearTimeout(this.#timer);
            this.#timer = null;
        }
        if (this.#layoutSub !== null) {
            this.#layoutSub.dispose();
            this.#layoutSub = null;
        }
        if (this.#onPageHide !== null && typeof window !== "undefined") {
            window.removeEventListener("pagehide", this.#onPageHide);
            this.#onPageHide = null;
        }
    }
}
