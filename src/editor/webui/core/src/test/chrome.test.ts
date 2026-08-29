// T1 for the window-chrome STRIPS (editor-window-chrome a2, target design 02 §2 / §6).
//
// WHAT THIS FILE MUST PROVE, per the task's own DoD: the strip DOM + chrome-mode gating + the
// max/restore glyph flip across ALL THREE modes and BOTH welcome/project boots, the regionProvider's
// PHYSICAL-px measurement with caption-before-controls ordering, and the publisher's three triggers
// (initial/layout, resize, DPI). Every live backend reports `system` until b1/c1 land (interim
// honesty, tasks/README.md), so the `custom`/`hybrid` cases here INJECT `chrome.state` values — this
// tier is the ONLY place those modes are exercised at all until then, which is exactly why the task
// demands they be DOM-tested now.
//
// The harness serves the REAL app.css (harness.html), so the fixture strips get the SHIPPED layout
// (38px titlebar, flex row, the hybrid inset padding rule) and `getBoundingClientRect` measures real
// geometry — the physical-px assertions are against what the browser actually laid out, not fixture
// arithmetic. The two boot cases drive the REAL `bootEditorCore` against a mock Shell (the
// boot.test.ts discipline): a regression that unwired `startChromeStrips` from boot would stay green
// in every mount-level case and red exactly there.

import { assert, assertEqual, delay, waitFor, type TestCase } from "./harness.js";
import { ShellBridge } from "../bridge.js";
import { EDITOR_ROOT_ID, bootEditorCore } from "../boot.js";
import { baseAnswers, mockShell } from "./boot.test.js";
import {
    CHROME_CONTROL_ATTRIBUTE,
    CHROME_INSET_LEFT_PROPERTY,
    CHROME_INSET_RIGHT_PROPERTY,
    CHROME_MAXIMIZED_ATTRIBUTE,
    CHROME_MODE_ATTRIBUTE,
    CHROME_REGION_CAPTION_ID,
    CHROME_REGION_CLOSE_ID,
    CHROME_REGION_MAX_ID,
    CHROME_REGION_MIN_ID,
    CHROME_STRIPS_ATTRIBUTE,
    ChromeRegionPublisher,
    DEFAULT_TITLE,
    EDITOR_PLAYBAR_ID,
    EDITOR_STATUSBAR_ID,
    EDITOR_TITLEBAR_ID,
    LABEL_CLOSE,
    LABEL_MAXIMIZE,
    LABEL_MINIMIZE,
    LABEL_PALETTE,
    LABEL_RESTORE,
    PLAYBAR_CLASS,
    STATUSBAR_CLASS,
    TITLEBAR_CLASS,
    TITLEBAR_DRAG_CLASS,
    TITLEBAR_TITLE_CLASS,
    mountChrome,
    parseChromeFact,
    subscribeChromeFacts,
    type ChromeMount,
    type ChromeStripElements,
    type ChromeWindowControls,
    type MediaQueryListLike,
    type ResizeTargetLike,
} from "../chrome.js";
import {
    REGION_KIND_CAPTION,
    REGION_KIND_CAPTION_CLOSE,
    REGION_KIND_CAPTION_MAX,
    REGION_KIND_CAPTION_MIN,
    type ShellRegion,
} from "../editorstate.js";
import { PALETTE_TOGGLE_COMMAND_ID } from "../palette.js";
import { EditorUiBus, UI_TOPIC_CHROME } from "../uibus.js";
import {
    CHROME_MODE_CUSTOM,
    CHROME_MODE_HYBRID,
    CHROME_MODE_SYSTEM,
    CHROME_WINDOW_PRIMARY,
    type ChromeState,
    type ToggleMaximizeResult,
} from "../window.js";

// ------------------------------------------------------------------------------- the fixtures

/** The strip elements as `app/index.html` ships them, appended to the shared harness document. */
function stripFixture(): { elements: ChromeStripElements; dispose(): void } {
    const titlebar = document.createElement("header");
    titlebar.id = EDITOR_TITLEBAR_ID;
    titlebar.className = TITLEBAR_CLASS;
    const playbar = document.createElement("div");
    playbar.id = EDITOR_PLAYBAR_ID;
    playbar.className = PLAYBAR_CLASS;
    const statusbar = document.createElement("footer");
    statusbar.id = EDITOR_STATUSBAR_ID;
    statusbar.className = STATUSBAR_CLASS;
    document.body.append(titlebar, playbar, statusbar);
    return {
        elements: { titlebar, playbar, statusbar },
        dispose: (): void => {
            titlebar.remove();
            playbar.remove();
            statusbar.remove();
            document.documentElement.removeAttribute(CHROME_STRIPS_ATTRIBUTE);
        },
    };
}

