// The Scene viewport's EDITOR-CORE half (M9 editor-UX e3, D7; taskflow 06 §1) — the DOM HOLE and the
// RECT, the two things the C++ producer cannot know and cannot measure.
//
// The split, stated once so neither side grows the other's job:
//
//   * the SHELL owns the pixels. `shell::ViewportBinding` keeps one render target per live viewport,
//     draws the D5 pass into it and publishes a `ViewportLayer` whose `content_rect` is the rect
//     this module measures. It composites that layer BENEATH the CEF layer.
//   * EDITOR-CORE owns the hole and the rect. The layer is only visible where the browser's own
//     frame is transparent, and the browser is the only thing that knows where a docked panel
//     actually landed — Dockview decides that, in CSS pixels, and re-decides it on every drag.
//
// ⚠ THE HOLE PINS THE RENDERER TO `"always"`. Dockview's default `onlyWhenVisible` REMOVES an
// inactive panel's element from the DOM. For a viewport that is not a memory optimisation, it is a
// correctness failure with two independent halves: (a) a detached element measures 0x0, so the rect
// this module reports is meaningless and the Shell would either publish a degenerate layer or keep a
// stale one over a panel that is no longer there; and (b) the transparent hole goes with it, so the
// native layer beneath would show through whatever tab the human switched TO. `panelhost.ts`'s
// `rendererFor` therefore consults `isNativeSurfacePanelId` before the content-type switch — a
// viewport is a `uitree` panel, and `uitree` is exactly the branch that takes the default.
//
// ⚠ THE RECT IS PHYSICAL. `ShellRegion::rect`, `ViewportLayer::content_rect` and the OS pointer
// stream are all physical client pixels; `getBoundingClientRect` is CSS pixels. The conversion is
// `editorstate.ts`'s `physicalRegion` — the ONE seam editor-window-chrome a2 established for the
// titlebar and that this module reuses rather than copying. At device scale 1.0 the two units
// coincide, which is why `viewport.test.ts` proves the arithmetic at 1.5 / 2 / 3 and not at 1.

import {
    REGION_KIND_VIEWPORT,
    defaultDevicePixelRatio,
    physicalRegion,
    type ShellRegion,
} from "./editorstate.js";
import type { DockviewPanelHandle } from "./dockview.js";
import type { PanelManifest } from "./panels.js";

/**
 * The built-in whose content is a NATIVE SURFACE — a transparent hole in the web layer with the
 * Shell's composited viewport showing through.
 *
 * A closed, build-known set of ONE, spelled as an id here for the same reason `SETTINGS_PANEL_ID`
 * and `SESSION_UNDO_PANEL_ID` are. The alternative — a new manifest field — is a CONTRACT change
 * (registry, schema, the generated client typings, the C++ mirror and its drift gate), and this task
 * deliberately adds no contract surface: the verbs and the roster entry it needs already exist.
 * When a second native-surface panel appears, THAT is the moment the manifest field earns its cost;
 * until then a second field would be a wire shape with one possible value.
 */
export const VIEWPORT_PANEL_ID = "builtin.viewport";

/**
 * The attribute `markPanelSlot` stamps on a native-surface panel's body element.
 *
 * It carries the whole coupling: `app.css` makes it transparent (the hole), and `viewportRegions`
 * below finds the live copies by it. Keying both on ONE attribute — rather than on the panel id in
 * two places — is what stops the stylesheet and the measurement drifting onto different elements,
 * which would present as a viewport that is drawn where nothing is transparent.
 */
export const VIEWPORT_SURFACE_ATTRIBUTE = "data-panel-native-surface";

/** The instance-id attribute `markPanelSlot` already stamps — the region id a viewport publishes. */
export const PANEL_INSTANCE_ATTRIBUTE = "data-panel-instance";

/**
 * The attribute stamped on a Dockview GROUP whose currently-shown panel is a native surface.
 *
 * ⚠ WHY A SECOND MARKER IS NEEDED AT ALL, and it is not obvious until you look at the DOM Dockview
 * actually builds. A `renderer: "always"` panel is NOT mounted inside its group: it lives in
 * `.dv-render-overlay`, a sibling subtree of `.dv-shell`, positioned over the group. So the group's
 * own box — which paints the docking surface — sits BEHIND the transparent slot and is reachable
 * from it by no selector at all (`:has()` walks descendants, not the paint order). Measured stack
 * under a viewport, outermost-last:
 *
 *     .ctx-panel-body | .dv-render-overlay | .dv-content-container | .dv-groupview | … | body | html
 *
 * and it was `.dv-groupview` painting `colors.panel` that made the "hole" opaque. Transparency on
 * the slot alone is therefore necessary and NOT sufficient — which is exactly the class of bug the
 * paint-stack assertion in `viewport.test.ts` exists to catch, and did.
 */
export const VIEWPORT_GROUP_ATTRIBUTE = "data-group-native-surface";

/** Whether a panel KIND renders as a native-surface hole. */
export function isNativeSurfacePanelId(panelId: string): boolean {
    return panelId === VIEWPORT_PANEL_ID;
}

/** The manifest-shaped sibling, for `rendererFor` and anything else holding a parsed manifest. */
export function isNativeSurfacePanel(manifest: PanelManifest): boolean {
    return isNativeSurfacePanelId(manifest.id);
}

