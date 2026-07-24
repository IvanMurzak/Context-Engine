// The docking engine's typed surface (M9 e05d1, design 04 §2 / D2).
//
// WHY THIS FILE EXISTS AT ALL. `dockview-core@7.0.2` is acquired through the SHA-pinned,
// verify-before-use fetch channel (`tools/dockview-toolchain.json` -> `tools/fetch_dockview.py`),
// which stages exactly two members beside the bundle: `dockview-core.min.js` and `dockview.css`.
// It stages NO TypeScript declarations — and it must not start doing so lightly, because every
// additional member is another pinned artifact in the supply chain. So the type surface is declared
// HERE, by hand, covering EXACTLY the API this app calls and nothing more.
//
// That is a feature rather than a compromise. A hand-written surface is a written-down statement of
// how much of Dockview we depend on, which is precisely what the D2 fallback plan needs: if Dockview
// ever has to be swapped for Golden Layout or Lumino (04 §2), the blast radius is this file plus
// `panelhost.ts`, and it is legible at a glance instead of being discovered by compiler errors
// across the app. Keep it MINIMAL; every member added here widens the fallback cost.
//
// TRANSPORT: the staged file is a UMD bundle (`umd_global: "dockview-core"` in the manifest), so
// loading it as a plain same-origin `<script>` publishes `globalThis["dockview-core"]`. It is
// deliberately NOT bundled by esbuild: the pinned artifact ships to the renderer BYTE-IDENTICAL to
// what `fetch_dockview.py` SHA-verified, and the `webui-assets` ctest re-hashes it in place. Passing
// it through the bundler would make the shipped bytes a build product rather than the verified
// artifact, quietly voiding that check.
//
// ⚠ THE POPOUT API IS DELIBERATELY UNUSED (B-F2, ratified at s1). Dockview v7 rejects non-http(s)
// popout URLs before `window.open` ever fires, and its popout is opener-owned DOM transfer — which
// is incompatible with the independent per-window editor-core instances design 04 §1 requires.
// OS-window tear-out is a PanelHost/Shell mechanism arriving in e10. Nothing below names `popout`,
// and nothing should start to.

import { isRecord } from "./bridge.js";

/** The theme descriptor Dockview v7 takes as an option (`{name, className, colorScheme}`). */
export interface DockviewTheme {
    readonly name: string;
    readonly className: string;
    readonly colorScheme: string;
}

/**
 * The content renderer Dockview asks us for, once per panel.
 *
 * `element` is the node Dockview inserts into the panel's frame — the DOM slot the hydration runtime
 * mounts into (04 §4 step 2). `init` / `dispose` are the lifecycle hooks PanelHost uses to bind and
 * release a panel; `onShow` / `onHide` are Dockview's visibility notifications, which PanelHost maps
 * onto the SUSPEND half of the panel lifecycle.
 */
export interface DockviewContentRenderer {
    readonly element: HTMLElement;
    /**
     * REQUIRED, not optional. Dockview-core@7 calls `content.init(params)` UNCONDITIONALLY inside
     * `addPanel` (verified against the pinned 7.0.2 bundle — it is the ONLY lifecycle method Dockview
     * invokes on a content renderer; `layout`/`update`/`focus`/`dispose` are not). A renderer that
     * omits it throws `TypeError: this.content.init is not a function` synchronously, aborting
     * `PanelHost.start()` after `panel.list` but before any `panel.render`. Declaring it optional
     * here is what let that ship: the type gate accepted a renderer with no `init`, and no local test
     * exercises this against real Dockview. Keep it required so the compiler refuses the regression.
     */
    init(params: DockviewPanelInitParameters): void;
    onShow?(): void;
    onHide?(): void;
    dispose?(): void;
}

/** What Dockview hands a renderer at `init`. Only the members this app reads are declared. */
export interface DockviewPanelInitParameters {
    readonly params?: Record<string, unknown>;
}