function chromeState(mode: ChromeState["mode"], overrides?: Partial<ChromeState>): ChromeState {
    return {
        mode,
        controlsInset: { left: 0, right: 0 },
        maximized: false,
        focused: true,
        window: CHROME_WINDOW_PRIMARY,
        ...overrides,
    };
}

/** A recording stand-in for the a1 window-control surface. */
function controlsSpy(toggleResult: ToggleMaximizeResult = { accepted: true, maximized: true }): {
    controls: ChromeWindowControls;
    calls: string[];
} {
    const calls: string[] = [];
    return {
        calls,
        controls: {
            minimize: (): Promise<unknown> => {
                calls.push("minimize");
                return Promise.resolve({ accepted: true });
            },
            toggleMaximize: (): Promise<ToggleMaximizeResult> => {
                calls.push("toggle-maximize");
                return Promise.resolve(toggleResult);
            },
            close: (): Promise<unknown> => {
                calls.push("close");
                return Promise.resolve({ closed: true });
            },
        },
    };
}

interface MountHarness {
    readonly mount: ChromeMount;
    readonly elements: ChromeStripElements;
    readonly calls: string[];
    readonly executed: string[];
    dispose(): void;
}

function mountHarness(
    mode: ChromeState["mode"],
    options?: {
        readonly stateOverrides?: Partial<ChromeState>;
        readonly projectName?: string;
        readonly welcome?: boolean;
        readonly devicePixelRatio?: () => number;
        readonly toggleResult?: ToggleMaximizeResult;
    },
): MountHarness {
    const fixture = stripFixture();
    const spy = controlsSpy(options?.toggleResult ?? { accepted: true, maximized: true });
    const executed: string[] = [];
    const mount = mountChrome(fixture.elements, {
        state: chromeState(mode, options?.stateOverrides),
        projectName: options?.projectName ?? "Sprocket Quest",
        welcome: options?.welcome ?? false,
        controls: spy.controls,
        executeCommand: (commandId: string): void => {
            executed.push(commandId);
        },
        ...(options?.devicePixelRatio === undefined
            ? {}
            : { devicePixelRatio: options.devicePixelRatio }),
    });
    return {
        mount,
        elements: fixture.elements,
        calls: spy.calls,
        executed,
        dispose: (): void => {
            fixture.dispose();
        },
    };
}

function controlButton(titlebar: HTMLElement, control: string): HTMLButtonElement {
    const button = titlebar.querySelector<HTMLButtonElement>(
        `[${CHROME_CONTROL_ATTRIBUTE}="${control}"]`,
    );
    assert(button !== null, `the ${control} control renders`);
    return button as HTMLButtonElement;
}

/** A hand-cranked resize target, so the trigger fires deterministically. */
function fakeResizeTarget(): { target: ResizeTargetLike; fire(): void; armed(): boolean } {
    let listener: (() => void) | null = null;
    return {
        target: {
            addEventListener: (_type: "resize", handler: () => void): void => {
                listener = handler;
            },
            removeEventListener: (_type: "resize", handler: () => void): void => {
                if (listener === handler) {
                    listener = null;
                }
            },
        },
        fire: (): void => {
            listener?.();
        },
        armed: (): boolean => listener !== null,
    };
}

/** A hand-cranked matchMedia, recording each armed query — the re-arm observable. */
function fakeMatchMedia(): {
    factory: (query: string) => MediaQueryListLike;
    queries: string[];
    fire(): void;
} {
    const queries: string[] = [];
    let active: (() => void) | null = null;
    return {
        queries,
        factory: (query: string): MediaQueryListLike => {
            queries.push(query);
            return {
                addEventListener: (_type: "change", listener: () => void): void => {
                    active = listener;
                },
                removeEventListener: (_type: "change", listener: () => void): void => {
                    if (active === listener) {
                        active = null;
                    }
                },
            };
        },
        fire: (): void => {
            active?.();
        },
    };
}

// ------------------------------------------------------------------------------- the cases

