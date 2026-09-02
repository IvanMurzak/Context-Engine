// T1 for the Scene viewport's editor-core half (M9 editor-UX e3, D7): the DOM HOLE, the `"always"`
// renderer that keeps it in the document, and the PHYSICAL-px rect the Shell composites against.
//
// ⚠ THE SCALE CASES ARE THE POINT. The set's gate is that a geometry assertion at device scale 1.0 is
// VACUOUS — physical and CSS pixels coincide there, so the correct code and the code that forgot to
// multiply are byte-identical. Every rect case below therefore runs at 1.0 AND at 1.5 / 2 / 3, over
// a fixture with FRACTIONAL CSS edges, and asserts the 1.0 result and the scaled result DIFFER — so
// the scaled half cannot be satisfied by an implementation the 1.0 half already accepts. A separate
// case pins the edge-rounding rule against the plausible-but-wrong `round(width · dpr)`, which
// agrees at every integral scale and disagrees exactly where it matters.
//
// The mounted cases use REAL Dockview in a REAL browser, for the reason extpanel.test.ts states
// about its frames: "the renderer is pinned to always" is a claim about what the ENGINE does with a
// panel, and an assertion on the option value would prove only what someone wrote. What is asserted
// instead is the CONSEQUENCE — an inactive viewport is still in the document and still measures —
// with the ordinary `uitree` panel beside it as the control that proves Dockview really does detach
// the ones it is allowed to.

import { assert, assertEqual, waitFor, type TestCase } from "./harness.js";
import { ShellBridge, type BridgeQuery, type BridgeQueryFunction } from "../bridge.js";
import { ChromeRegionPublisher } from "../chrome.js";
import { detectDockview } from "../dockview.js";
import { REGION_KIND_VIEWPORT, type ShellRegion } from "../editorstate.js";
import { PanelHost } from "../panelhost.js";
import { PANEL_LIST_METHOD, PanelClient, parsePanelManifest } from "../panels.js";
import {
    PANEL_INSTANCE_ATTRIBUTE,
    VIEWPORT_DOCK_ATTRIBUTE,
    VIEWPORT_DOCUMENT_ATTRIBUTE,
    VIEWPORT_PANEL_ID,
    VIEWPORT_SURFACE_ATTRIBUTE,
    isNativeSurfacePanel,
    isNativeSurfacePanelId,
    viewportRegions,
} from "../viewport.js";

// ------------------------------------------------------------------------------- the roster mock

/**
 * One `panel.list` entry in the WIRE shape `PanelHost::list()` emits (panel_host.cpp).
 *
 * ⚠ `zone` IS EXPLICIT AND LOAD-BEARING, not decoration. `PanelHost.open` derives BOTH the Dockview
 * direction and (since e3) the reference panel from it, so a fixture that leaves every panel in one
 * zone is asserting a single-group arrangement — see the real-roster case at the bottom of this file
 * for why that is a fixture choice rather than a fact about the editor.
 */
function manifestJson(
    id: string,
    mode: "singleton" | "unlimited" = "singleton",
    zone = "center",
    hosted = true,
    contentType = "uitree",
): Record<string, unknown> {
    return {
        id,
        kind: "panel",
        title: id,
        icon: "",
        contractVersion: 3,
        dock: { zone, minWidth: 0, minHeight: 0 },
        instances: { mode, max: 0 },
        path: "Scene",
        content: { type: contentType, entry: "" },
        state: { schemaVersion: 1 },
        capabilities: [],
        commands: [],
        hosted,
        gestures: false,
        persists: false,
        revision: 1,
    };
}

/**
 * THE REAL ROSTER, in the REAL order, with the REAL zones and the REAL hosted set — transcribed from
 * `src/editor/gui/contract/src/builtin_roster.cpp` (`build_roster`, which `panel_host.cpp:615`
 * serves to `panel.list` IN ORDER) and `src/editor/shell/panels/src/builtin_panels.cpp`
 * (`hostable_panel_ids`, which is exactly what the `hosted` bit reports).
 *
 * WHY A REAL FIXTURE AND NOT A SYNTHETIC ONE: the defect class this pins is "mounting panel N
 * displaces panel M from the DOM", and in a synthetic roster the author picks both N and M — so the
 * victim is always a panel the test was willing to lose. Only the shipped order, zones and hosted
 * set decide who actually gets displaced. That is the difference between the case below and the
 * `always`-renderer case above, which is deliberately synthetic because its claim is about Dockview.
 *
 * The C++ side is pinned by `context_gui_contract_test_roster`; if this drifts from it, the mounted
 * set assertion in the case below is what says so.
 */
function realRosterJson(): Record<string, unknown>[] {
    // id, zone, hosted, contentType — the four fields placement and mountability read.
    const entries: readonly (readonly [string, string, boolean, string])[] = [
        ["placeholder", "center", true, "uitree"],
        ["builtin.scene-tree", "left", true, "uitree"],
        ["builtin.files", "left", true, "uitree"],
        ["builtin.inspector", "right", true, "uitree"],
        ["builtin.viewport", "center", true, "uitree"],
        ["builtin.problems", "bottom", true, "uitree"],
        ["builtin.tilemap-painter", "right", false, "uitree"],
        ["builtin.viewport-edit", "right", false, "uitree"],
        ["builtin.help", "right", false, "uitree"],
        ["builtin.session.undo", "bottom", true, "uitree"],
        // The ONE `local` panel. Unmountable in this harness (no factory is registered), which is
        // also true of any window whose bundle lacks one — and it is last in roster order either
        // way, so it cannot participate in the placement this case is about.
        ["builtin.settings", "right", true, "local"],
    ];
    return entries.map(([id, zone, hosted, contentType]) =>
        manifestJson(
            id,
            id === "builtin.viewport" ? "unlimited" : "singleton",
            zone,
            hosted,
            contentType,
        ),
    );
}