/** The `{id, name}` pair Dockview passes to `createComponent`. */
export interface DockviewComponentRequest {
    readonly id: string;
    readonly name: string;
}

/** Where a newly added panel goes relative to the existing arrangement. */
export interface DockviewPanelPosition {
    readonly referencePanel?: string;
    readonly direction?: "left" | "right" | "above" | "below" | "within";
}

/** The options `addPanel` accepts (the subset PanelHost sets). */
export interface DockviewAddPanelOptions {
    readonly id: string;
    readonly component: string;
    readonly title?: string;
    readonly position?: DockviewPanelPosition;
    readonly params?: Record<string, unknown>;
}

/** A live panel handle. */
export interface DockviewPanelHandle {
    readonly id: string;
}

/** A Dockview event registration; calling `dispose` detaches the listener. */
export interface DockviewDisposable {
    dispose(): void;
}

/** The `DockviewApi` surface this app uses. */
export interface DockviewApi {
    readonly panels: readonly DockviewPanelHandle[];
    addPanel(options: DockviewAddPanelOptions): DockviewPanelHandle;
    getPanel(id: string): DockviewPanelHandle | undefined;
    removePanel(panel: DockviewPanelHandle): void;
    /**
     * Float an existing panel's group inside THIS window (M9 e10b) — Dockview's IN-WINDOW floating,
     * which is geometry and therefore in scope (NOT popout, B-F2). The loud-degradation home when a
     * secondary-window create fails (03 §7): the torn-out panel becomes a floating group in the
     * source window instead of silently doing nothing. `dockview-core` renders a floating group with
     * an INLINE `position:absolute` on its container — which is what makes "it floated" observable on
     * the rendered output rather than reasoned from a selector (panelhost.ts / the live smoke verify
     * the RENDERED result, standing lesson #3).
     */
    addFloatingGroup(panel: DockviewPanelHandle): void;
    layout(width: number, height: number): void;
    toJSON(): unknown;
    fromJSON(state: unknown): void;
    clear(): void;
    dispose(): void;
    onDidLayoutChange(listener: () => void): DockviewDisposable;
}

/** The options `createDockview` accepts (the subset PanelHost sets). */
export interface DockviewOptions {
    createComponent(request: DockviewComponentRequest): DockviewContentRenderer;
    readonly theme?: DockviewTheme;
    readonly disableFloatingGroups?: boolean;
}

/** The module surface the UMD global publishes. */
export interface DockviewModule {
    createDockview(parent: HTMLElement, options: DockviewOptions): DockviewApi;
    readonly themeDark: DockviewTheme;
}

/**
 * The UMD global's name. Mirrors `umd_global` in `tools/dockview-toolchain.json` — the manifest is
 * the authority, and a change there without a change here would leave the app unable to find an
 * engine that loaded perfectly.
 */
export const DOCKVIEW_GLOBAL = "dockview-core";

/** The staged script member, relative to the asset root. Mirrors the manifest's `members` key. */
export const DOCKVIEW_SCRIPT = "dockview-core.min.js";

/**
 * Read the docking engine off a global object.
 *
 * Returns `undefined` rather than throwing when it is absent — the same discipline
 * `detectBridgeQuery` follows, and for the same reason: the bundle must stay loadable outside the
 * Shell (a plain browser, a harness) instead of dying at import time with a stack trace nobody can
 * act on. PanelHost degrades to a diagnosable "no docking engine" state instead.
 *
 * The shape is CHECKED, not assumed: a global that exists but does not carry `createDockview` is a
 * stale or wrong artifact, and treating it as valid would surface as a `TypeError` deep inside a
 * mount rather than as the honest "the docking engine did not load" this reports.
 */
export function detectDockview(scope: unknown = globalThis): DockviewModule | undefined {
    if (!isRecord(scope)) {
        return undefined;
    }
    const candidate = scope[DOCKVIEW_GLOBAL];
    if (!isRecord(candidate) || typeof candidate["createDockview"] !== "function") {
        return undefined;
    }
    return candidate as unknown as DockviewModule;
}