/**
 * Measure every mounted viewport copy as a `viewport`-kind region, in PHYSICAL px.
 *
 * KEYED BY INSTANCE ID, not by panel id (c3): `builtin.viewport` is the one built-in declared
 * `instances.mode: "unlimited"`, so two Scene views are two regions, two render targets and two
 * cameras — and the Shell keys all three by the string reported here.
 *
 * DOM ORDER is the publish order, and it is the arbitration order the Shell's back-to-front
 * last-match-wins hit-test reads (input.h). Two viewports never overlap in a dock layout, so the
 * order is not load-bearing between them; it IS load-bearing against the chrome regions, which
 * boot.ts publishes AFTER these so a caption control always wins over dock content beneath it.
 *
 * A copy that is not laid out (mid-mount, a collapsed group, a zero-height drag state) measures
 * empty and is DROPPED rather than published as a degenerate rect — the Shell drops it again on its
 * own side, and the two agreeing is what keeps a mid-resize frame from allocating a 0x0 target.
 *
 * ⚠ SO IS A PARKED COPY, and this one is only visible once you look. Dockview does not DELETE an
 * `always`-rendered panel that stops being the active tab — that is the whole point of `always` —
 * it MOVES it out of the dock (measured: exactly one dock-height below it). The element is still
 * connected and still measures its full size, so a naive publisher would hand the Shell a perfectly
 * plausible rect for a viewport the human cannot see: a render target allocated, a scene drawn into
 * it and a layer composited, every frame, for a hidden tab. Anything lying entirely outside `root`
 * (the dock) is therefore withdrawn, and comes back the moment the copy is active again (a tab
 * activation IS a Dockview layout change, so the republish is already wired). Clipped against the
 * DOCK and not the window: the parked position is usually still inside the window, so a
 * window-relative test would have missed exactly the case this exists for. INTERSECTION, not
 * containment: a viewport legitimately extends past an edge mid-resize, and clipping that one to
 * nothing would blank a viewport the human is looking at.
 */
export function viewportRegions(
    root: ParentNode | null | undefined,
    devicePixelRatio: number = defaultDevicePixelRatio(),
): readonly ShellRegion[] {
    if (root === null || root === undefined) {
        return [];
    }
    const regions: ShellRegion[] = [];
    for (const element of root.querySelectorAll(`[${VIEWPORT_SURFACE_ATTRIBUTE}]`)) {
        const instanceId = element.getAttribute(PANEL_INSTANCE_ATTRIBUTE) ?? "";
        if (instanceId === "") {
            // An unkeyed surface would name a target and a camera by nothing. Skipped rather than
            // published under the KIND, which would make two copies collide on one region id.
            continue;
        }
        if (!intersectsHost(element, root)) {
            continue; // parked off-dock (see the header) — withdraw rather than publish
        }
        const region = physicalRegion(element, instanceId, REGION_KIND_VIEWPORT, devicePixelRatio);
        if (region !== null) {
            regions.push(region);
        }
    }
    return regions;
}

/**
 * Does `element` overlap the DOCK's own box at all?
 *
 * Measured against `root` (the dock element) rather than the browser's client area, because THAT is
 * where Dockview parks a hidden `always`-rendered panel: exactly one dock-height below the dock,
 * which on any window taller than the dock is still perfectly inside the client area. Clipping
 * against the window would therefore have missed the case this exists for.
 *
 * INTERSECTION, not containment (see the caller). A `root` that is not an Element — a Document, a
 * fragment, a documentless harness — has no box to clip against and answers TRUE: refusing every
 * region there would silently disable the whole mechanism rather than degrade it.
 */
function intersectsHost(element: Element, root: ParentNode): boolean {
    if (!(root instanceof Element)) {
        return true;
    }
    const rect = element.getBoundingClientRect();
    const host = root.getBoundingClientRect();
    return (
        rect.right > host.left &&
        rect.bottom > host.top &&
        rect.left < host.right &&
        rect.top < host.bottom
    );
}

/**
 * Mark every Dockview group whose ACTIVE panel is a native surface, and unmark the rest.
 *
 * ACTIVE, not merely present: a group holding a background viewport tab is showing something else
 * over its box and must keep its surface. Idempotent and total — it recomputes the whole set on
 * every call, mirroring `RegionMap::publish`'s wholesale replace for the same reason an incremental
 * update is wrong here: a group that stopped showing a viewport must LOSE the mark, and a diff is
 * how a stale mark outlives the panel that earned it.
 *
 * `panelIdOf` maps an instance id back to its KIND (c3's pair), so the caller keeps ownership of the
 * id grammar and this module never re-derives it.
 */
export function syncNativeSurfaceGroups(
    panels: readonly DockviewPanelHandle[],
    panelIdOf: (instanceId: string) => string,
): void {
    const marked = new Set<HTMLElement>();
    const seen = new Set<HTMLElement>();
    for (const panel of panels) {
        const element = panel.group?.element;
        if (element === undefined) {
            continue; // a harness handle, or a panel Dockview has not grouped yet
        }
        seen.add(element);
        const activeId = panel.group?.activePanel?.id;
        if (activeId !== undefined && isNativeSurfacePanelId(panelIdOf(activeId))) {
            marked.add(element);
        }
    }
    for (const element of seen) {
        if (marked.has(element)) {
            element.setAttribute(VIEWPORT_GROUP_ATTRIBUTE, "");
        } else {
            element.removeAttribute(VIEWPORT_GROUP_ATTRIBUTE);
        }
    }
}