/** A Shell serving one roster and refusing everything else, exactly as the real router's default. */
function mockShell(panels: readonly Record<string, unknown>[]): ShellBridge {
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
    readonly container: HTMLElement;
    dispose(): void;
}

async function mountHost(
    panels: readonly Record<string, unknown>[],
    // Handed the CONTAINER, because every report worth recording is a measurement taken against it —
    // and the caller cannot close over `mountHost`'s return value: the first reports are delivered
    // from inside `host.start()`, while that binding is still in its temporal dead zone.
    onArrangementChanged?: (container: HTMLElement) => void,
): Promise<Mounted> {
    const dockview = detectDockview();
    assert(
        dockview !== undefined,
        "the pinned dockview-core UMD global is loaded — harness.html must load " +
            "dockview-core.min.js before the test bundle, or this whole file passes vacuously",
    );
    const dv = dockview as NonNullable<typeof dockview>;
    const container = window.document.createElement("div");
    // FIXED at the origin, and SMALL ENOUGH TO FIT. `elementsFromPoint` answers in VIEWPORT
    // coordinates, so a dock pushed below the fold by whatever else the harness page holds — or one
    // simply larger than the harness window (measured 750x438 in the headless runner) — hit-tests to
    // nothing, and the paint-stack case would then pass vacuously on an empty stack. The case
    // asserts the stack has DEPTH for exactly that reason.
    container.className = "dockview-theme-dark";
    container.setAttribute(
        "style",
        "position: fixed; left: 0; top: 0; z-index: 9; width: 600px; height: 300px;",
    );
    window.document.body.appendChild(container);
    const host = new PanelHost({
        container,
        client: new PanelClient(mockShell(panels)),
        dockview: dv,
        // Absent unless a case asks for it (exactOptionalPropertyTypes forbids an explicit
        // `undefined`), so every other case mounts exactly the host it mounted before.
        ...(onArrangementChanged === undefined
            ? {}
            : {
                  onArrangementChanged: (): void => {
                      onArrangementChanged(container);
                  },
              }),
    });
    await host.start();
    // RE-APPLIED after `start()`: the host mounts Dockview into this element and sets its own
    // layout styles, so anything written before would be silently replaced. The explicit `layout`
    // then makes Dockview recompute at the final size RIGHT NOW — its own resize observation is
    // asynchronous, and every geometry assertion in this file would otherwise race it.
    container.setAttribute(
        "style",
        "position: fixed; left: 0; top: 0; z-index: 9; width: 600px; height: 300px;",
    );
    host.api?.layout(600, 300);
    return {
        host,
        container,
        dispose: (): void => {
            host.dispose();
            container.remove();
        },
    };
}

/** The mounted body element of one panel COPY, or null. */
function slotFor(container: HTMLElement, instanceId: string): HTMLElement | null {
    return container.querySelector<HTMLElement>(
        `[${PANEL_INSTANCE_ATTRIBUTE}="${instanceId}"]`,
    );
}

/** The Dockview GROUP painting behind `element`, found in its paint stack (never its ancestors). */
function groupBehind(element: Element, root: Element): Element | null {
    return (
        paintStack(element, root).find((candidate) =>
            candidate.classList.contains("dv-groupview"),
        ) ?? null
    );
}

/** Is `slot` laid out INSIDE the dock's own box (as opposed to parked outside it)? */
function isInsideDock(slot: Element, container: Element): boolean {
    const rect = slot.getBoundingClientRect();
    const dock = container.getBoundingClientRect();
    return (
        rect.width > 0 &&
        rect.height > 0 &&
        rect.right > dock.left &&
        rect.bottom > dock.top &&
        rect.left < dock.right &&
        rect.top < dock.bottom
    );
}

/** Is a computed background fully transparent (alpha 0 / `transparent` / `none`)? */
function isTransparent(color: string): boolean {
    const normalized = color.trim().toLowerCase();
    if (normalized === "transparent" || normalized === "" || normalized === "none") {
        return true;
    }
    // `rgba(r, g, b, 0)` — the only opaque-vs-not question a computed background-color answers.
    const match = /^rgba?\(([^)]*)\)$/.exec(normalized);
    if (match === null) {
        return false;
    }
    const parts = match[1]?.split(",") ?? [];
    return parts.length === 4 && Number.parseFloat(parts[3] ?? "1") === 0;
}

/**
 * The elements the browser would PAINT under the centre of `element`, outermost-last, restricted to
 * the dock (plus `html`/`body`, which are legitimately part of the stack).
 *
 * `elementsFromPoint`, not an ancestor walk: Dockview mounts an `always`-rendered panel in a SIBLING
 * overlay container, so the group view that paints the docking surface is not an ancestor of the
 * viewport's slot at all — it is BEHIND it, which is exactly where an opaque layer would hide the
 * Shell's composited viewport while an ancestor walk reported everything transparent.
 */
function paintStack(element: Element, root: Element): Element[] {
    const rect = element.getBoundingClientRect();
    const x = rect.left + rect.width / 2;
    const y = rect.top + rect.height / 2;
    return window.document
        .elementsFromPoint(x, y)
        .filter(
            (candidate) =>
                root.contains(candidate) ||
                candidate === root ||
                candidate === window.document.body ||
                candidate === window.document.documentElement,
        );
}

function describe(element: Element): string {
    return `${element.tagName.toLowerCase()}.${element.className || "(no class)"}`;
}