export const chromeTests: readonly TestCase[] = [
    {
        name: "chrome: custom mode renders the full titlebar — brand, title, palette, labelled controls",
        run: () => {
            const h = mountHarness(CHROME_MODE_CUSTOM);
            try {
                const titlebar = h.elements.titlebar;
                assertEqual(
                    titlebar.getAttribute(CHROME_MODE_ATTRIBUTE),
                    CHROME_MODE_CUSTOM,
                    "the mode gates via the DOM attribute",
                );
                assert(
                    titlebar.querySelector(`.${TITLEBAR_DRAG_CLASS}`) !== null,
                    "the caption drag surface renders",
                );
                assertEqual(
                    titlebar.querySelector(`.${TITLEBAR_TITLE_CLASS}`)?.textContent,
                    "Sprocket Quest",
                    "the project name is the title",
                );
                // The palette opener (01 §7: the first reliable, non-programmatic one).
                const palette = titlebar.querySelector<HTMLButtonElement>(
                    `[data-command="${PALETTE_TOGGLE_COMMAND_ID}"]`,
                );
                assert(palette !== null, "the palette button renders");
                assertEqual(
                    palette?.getAttribute("aria-label"),
                    LABEL_PALETTE,
                    "…with its accessible label (the banners ARIA discipline)",
                );
                palette?.click();
                assertEqual(
                    h.executed,
                    [PALETTE_TOGGLE_COMMAND_ID],
                    "…dispatching the palette toggle through the late-bound registry",
                );
                // The controls cluster: a labelled group of three labelled kit buttons.
                const cluster = titlebar.querySelector('[role="group"]');
                assertEqual(
                    cluster?.getAttribute("aria-label"),
                    "Window controls",
                    "the cluster is a labelled group",
                );
                assertEqual(
                    controlButton(titlebar, "minimize").getAttribute("aria-label"),
                    LABEL_MINIMIZE,
                    "minimize carries its label",
                );
                assertEqual(
                    controlButton(titlebar, "maximize").getAttribute("aria-label"),
                    LABEL_MAXIMIZE,
                    "maximize carries its label",
                );
                assertEqual(
                    controlButton(titlebar, "close").getAttribute("aria-label"),
                    LABEL_CLOSE,
                    "close carries its label",
                );
                // Every control is a KIT component (02 §2: no new family, the kit is the authority).
                for (const control of ["minimize", "maximize", "close"]) {
                    assert(
                        controlButton(titlebar, control).classList.contains("ctx-button"),
                        `the ${control} control is a kit button`,
                    );
                }
            } finally {
                h.dispose();
            }
        },
    },
    {
        name: "chrome: the controls dispatch the a1 window verbs, and the toggle result flips the glyph",
        run: async () => {
            const h = mountHarness(CHROME_MODE_CUSTOM, {
                toggleResult: { accepted: true, maximized: true },
            });
            try {
                const titlebar = h.elements.titlebar;
                controlButton(titlebar, "minimize").click();
                assertEqual(h.calls, ["minimize"], "minimize dispatched window.minimize");
                assert(!h.mount.isMaximized(), "the glyph starts on the boot snapshot (restored)");
                controlButton(titlebar, "maximize").click();
                await waitFor("the toggle result to flip the glyph", () => h.mount.isMaximized());
                assertEqual(
                    controlButton(titlebar, "maximize").getAttribute("aria-label"),
                    LABEL_RESTORE,
                    "the accepted toggle's NEW state relabels the button",
                );
                controlButton(titlebar, "close").click();
                assertEqual(
                    h.calls,
                    ["minimize", "toggle-maximize", "close"],
                    "each control dispatched exactly its verb",
                );
            } finally {
                h.dispose();
            }
        },
    },
    {
        name: "chrome: a REFUSED toggle leaves the glyph alone (the honest unbound-Shell degrade)",
        run: async () => {
            const h = mountHarness(CHROME_MODE_CUSTOM, {
                toggleResult: { accepted: false, maximized: true },
            });
            try {
                controlButton(h.elements.titlebar, "maximize").click();
                await delay(20);
                assert(
                    !h.mount.isMaximized(),
                    "an accepted:false toggle (no window bound) must not pretend the state moved",
                );
            } finally {
                h.dispose();
            }
        },
    },
    {
        name: "chrome: hybrid mode — no controls, inset padding measured from physical px / dpr",
        run: () => {
            const h = mountHarness(CHROME_MODE_HYBRID, {
                stateOverrides: { controlsInset: { left: 72, right: 8 } },
                devicePixelRatio: () => 2,
            });
            try {
                const titlebar = h.elements.titlebar;
                assertEqual(
                    titlebar.getAttribute(CHROME_MODE_ATTRIBUTE),
                    CHROME_MODE_HYBRID,
                    "the mode attribute gates the stylesheet",
                );
                assert(
                    titlebar.querySelector(`[${CHROME_CONTROL_ATTRIBUTE}]`) === null,
                    "hybrid renders NO window controls (the OS draws the traffic lights)",
                );
                assertEqual(
                    titlebar.style.getPropertyValue(CHROME_INSET_LEFT_PROPERTY),
                    "36px",
                    "the PHYSICAL 72px inset lands as 36 CSS px at dpr 2",
                );
                assertEqual(
                    titlebar.style.getPropertyValue(CHROME_INSET_RIGHT_PROPERTY),
                    "4px",
                    "…and the right inset likewise",
                );
                // The SHIPPED stylesheet rule actually consumes the property (harness serves the
                // real app.css) — a custom property nothing reads would be a silent no-op.
                assertEqual(
                    getComputedStyle(titlebar).paddingLeft,
                    "36px",
                    "the computed padding reserves the inset",
                );
                // Hybrid still has drag duty: the caption region publishes, without control rects.
                const regions = h.mount.regions();
                assertEqual(regions.length, 1, "hybrid publishes exactly the caption region");
                assertEqual(regions[0]?.id, CHROME_REGION_CAPTION_ID, "…the caption");
            } finally {
                h.dispose();
            }
        },
    },
    {
        name: "chrome: system mode — menu-bar-only strip, no controls and NO drag duty (empty regions)",
        run: () => {
            const h = mountHarness(CHROME_MODE_SYSTEM);
            try {
                assert(
                    h.elements.titlebar.querySelector(`[${CHROME_CONTROL_ATTRIBUTE}]`) === null,
                    "system renders no window controls (D6: the WM owns the frame)",
                );
                assertEqual(
                    h.mount.regions().length,
                    0,
                    "system publishes NO regions — no drag duty is a fact the Shell must see",
                );
            } finally {
                h.dispose();
            }
        },
    },
    {
        name: "chrome: the welcome boot hides the play-bar slot; the project boot shows it",
        run: () => {
            const welcome = mountHarness(CHROME_MODE_SYSTEM, { welcome: true, projectName: "" });
            try {
                assert(
                    welcome.elements.playbar.hidden === true,
                    "no session to control on the front door",
                );
                assertEqual(
                    welcome.elements.titlebar.querySelector(`.${TITLEBAR_TITLE_CLASS}`)
                        ?.textContent,
                    DEFAULT_TITLE,
                    "with no project name the title falls back to the product name",
                );
            } finally {
                welcome.dispose();
            }
            const project = mountHarness(CHROME_MODE_SYSTEM, { welcome: false });
            try {
                assert(!project.elements.playbar.hidden, "the project boot shows the empty slot");
            } finally {
                project.dispose();
            }
        },
    },
    {
        name: "chrome: regions are PHYSICAL px, caption first, controls after and outside the caption",
        run: () => {
            const dpr = 2;
            const h = mountHarness(CHROME_MODE_CUSTOM, { devicePixelRatio: () => dpr });
            try {
                const regions = h.mount.regions();
                assertEqual(
                    regions.map((region: ShellRegion): string => region.id),
                    [
                        CHROME_REGION_CAPTION_ID,
                        CHROME_REGION_MIN_ID,
                        CHROME_REGION_MAX_ID,
                        CHROME_REGION_CLOSE_ID,
                    ],
                    "caption publishes FIRST, the controls after (the 02 §6 arbitration order)",
                );
                assertEqual(
                    regions.map((region: ShellRegion): string => region.kind),
                    [
                        REGION_KIND_CAPTION,
                        REGION_KIND_CAPTION_MIN,
                        REGION_KIND_CAPTION_MAX,
                        REGION_KIND_CAPTION_CLOSE,
                    ],
                    "each region carries its closed-vocabulary kind",
                );
                // PHYSICAL px: the CSS rect scaled by the injected ratio — the arithmetic a live
                // devicePixelRatio of 1 could never distinguish from "forgot the multiply".
                const drag = h.elements.titlebar.querySelector(`.${TITLEBAR_DRAG_CLASS}`);
                assert(drag !== null, "the drag surface exists to measure");
                const cssRect = (drag as HTMLElement).getBoundingClientRect();
                const caption = regions[0];
                assert(caption !== undefined, "the caption region exists");
                assertEqual(caption?.rect.x, Math.round(cssRect.left * dpr), "x is physical");
                assertEqual(caption?.rect.y, Math.round(cssRect.top * dpr), "y is physical");
                // Extents derive from the ROUNDED EDGES (chrome.ts § physicalRegion), so the
                // expectation here is the same arithmetic — `round(width · dpr)` would disagree by
                // one physical px whenever the CSS edges are fractional.
                assertEqual(
                    caption?.rect.width,
                    Math.round(cssRect.right * dpr) - Math.round(cssRect.left * dpr),
                    "width is physical (edge-rounded)",
                );
                assertEqual(
                    caption?.rect.height,
                    Math.round(cssRect.bottom * dpr) - Math.round(cssRect.top * dpr),
                    "height is physical (edge-rounded)",
                );
                // The controls sit OUTSIDE the caption rect — that is what makes carve-out tokens
                // unnecessary even before last-match-wins is consulted.
                const min = regions[1];
                assert(
                    min !== undefined &&
                        caption !== undefined &&
                        caption.rect.x + caption.rect.width <= min.rect.x,
                    "the caption drag surface ends before the first control begins",
                );
            } finally {
                h.dispose();
            }
        },
    },
    {
        name: "chrome: the glyph flips on the editor.ui.chrome fact — and only for THIS window",
        run: () => {
            const h = mountHarness(CHROME_MODE_CUSTOM);
            try {
                const bus = new EditorUiBus({ origin: "7" });
                subscribeChromeFacts(bus, 7, h.mount);
                bus.publish(UI_TOPIC_CHROME, { windowId: 3, maximized: true });
                assert(!h.mount.isMaximized(), "a peer window's fact must not flip this glyph");
                bus.publish(UI_TOPIC_CHROME, { windowId: 7, maximized: true });
                assert(h.mount.isMaximized(), "this window's fact flips the glyph");
                assertEqual(
                    h.elements.titlebar.getAttribute(CHROME_MAXIMIZED_ATTRIBUTE),
                    "true",
                    "…and the DOM reports it",
                );
                bus.publish(UI_TOPIC_CHROME, { windowId: 7, maximized: false });
                assert(!h.mount.isMaximized(), "the restore fact flips it back");
                // TOTAL against garbage: a malformed mirrored payload neither throws nor flips.
                bus.publish(UI_TOPIC_CHROME, "not a record");
                assert(!h.mount.isMaximized(), "garbage is dropped, not applied");
                assertEqual(parseChromeFact(null), null, "parseChromeFact: null in, null out");
                assertEqual(
                    parseChromeFact({ maximized: true }),
                    null,
                    "…no readable windowId, null out",
                );
                assertEqual(
                    parseChromeFact({ windowId: 2, maximized: "yes" }),
                    { windowId: 2, maximized: false },
                    "…a non-boolean maximized reads as false, never as true",
                );
            } finally {
                h.dispose();
            }
        },
    },
    {
        name: "chrome: the publisher fires on start, resize, and DPI change — and re-arms the DPI query",
        run: async () => {
            let dpr = 2;
            const h = mountHarness(CHROME_MODE_CUSTOM, { devicePixelRatio: () => dpr });
            const published: (readonly ShellRegion[])[] = [];
            const resize = fakeResizeTarget();
            const media = fakeMatchMedia();
            const publisher = new ChromeRegionPublisher({
                provider: (): readonly ShellRegion[] => h.mount.regions(),
                publish: (regions: readonly ShellRegion[]): Promise<boolean> => {
                    published.push(regions);
                    return Promise.resolve(true);
                },
                debounceMs: 1,
                devicePixelRatio: () => dpr,
                resizeTarget: resize.target,
                matchMedia: media.factory,
            });
            try {
                await publisher.start();
                assertEqual(publisher.publishes, 1, "start performs the initial publish");
                assert(
                    published[0]?.some(
                        (region: ShellRegion): boolean => region.id === CHROME_REGION_CAPTION_ID,
                    ) === true,
                    "…carrying the caption region wholesale",
                );
                assertEqual(
                    media.queries[0],
                    "(resolution: 2dppx)",
                    "the DPI trigger armed against the live ratio",
                );
                assert(resize.armed(), "the resize trigger armed");
                resize.fire();
                await waitFor("the resize republish", () => publisher.publishes === 2);
                dpr = 3;
                media.fire();
                await waitFor("the DPI republish", () => publisher.publishes === 3);
                assertEqual(
                    media.queries[media.queries.length - 1],
                    "(resolution: 3dppx)",
                    "the DPI trigger re-armed against the NEW ratio (a one-shot query re-listened)",
                );
                publisher.dispose();
                resize.fire();
                await delay(25);
                assertEqual(publisher.publishes, 3, "a disposed publisher publishes nothing");
                assert(!resize.armed(), "…and released its resize listener");
            } finally {
                publisher.dispose();
                h.dispose();
            }
        },
    },
    {
        name: "chrome: a PROJECT boot mounts the strips and publishes the region map over the bridge",
        run: async () => {
            const fixture = stripFixture();
            const root = document.createElement("main");
            root.id = EDITOR_ROOT_ID;
            document.body.append(root);
            const publishedRegions: unknown[] = [];
            // boot.test.ts's mock Shell (ONE copy of the deny-by-default envelope), with the a2
            // answers overriding its project-mode base and a hook recording what the region
            // channel actually carried.
            const shell = mockShell(
                {
                    ...baseAnswers(),
                    "shell.hello": { nonce: "a2-nonce" },
                    "welcome.state": { mode: "project", projectName: "Sprocket Quest" },
                    "chrome.state": {
                        mode: "custom",
                        controlsInset: { left: 0, right: 0 },
                        maximized: false,
                        focused: true,
                        window: "primary",
                    },
                    "editor.regions.publish": {},
                },
                (method: string, params: unknown): void => {
                    if (method === "editor.regions.publish") {
                        const regions = (params as { regions?: unknown[] } | undefined)?.regions;
                        publishedRegions.push(...(regions ?? []));
                    }
                },
            );
            try {
                const report = await bootEditorCore(new ShellBridge(shell.query));
                assert(report.ready, "the editor boots");
                // THE WIRING CLAIM: the REAL boot mounted the strips from the SERVED chrome state…
                assert(
                    fixture.elements.titlebar.querySelector(`[${CHROME_CONTROL_ATTRIBUTE}]`) !==
                        null,
                    "the served custom mode rendered the controls cluster",
                );
                assert(!fixture.elements.playbar.hidden, "the project boot shows the play-bar slot");
                // …and PUBLISHED the measured region map over the real bridge channel at boot —
                // the assertion that reds if startChromeStrips is unwired from bootEditorCore.
                assert(
                    publishedRegions.some(
                        (region: unknown): boolean =>
                            typeof region === "object" &&
                            region !== null &&
                            (region as { id?: unknown }).id === CHROME_REGION_CAPTION_ID &&
                            (region as { kind?: unknown }).kind === REGION_KIND_CAPTION,
                    ),
                    "boot published the caption region through editor.regions.publish",
                );
                const strips =
                    document.documentElement.getAttribute(CHROME_STRIPS_ATTRIBUTE) ?? "";
                assert(
                    strips.includes("mode=custom") && strips.includes("playbar=visible"),
                    `the data-editor-strips report names the mounted state, got: ${strips}`,
                );
            } finally {
                fixture.dispose();
                root.remove();
            }
        },
    },
    {
        name: "chrome: a WELCOME boot renders the strips too, with the play-bar slot hidden",
        run: async () => {
            const fixture = stripFixture();
            const root = document.createElement("main");
            root.id = EDITOR_ROOT_ID;
            document.body.append(root);
            const shell = mockShell({
                ...baseAnswers(),
                "shell.hello": { nonce: "a2-welcome-nonce" },
                "welcome.state": { mode: "welcome", recents: [], templates: [] },
                "chrome.state": {
                    mode: "system",
                    controlsInset: { left: 0, right: 0 },
                    maximized: false,
                    focused: true,
                    window: "primary",
                },
                "editor.regions.publish": {},
            });
            try {
                const report = await bootEditorCore(new ShellBridge(shell.query));
                assert(report.ready, "the front door boots");
                assert(
                    root.querySelector(".welcome-screen") !== null,
                    "the welcome screen mounted in the dock slot",
                );
                assert(
                    fixture.elements.titlebar.getAttribute(CHROME_MODE_ATTRIBUTE) === "system",
                    "the strips render on the front door too — the frame is the app's frame",
                );
                assert(
                    fixture.elements.playbar.hidden === true,
                    "…with the play-bar slot hidden (no session to control)",
                );
                // d1: the strip CONTENT is not mounted on the welcome path either — hidden is the
                // a2 gate, empty is d1's (startPlaybar runs on the editor path only).
                assertEqual(
                    fixture.elements.playbar.children.length,
                    0,
                    "the welcome boot mounts no transport into the hidden slot",
                );
            } finally {
                fixture.dispose();
                root.replaceChildren();
                root.remove();
            }
        },
    },
];