// ------------------------------------------------------------------------------ the DOM fixture
//
// A plain fixture with FRACTIONAL CSS edges, for the arithmetic tier. Deliberately not a Dockview
// layout: the rounding rule must be provable at coordinates chosen to expose it, and a dock's own
// geometry is not ours to choose.

interface Fixture {
    readonly root: HTMLElement;
    dispose(): void;
}

function mountFixture(
    rects: readonly (readonly [string, number, number, number, number])[],
): Fixture {
    const root = window.document.createElement("div");
    root.style.position = "fixed";
    root.style.left = "0px";
    root.style.top = "0px";
    root.style.width = "1000px";
    root.style.height = "800px";
    for (const [instanceId, left, top, width, height] of rects) {
        const slot = window.document.createElement("div");
        slot.setAttribute("data-panel-id", VIEWPORT_PANEL_ID);
        slot.setAttribute(PANEL_INSTANCE_ATTRIBUTE, instanceId);
        slot.setAttribute(VIEWPORT_SURFACE_ATTRIBUTE, "");
        slot.style.position = "absolute";
        slot.style.left = `${String(left)}px`;
        slot.style.top = `${String(top)}px`;
        slot.style.width = `${String(width)}px`;
        slot.style.height = `${String(height)}px`;
        root.appendChild(slot);
    }
    window.document.body.appendChild(root);
    return {
        root,
        dispose: (): void => {
            root.remove();
        },
    };
}

// ------------------------------------------------------------------------------------- the cases

export const viewportTests: readonly TestCase[] = [
    {
        name: "viewport: the native-surface predicate is the closed set of one",
        run: (): void => {
            assert(isNativeSurfacePanelId(VIEWPORT_PANEL_ID), "the Scene viewport is one");
            assert(
                !isNativeSurfacePanelId("builtin.viewport-edit"),
                "its SIBLING is not — the two ids differ by a suffix and share a directory, which " +
                    "is exactly the confusion a substring test would introduce",
            );
            assert(!isNativeSurfacePanelId("builtin.files"), "an ordinary uitree panel is not");
            assert(!isNativeSurfacePanelId(""), "and neither is nothing");
            const manifest = parsePanelManifest(manifestJson(VIEWPORT_PANEL_ID));
            assert(manifest !== null, "the fixture manifest parses");
            if (manifest !== null) {
                assert(isNativeSurfacePanel(manifest), "the manifest-shaped sibling agrees");
                assert(
                    manifest.contentType === "uitree",
                    "and it is a `uitree` panel — the branch that takes Dockview's DEFAULT renderer, " +
                        "which is why the native-surface question must be asked BEFORE the content type",
                );
            }
        },
    },
    {
        name: "viewport: the rect is PHYSICAL px at device scale 1 / 1.5 / 2 / 3, edges rounded",
        run: (): void => {
            // FRACTIONAL edges on purpose: at whole-pixel coordinates every rounding rule agrees.
            const fixture = mountFixture([["builtin.viewport#1", 50.25, 40.25, 99.95, 60.7]]);
            try {
                const slot = fixture.root.firstElementChild;
                assert(slot !== null, "the fixture slot is in the document");
                if (slot === null) {
                    return;
                }
                const css = slot.getBoundingClientRect();
                assert(
                    css.left % 1 !== 0 || css.top % 1 !== 0,
                    "the fixture really did lay out on fractional CSS coordinates — without that " +
                        "the edge-rounding rule below is untestable and this case is vacuous",
                );

                const at1 = viewportRegions(fixture.root, 1);
                assertEqual(at1.length, 1, "one viewport, one region");
                assertEqual(at1[0]?.kind, REGION_KIND_VIEWPORT, "published as a viewport region");
                assertEqual(at1[0]?.id, "builtin.viewport#1", "keyed by the INSTANCE id");

                for (const dpr of [1, 1.5, 2, 3]) {
                    const regions = viewportRegions(fixture.root, dpr);
                    const rect = regions[0]?.rect;
                    assert(rect !== undefined, `a region at dpr ${String(dpr)}`);
                    if (rect === undefined) {
                        continue;
                    }
                    const x0 = Math.round(css.left * dpr);
                    const y0 = Math.round(css.top * dpr);
                    assertEqual(rect.x, x0, `x is physical at dpr ${String(dpr)}`);
                    assertEqual(rect.y, y0, `y is physical at dpr ${String(dpr)}`);
                    // THE EDGE RULE: round the far edge and subtract, never round the extent.
                    assertEqual(
                        rect.width,
                        Math.round(css.right * dpr) - x0,
                        `width derives from the ROUNDED edges at dpr ${String(dpr)}`,
                    );
                    assertEqual(
                        rect.height,
                        Math.round(css.bottom * dpr) - y0,
                        `height derives from the ROUNDED edges at dpr ${String(dpr)}`,
                    );
                }

                // ⚠ THE NON-VACUITY HALVES. Without these the four loops above would all pass for an
                // implementation that ignored `dpr` entirely (at 1.0 it is correct) or that rounded
                // the EXTENT (at 1.0 and 2.0 it agrees).
                const scaled = viewportRegions(fixture.root, 1.5)[0]?.rect;
                assert(scaled !== undefined && at1[0] !== undefined, "both measurements exist");
                if (scaled === undefined || at1[0] === undefined) {
                    return;
                }
                assert(
                    scaled.x !== at1[0].rect.x && scaled.width !== at1[0].rect.width,
                    "the 1.5 rect DIFFERS from the 1.0 rect — a producer that dropped the scale " +
                        "would pass every assertion above and fail here",
                );
                // The plausible-but-wrong rule, `round(width · dpr)`, agrees with the edge rule at
                // MOST coordinates and at every integral scale — which is why the fixture's edges are
                // chosen to expose it, and why the disagreement is SEARCHED FOR rather than asserted
                // at one scale: if no tested scale separated the two rules, the width assertions
                // above would be satisfied by both and would pin neither.
                const separating = [1, 1.5, 2, 3].filter((dpr) => {
                    const x0 = Math.round(css.left * dpr);
                    return Math.round(css.width * dpr) !== Math.round(css.right * dpr) - x0;
                });
                assert(
                    separating.length > 0,
                    "at least one tested scale SEPARATES the edge rule from `round(width · dpr)` — " +
                        `none did, so the width assertions pin nothing (css ${String(css.left)}` +
                        `..${String(css.right)})`,
                );
            } finally {
                fixture.dispose();
            }
        },
    },
    {
        name: "viewport: two copies are two regions, keyed by instance id, in DOM order",
        run: (): void => {
            // c3's `unlimited` mode: several scene views is the point, and the Shell keys a render
            // target AND a camera by each id — so two copies sharing one key would share a camera.
            const fixture = mountFixture([
                ["builtin.viewport#1", 0.5, 40.5, 400.25, 300.5],
                ["builtin.viewport#2", 400.75, 40.5, 399.25, 300.5],
            ]);
            try {
                const regions = viewportRegions(fixture.root, 1.5);
                assertEqual(regions.length, 2, "two copies, two regions");
                assertEqual(regions[0]?.id, "builtin.viewport#1", "DOM order is publish order");
                assertEqual(regions[1]?.id, "builtin.viewport#2", "…and the second follows");
                assert(
                    regions[0]?.rect.x !== regions[1]?.rect.x,
                    "with independent rects, at a scale where they are not accidentally equal",
                );
                // They ABUT rather than overlap: the edge-rounding rule is what guarantees the right
                // edge of one is the left edge of the next, at a fractional split and a fractional
                // scale. A per-extent rounding leaves a seam or an overlap here.
                const first = regions[0]?.rect;
                const second = regions[1]?.rect;
                assert(first !== undefined && second !== undefined, "both rects exist");
                if (first !== undefined && second !== undefined) {
                    assertEqual(
                        first.x + first.width,
                        second.x,
                        "the two rects ABUT exactly — no seam, no overlap, at dpr 1.5",
                    );
                }
            } finally {
                fixture.dispose();
            }
        },
    },
    {
        name: "viewport: an unlaid-out or unkeyed copy publishes nothing",
        run: (): void => {
            const fixture = mountFixture([["builtin.viewport#1", 10, 10, 0, 0]]);
            try {
                assertEqual(
                    viewportRegions(fixture.root, 2).length,
                    0,
                    "a collapsed / mid-resize copy is DROPPED rather than published as a " +
                        "degenerate hole the compositor would scissor to nothing",
                );
                const slot = fixture.root.firstElementChild;
                if (slot !== null) {
                    (slot as HTMLElement).style.width = "200px";
                    (slot as HTMLElement).style.height = "150px";
                    assertEqual(
                        viewportRegions(fixture.root, 2).length,
                        1,
                        "and it comes back the moment it has a rect again",
                    );
                    slot.setAttribute(PANEL_INSTANCE_ATTRIBUTE, "");
                    assertEqual(
                        viewportRegions(fixture.root, 2).length,
                        0,
                        "an UNKEYED surface publishes nothing: a region id of \"\" would name a " +
                            "render target and a camera by nothing",
                    );
                }
            } finally {
                fixture.dispose();
            }
        },
    },
    {
        name: "viewport: NOTHING in the slot's paint stack paints; an ordinary panel's does",
        run: async (): Promise<void> => {
            // A composited native layer is visible only where the browser's frame is alpha-0 ALL THE
            // WAY DOWN, so that — not "the slot and the group are transparent" — is what this case
            // asserts. The measured paint stack under a viewport is
            //
            //     .ctx-panel-body | .dv-render-overlay | .dv-content-container | .dv-groupview |
            //     … | .dv-grid-view.dv-dockview | body | html
            //
            // ⚠ THIS CASE USED TO ASSERT ONLY THE FIRST TWO, and that is exactly how the hole shipped
            // shut. e3 delivered the slot and the group behind it (which is not an ancestor of the
            // slot: an `always`-rendered panel lives in `.dv-render-overlay`, a sibling subtree, so
            // the group paints BEHIND the hole and no selector reaches it from there) and recorded
            // the rest as a deferred boundary. The rest was the part that mattered: `.dv-dockview`
            // paints `--dv-group-view-background-color` behind every group and `html, body` paint the
            // document canvas behind that, so the live editor's viewport slot measured `#0a0a0a` —
            // byte-identical to every other panel body — with both of e3's rules already in force,
            // and the Shell's layer was painted over on every frame in every theme.
            //
            // So the assertion is now a WHOLE-STACK one, expressed as a loop rather than a list of
            // named elements: a future dockview upgrade that inserts one more painted wrapper must
            // fail this, and it cannot if the test only knows the names of the wrappers that existed
            // when it was written.
            //
            // (The third deferred item, CEF's own `CefBrowserSettings.background_color`, needed no
            // change and its premise was wrong: it is left at 0, whose alpha is 0, and CEF documents
            // that as "use transparent painting" for a windowless browser. Measured on the live
            // editor — with these rules in place and NOTHING changed C++-side, the composited scene
            // shows through.)
            const mounted = await mountHost([
                manifestJson(VIEWPORT_PANEL_ID, "unlimited"),
                manifestJson("builtin.files"),
            ]);
            try {
                const viewport = mounted.container.querySelector<HTMLElement>(
                    `[${VIEWPORT_SURFACE_ATTRIBUTE}]`,
                );
                assert(
                    viewport !== null,
                    "the viewport's slot carries the native-surface marker — stamped by " +
                        "`markPanelSlot`, which every renderer's slot goes through",
                );
                if (viewport === null) {
                    return;
                }
                assertEqual(
                    viewport.getAttribute("data-panel-id"),
                    VIEWPORT_PANEL_ID,
                    "and the marker is on the VIEWPORT's slot",
                );
                const ordinaryInstance = mounted.host.mounted.find(
                    (id) => mounted.host.panelIdOf(id) === "builtin.files",
                );
                assert(ordinaryInstance !== undefined, "the ordinary panel mounted too");

                // ACTIVATE THE VIEWPORT. Both panels land in the `center` group and the one added
                // last is the active tab, so without this the hole would be measured on the copy the
                // human is not looking at — which Dockview parks outside the dock entirely.
                const viewportInstance = viewport.getAttribute(PANEL_INSTANCE_ATTRIBUTE) ?? "";
                mounted.host.api?.getPanel(viewportInstance)?.api?.setActive();
                await waitFor(
                    "the viewport became the active tab and is laid out INSIDE the dock",
                    () => isInsideDock(viewport, mounted.container),
                );

                assert(
                    isTransparent(window.getComputedStyle(viewport).backgroundColor),
                    "the SLOT stops painting",
                );

                // THE WHOLE STACK, which is the assertion the composite actually depends on. Every
                // element the browser would paint under the slot's centre — the slot itself, every
                // wrapper, the dockview root, `body`, `html` — must be alpha-0; ONE opaque layer
                // anywhere in it hides the Shell's viewport just as completely as all of them would.
                const stack = paintStack(viewport, mounted.container);
                assert(
                    stack.length >= 4,
                    `the paint stack has depth (${String(stack.length)} elements) — a short stack ` +
                        "means elementsFromPoint hit nothing and the loop below is vacuous",
                );
                const painted = stack.filter(
                    (candidate) =>
                        !isTransparent(window.getComputedStyle(candidate).backgroundColor),
                );
                assertEqual(
                    painted.map(describe).join(", "),
                    "",
                    "NOTHING in the viewport's paint stack paints a background",
                );
                assert(
                    stack.some((candidate) => candidate === window.document.documentElement),
                    "…and the stack reaches `html`, so the canvas was actually one of the elements " +
                        "the loop above cleared rather than one it never saw",
                );

                const group = groupBehind(viewport, mounted.container);
                assert(
                    group !== null,
                    "the group behind the viewport is in its paint stack — a null here means the " +
                        "stack was empty and every assertion below would be vacuous",
                );
                if (group === null) {
                    return;
                }
                assert(
                    group.hasAttribute("data-group-native-surface"),
                    `the GROUP is marked as showing a native surface (${describe(group)})`,
                );
                assert(
                    isTransparent(window.getComputedStyle(group).backgroundColor),
                    `…so it stops painting too (${window.getComputedStyle(group).backgroundColor})`,
                );

                // The two window-wide markers the transparency above is SCOPED to. Asserted by name
                // as well as by effect: the whole-stack loop proves the pixels, these prove WHY, so a
                // stylesheet that made every dock transparent unconditionally (which would repaint
                // every window with no viewport open) cannot pass this case.
                const dock = mounted.container.querySelector(".dv-dockview");
                assert(dock !== null, "the dockview root is in the DOM");
                if (dock === null) {
                    return;
                }
                assert(
                    dock.hasAttribute(VIEWPORT_DOCK_ATTRIBUTE),
                    "the dockview ROOT is marked as carrying a native surface",
                );
                assert(
                    window.document.documentElement.hasAttribute(VIEWPORT_DOCUMENT_ATTRIBUTE),
                    "…and so is the document, which is what stops the canvas painting",
                );

                // THE CONTROL, in BOTH directions. Switch to the ordinary panel: the SAME group must
                // lose the mark and start painting again. Without this, "the group is transparent"
                // would pass for a stylesheet that made every group transparent, or for one that
                // never loaded at all.
                mounted.host.api?.getPanel(ordinaryInstance ?? "")?.api?.setActive();
                await waitFor(
                    "the ordinary panel became the active tab and is laid out INSIDE the dock",
                    () => {
                        const slot = slotFor(mounted.container, ordinaryInstance ?? "");
                        return slot !== null && isInsideDock(slot, mounted.container);
                    },
                );
                assert(
                    !group.hasAttribute("data-group-native-surface"),
                    "the mark is WITHDRAWN when the group stops showing the viewport — a group " +
                        "holding a background viewport tab is showing something else over its box",
                );
                assert(
                    !isTransparent(window.getComputedStyle(group).backgroundColor),
                    "and the docking surface is painted again " +
                        `(${window.getComputedStyle(group).backgroundColor})`,
                );
                assert(
                    !dock.hasAttribute(VIEWPORT_DOCK_ATTRIBUTE) &&
                        !window.document.documentElement.hasAttribute(
                            VIEWPORT_DOCUMENT_ATTRIBUTE,
                        ),
                    "…and BOTH window-wide markers are withdrawn, so a window showing no viewport " +
                        "paints its dock and its canvas exactly as it did before the hole existed",
                );
                assert(
                    !isTransparent(
                        window.getComputedStyle(window.document.documentElement).backgroundColor,
                    ) ||
                        !isTransparent(window.getComputedStyle(window.document.body).backgroundColor),
                    "…which the canvas itself confirms: it is painting again",
                );
                const ordinary = slotFor(mounted.container, ordinaryInstance ?? "");
                assert(
                    ordinary !== null && !ordinary.hasAttribute(VIEWPORT_SURFACE_ATTRIBUTE),
                    "and the ordinary panel's own slot carries no native-surface marker",
                );
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "viewport: the dock's own trigger gets a viewport rect published",
        run: async (): Promise<void> => {
            // THE TRIGGER, and it is the half without which every rule in this file is decoration.
            //
            // A viewport is composited from a `viewport`-kind region, and that region exists only
            // once Dockview has laid the panel out. Nothing else in the editor can notice that
            // moment: it is not a window resize and not a DPI change (the region publisher's own two
            // triggers), and `LayoutPersistence` subscribes the same Dockview event only AFTER
            // `start()` has added every panel — so the adds that first produced the rect are already
            // past. Measured on the live editor before this seam existed: the Shell held 4 regions,
            // zero of them `viewport`-kind, for the entire life of the window, and the composited
            // layer stack stayed empty until the human happened to drag a sash.
            //
            // DRIVEN THROUGH THE REAL `ChromeRegionPublisher`, wired exactly as boot.ts wires it,
            // rather than by asserting the callback fired. That is not ceremony: a report can arrive
            // BEFORE Dockview has applied the arrangement to the DOM, and what makes the published
            // map correct anyway is the publisher's DEBOUNCE — it measures when the burst settles,
            // not when it starts. A test that sampled the geometry at callback time would assert a
            // property the production path does not have and does not need. (This case was written
            // that way first; it failed against a working editor, which is how the debounce's role
            // got pinned down rather than assumed.)
            //
            // The other two triggers are stubbed OUT — no resize target, no matchMedia — so what is
            // proven here is the DOCK's trigger alone, not something a stray resize could satisfy.
            let publisher: ChromeRegionPublisher | undefined;
            const published: number[] = [];
            const mounted = await mountHost(
                // BOTH IN ONE GROUP, deliberately (the always-renderer case builds the same
                // fixture for the same reason): a tab switch is the cheapest arrangement change that
                // both moves a viewport OUT of the dock and back, so one case can prove the rect is
                // published AND withdrawn on the same trigger.
                [
                    manifestJson(VIEWPORT_PANEL_ID, "unlimited", "center"),
                    manifestJson("builtin.files", "singleton", "center"),
                ],
                (container: HTMLElement): void => {
                    publisher ??= new ChromeRegionPublisher({
                        provider: (): readonly ShellRegion[] => viewportRegions(container),
                        publish: async (regions): Promise<boolean> => {
                            published.push(regions.length);
                            return Promise.resolve(true);
                        },
                        // The SHIPPED default is 100 ms; 60 keeps the case quick while still
                        // letting a burst settle before the provider measures — which is the whole
                        // reason the production path is debounced at all (see the header).
                        debounceMs: 60,
                        resizeTarget: { addEventListener: (): void => {}, removeEventListener: (): void => {} },
                        matchMedia: (): null => null,
                    });
                    publisher.schedule();
                },
            );
            try {
                assert(
                    publisher !== undefined,
                    "the mount itself reported an arrangement — a host that only reports on LATER " +
                        "changes leaves a window that is never touched again publishing nothing",
                );

                // ACTIVATE THE VIEWPORT, for the reason the paint-stack case does: both panels land
                // in the `center` group, the one added last is the active tab, and Dockview parks an
                // `always`-rendered background panel a full dock-height OUTSIDE the dock — where
                // `viewportRegions` deliberately withdraws it.
                const viewportInstance = mounted.host.mounted.find(
                    (id) => mounted.host.panelIdOf(id) === VIEWPORT_PANEL_ID,
                );
                mounted.host.api?.getPanel(viewportInstance ?? "")?.api?.setActive();
                await waitFor(
                    "a publish carried the viewport's rect",
                    () => published.some((count) => count > 0),
                );

                // …AND IT KEEPS FIRING. A trigger that only ever fired once would leave the Shell
                // holding whatever map the mount happened to produce, which is the same defect one
                // arrangement later: a viewport that moves (a tab switch, a sash drag, a tear-out)
                // must republish, and the map is replaced wholesale so a rect that stops being
                // published is a rect the Shell drops.
                //
                // WHAT IS *NOT* ASSERTED HERE, deliberately: which regions that later publish
                // carries. Whether a parked copy is withdrawn is `viewportRegions`' own rule and its
                // own cases above ("an unlaid-out or unkeyed copy publishes nothing", and the
                // off-dock clip) prove it directly against constructed geometry, which is the tier
                // that can choose the geometry. Re-deriving it here through a real dock would assert
                // Dockview's parking behaviour, not ours.
                const before = published.length;
                const files = mounted.host.mounted.find(
                    (id) => mounted.host.panelIdOf(id) === "builtin.files",
                );
                mounted.host.api?.getPanel(files ?? "")?.api?.setActive();
                await waitFor(
                    "…and a later arrangement change publishes again",
                    () => published.length > before,
                );
            } finally {
                publisher?.dispose();
                mounted.dispose();
            }
        },
    },
    {
        name: "viewport: the renderer is pinned to always — an inactive copy stays in the DOM",
        run: async (): Promise<void> => {
            // The CONSEQUENCE, not the option: an `onlyWhenVisible` panel is REMOVED from the DOM
            // when another tab in its group is activated, which makes its rect meaningless and takes
            // the transparent hole with it (viewport.ts § THE HOLE PINS THE RENDERER).
            //
            // ⚠ BOTH PANELS ARE DECLARED `center` ON PURPOSE, and that is the whole reason they share
            // a group here. This case needs two panels in ONE group to have a tab switch to observe
            // at all, so it BUILDS one — it does not discover one. Read no further than that: the
            // detachment asserted at the end of this case is Dockview doing the correct thing to a
            // co-tabbed panel, NOT a statement that displacing a sibling out of the DOM is
            // acceptable when the roster never asked for co-tabbing. It is not, it shipped, and the
            // real-roster case at the bottom of this file is what pins it.
            const mounted = await mountHost([
                manifestJson(VIEWPORT_PANEL_ID, "unlimited", "center"),
                manifestJson("builtin.files", "singleton", "center"),
            ]);
            try {
                const api = mounted.host.api;
                assert(api !== null, "the dock is up");
                const viewportPanels = mounted.host.mounted.filter(
                    (id) => mounted.host.panelIdOf(id) === VIEWPORT_PANEL_ID,
                );
                const otherPanels = mounted.host.mounted.filter(
                    (id) => mounted.host.panelIdOf(id) === "builtin.files",
                );
                assertEqual(viewportPanels.length, 1, "one viewport copy is open");
                assertEqual(otherPanels.length, 1, "and one ordinary panel beside it");
                const viewportId = viewportPanels[0] ?? "";
                const otherId = otherPanels[0] ?? "";

                const viewportSlot = slotFor(mounted.container, viewportId);
                const otherSlot = slotFor(mounted.container, otherId);
                assert(viewportSlot !== null, "the viewport slot is mounted");
                assert(otherSlot !== null, "the ordinary slot is mounted");
                if (viewportSlot === null || otherSlot === null) {
                    return;
                }

                // Activate the ORDINARY panel. Both fixtures declare `center`, so both land in one
                // group (see the roster note above) and this is the tab switch that detaches
                // whatever Dockview is allowed to detach.
                api?.getPanel(otherId)?.api?.setActive();
                await waitFor(
                    "the ordinary panel became the active tab",
                    () => !otherSlot.isConnected || otherSlot.getBoundingClientRect().width > 0,
                );

                assert(
                    viewportSlot.isConnected,
                    "the INACTIVE viewport is STILL IN THE DOCUMENT — `renderer: \"always\"`. " +
                        "Detached, its element would be destroyed and re-created on every tab " +
                        "switch, and the region map would carry a rect for an element that is gone",
                );
                const rect = viewportSlot.getBoundingClientRect();
                assert(
                    rect.width > 0 && rect.height > 0,
                    `and it still MEASURES (${String(rect.width)}×${String(rect.height)}) — which ` +
                        "is exactly why the parked case must be handled: the rect is plausible",
                );
                assert(
                    !isInsideDock(viewportSlot, mounted.container),
                    "…but Dockview PARKED it outside the dock rather than deleting it, which is " +
                        "what `always` means and what a naive publisher would miss",
                );
                assertEqual(
                    viewportRegions(mounted.container, 2).length,
                    0,
                    "so the region is WITHDRAWN while the copy is parked — no render target, no " +
                        "scene drawn and no layer composited for a viewport the human cannot see",
                );

                // BACK AGAIN: activating the copy restores its region, so the withdrawal above is a
                // STATE and not a one-way loss.
                api?.getPanel(viewportId)?.api?.setActive();
                await waitFor(
                    "the viewport is back inside the dock",
                    () => isInsideDock(viewportSlot, mounted.container),
                );
                assertEqual(
                    viewportRegions(mounted.container, 2).length,
                    1,
                    "the region returns the moment the copy is the active tab again",
                );

                // THE CONTROL, and what makes `always` mean anything at all: Dockview REMOVES an
                // ordinary `uitree` panel from the document when it stops being active. Without this
                // assertion "the viewport stayed" proves nothing — a Dockview that never detached
                // anything would satisfy it for free.
                //
                // ⚠ THIS ASSERTS A PLATFORM FACT, AND ONLY BECAUSE THIS FIXTURE ASKED FOR IT. Both
                // panels here declare `center`, so co-tabbing is what the roster requested and
                // detaching the inactive one is correct. The same platform fact applied to a panel
                // the roster did NOT ask to co-tab is the e3 regression — pinned separately below,
                // because no synthetic roster can catch it: the author picks the victim.
                await waitFor(
                    "an ordinary `uitree` panel to be DETACHED once it stops being the active tab — " +
                        "the default this task pins the viewport away from",
                    () => !otherSlot.isConnected,
                );
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "viewport: the mounted copy's region is measured off the REAL dock rect",
        run: async (): Promise<void> => {
            const mounted = await mountHost([manifestJson(VIEWPORT_PANEL_ID, "unlimited")]);
            try {
                const ids = mounted.host.mounted.filter(
                    (id) => mounted.host.panelIdOf(id) === VIEWPORT_PANEL_ID,
                );
                assertEqual(ids.length, 1, "one copy");
                const slot = slotFor(mounted.container, ids[0] ?? "");
                assert(slot !== null, "its slot is mounted");
                if (slot === null) {
                    return;
                }
                await waitFor(
                    "the single copy is laid out INSIDE the dock",
                    () => isInsideDock(slot, mounted.container),
                    5000,
                    () =>
                        `slot ${JSON.stringify(slot.getBoundingClientRect())} vs dock ` +
                        `${JSON.stringify(mounted.container.getBoundingClientRect())}`,
                );
                const css = slot.getBoundingClientRect();
                assert(css.width > 0, "the dock laid it out");

                for (const dpr of [1, 2]) {
                    const regions = viewportRegions(mounted.container, dpr);
                    assertEqual(regions.length, 1, `one region at dpr ${String(dpr)}`);
                    assertEqual(regions[0]?.id, ids[0], "keyed by the copy's instance id");
                    assertEqual(
                        regions[0]?.rect.width,
                        Math.round(css.right * dpr) - Math.round(css.left * dpr),
                        `the REAL dock rect, in physical px at dpr ${String(dpr)}`,
                    );
                }
                const one = viewportRegions(mounted.container, 1)[0]?.rect.width ?? 0;
                const two = viewportRegions(mounted.container, 2)[0]?.rect.width ?? 0;
                assertEqual(two, one * 2, "and the two scales really do differ");
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        // THE e3 REGRESSION, pinned against the SHIPPED roster.
        //
        // Hosting the viewport put a `center` panel immediately after `builtin.inspector` (`right`)
        // in roster order. `open` placed every panel relative to whatever was mounted LAST with no
        // regard for zone, and `center` is the one zone that maps to `"within"` — join the reference
        // panel's group as a tab. So the Scene view tabbed itself onto the Inspector and Dockview's
        // default `onlyWhenVisible` renderer DETACHED the Inspector's element from the document.
        //
        // The Inspector stayed mounted, stayed listed and stayed driveable over the bridge; it was
        // simply not in the DOM. `editor-cef-smoke-shell-inspector-fanout` therefore did not fail an
        // assertion — its `querySelector` returned null, its injected script returned early, nothing
        // ever staged, and the test TIMED OUT at ~420 s on all three OS legs.
        name: "viewport: hosting the viewport does not evict a sibling panel from the DOM (real roster)",
        run: async (): Promise<void> => {
            const mounted = await mountHost(realRosterJson());
            try {
                // THE DRIFT DETECTOR. If the C++ roster or `hostable_panel_ids` changes and this
                // fixture is not updated with it, this is what says so — before the assertions below
                // start proving something about a roster the editor no longer serves.
                const kinds = mounted.host.mounted.map((id) => mounted.host.panelIdOf(id));
                assertEqual(
                    JSON.stringify([...kinds].sort((a, b) => a.localeCompare(b))),
                    JSON.stringify([
                        "builtin.files",
                        "builtin.inspector",
                        "builtin.problems",
                        "builtin.scene-tree",
                        "builtin.session.undo",
                        "builtin.viewport",
                        "placeholder",
                    ]),
                    "the fixture mounts exactly the hosted `uitree` built-ins — if this fails, the " +
                        "real roster moved and realRosterJson() must be re-transcribed from " +
                        "builtin_roster.cpp + builtin_panels.cpp before the rest of this case means " +
                        "anything",
                );

                const inspectorId = mounted.host.instancesOf("builtin.inspector")[0] ?? "";
                assert(inspectorId !== "", "the Inspector is mounted");
                const inspectorSlot = slotFor(mounted.container, inspectorId);

                // THE ASSERTION THE SMOKE'S TIMEOUT WAS. Not "the Inspector is open" — it was open
                // throughout the outage — but "the Inspector is REACHABLE THROUGH THE DOCUMENT",
                // which is the only thing a `document.querySelector` (the Shell's own fan-out smoke,
                // a11y tooling, and every user click) can act on.
                assert(
                    inspectorSlot !== null && inspectorSlot.isConnected,
                    "the Inspector's slot is IN THE DOCUMENT after start() — with the viewport " +
                        "hosted, a zone-blind reference panel tabs the Scene view onto the " +
                        "Inspector and Dockview detaches the inactive tab, which is invisible to " +
                        "every 'is it mounted?' check and fatal to every DOM one",
                );

                // AND NOBODY ELSE EITHER. The Inspector is the panel that broke, but the defect is
                // "a `center` mount evicts whatever preceded it", so the victim is an accident of
                // roster order — pinning only the Inspector would let the next reorder move the
                // damage to a panel nothing happens to name here.
                for (const kind of [
                    "builtin.scene-tree",
                    "builtin.files",
                    "builtin.problems",
                    "builtin.session.undo",
                ]) {
                    const instanceId = mounted.host.instancesOf(kind)[0] ?? "";
                    const slot = slotFor(mounted.container, instanceId);
                    assert(
                        slot !== null && slot.isConnected,
                        `'${kind}' is in the document too — no panel the roster never asked to ` +
                            "co-tab may be evicted by a later mount",
                    );
                }
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        // THE OTHER HALF, and the reason the case above cannot be satisfied the cheap way.
        //
        // "The Inspector stays in the DOM" is trivially true of a build that does not host the
        // viewport at all, and equally true of one that opens it `inactive` — parked, tabbed behind
        // the Inspector, with `viewportRegions` withdrawing its rect so the Shell composites no scene
        // and the human sees nothing. Both would score GREEN above while defeating the entire point
        // of e3. This case asserts the viewport is not merely mounted but LAID OUT AND PUBLISHED, so
        // the pair together admit only an arrangement that is correct on both counts.
        name: "viewport: …and the viewport is still visible and publishing a region (real roster)",
        run: async (): Promise<void> => {
            const mounted = await mountHost(realRosterJson());
            try {
                const viewportId = mounted.host.instancesOf(VIEWPORT_PANEL_ID)[0] ?? "";
                assert(viewportId !== "", "the viewport is mounted");
                const slot = slotFor(mounted.container, viewportId);
                assert(slot !== null, "the viewport's slot exists");
                if (slot === null) {
                    return;
                }
                await waitFor(
                    "the viewport is laid out INSIDE the dock — not parked outside it, which is " +
                        "what an `inactive` open would leave and what withdraws its region",
                    () => isInsideDock(slot, mounted.container),
                    5000,
                    () =>
                        `slot ${JSON.stringify(slot.getBoundingClientRect())} vs dock ` +
                        `${JSON.stringify(mounted.container.getBoundingClientRect())}`,
                );
                assertEqual(
                    viewportRegions(mounted.container, 1).length,
                    1,
                    "and it PUBLISHES a region — the Shell composites a scene into this rect, so a " +
                        "viewport that boots parked is a viewport the user cannot see",
                );
            } finally {
                mounted.dispose();
            }
        },
    },
];
