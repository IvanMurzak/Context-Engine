// The a4 T1 (editor-UX c4, `07-ui-states.md` §5) — the duplicated panel title.
//
// Eight of the nine C++ panel models emit a `Role::heading` as the FIRST CHILD of the panel root with
// text equal to their roster title, and Dockview separately renders the SAME title into the tab — so
// it prints twice. `hydration.ts`'s `#hideDuplicateHeading` marks that heading `VISUALLY_HIDDEN_CLASS`
// instead of deleting it: present in the accessibility tree, absent from the picture.
//
// BOTH HALVES, in every positive case, over the SERVED `kit.css` — never the class name alone. A test
// that only asserted `classList.contains(VISUALLY_HIDDEN_CLASS)` would pass unchanged if `kit.css`
// forgot to style the class (or styled it with `display: none`, which silently breaks the whole
// point). `getComputedStyle` is what proves the shipped stylesheet actually resolves the clip-rect
// pattern in a real browser — the same reasoning `kit.test.ts`'s header gives for why its tier exists
// alongside `check_kit_tokens.py`. One half alone would pass with the node deleted (the DoD's own
// words): "not visible" alone permits `display:none`; "still a heading with its name" alone permits
// never having applied the class at all. Every positive case below asserts both.

import { assert, assertEqual, type TestCase } from "./harness.js";
import { ShellBridge, type BridgeQuery, type BridgeQueryFunction } from "../bridge.js";
import { HydrationRuntime } from "../hydration.js";
import { PanelClient, type PanelRender } from "../panels.js";
import { VISUALLY_HIDDEN_CLASS } from "../../../kit/src/index.js";

const PANEL_ID = "a4.fixture";
const PANEL_TITLE = "Scene Tree";

/** A `PanelClient` over a Shell that never refuses — nothing here exercises a dispatch. */
function noopClient(): PanelClient {
    const query: BridgeQueryFunction = (request: BridgeQuery): number => {
        const parsed = JSON.parse(request.request) as { id: number };
        request.onSuccess(
            JSON.stringify({
                jsonrpc: "2.0",
                id: parsed.id,
                result: { dispatched: false, revision: 1 },
            }),
        );
        return parsed.id;
    };
    return new PanelClient(new ShellBridge(query));
}

function render(html: string, revision: number): PanelRender {
    return {
        panelId: PANEL_ID,
        instanceId: PANEL_ID,
        revision,
        html,
        focusOrder: [],
        commands: [],
    };
}

interface Mounted {
    readonly container: HTMLElement;
    readonly runtime: HydrationRuntime;
    dispose(): void;
}

function mount(html: string, panelTitle: string = PANEL_TITLE): Mounted {
    const container = document.createElement("div");
    document.body.appendChild(container);
    const runtime = new HydrationRuntime(container, noopClient(), PANEL_ID, {
        gestures: false,
        panelTitle,
    });
    runtime.apply(render(html, 1));
    return {
        container,
        runtime,
        dispose(): void {
            runtime.dispose();
            container.remove();
        },
    };
}

function mountedNode(mounted: Mounted, nodeId: string): HTMLElement {
    const found = mounted.container.querySelector<HTMLElement>(`[data-node-id="${nodeId}"]`);
    assert(found !== null, `the fixture must mount a node ${nodeId}`);
    return found as HTMLElement;
}

/**
 * Judged the same way the standard clip-rect pattern is meant to be judged: pulled out of the visible
 * box (near-zero dimensions, clipped, off the normal flow) WITHOUT the two properties that would also
 * pull it out of the accessibility tree (`display: none`, `visibility: hidden`) — see `kit.css`'s own
 * comment on why those two are unusable here.
 */
function isVisuallyHiddenButAccessible(element: HTMLElement): boolean {
    const computed = window.getComputedStyle(element);
    return (
        computed.position === "absolute" &&
        computed.width === "1px" &&
        computed.height === "1px" &&
        computed.overflow === "hidden" &&
        computed.display !== "none" &&
        computed.visibility !== "hidden"
    );
}

export const panelTitleTests: readonly TestCase[] = [
    {
        name: "a4: a heading duplicating the panel title is visually hidden, but stays an accessible heading",
        run: () => {
            const html =
                `<section id="a4.panel" role="region" aria-label="${PANEL_TITLE}">` +
                `<h2 id="a4.heading" role="heading" aria-label="${PANEL_TITLE}">${PANEL_TITLE}</h2>` +
                `<div id="a4.body" role="group"></div>` +
                `</section>`;
            const mounted = mount(html);
            try {
                const node = mountedNode(mounted, "a4.heading");

                // Half 1 — not visible, over the SERVED kit.css.
                assert(
                    node.classList.contains(VISUALLY_HIDDEN_CLASS),
                    "the duplicate heading carries the visually-hidden class",
                );
                assert(
                    isVisuallyHiddenButAccessible(node),
                    "the served kit.css resolves the class to the clip-rect pattern, without " +
                        "display:none or visibility:hidden",
                );

                // Half 2 — still reachable, with its name. One half alone would pass with the node
                // deleted, which is exactly what these four assertions rule out.
                assertEqual(node.getAttribute("role"), "heading", "still a heading in the model's terms");
                assertEqual(node.getAttribute("aria-label"), PANEL_TITLE, "the accessible name survives");
                assertEqual(node.textContent, PANEL_TITLE, "the node's own text is never deleted");
                assert(mounted.container.contains(node), "the node is still mounted, not removed");
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "a4: a heading whose text differs from the panel title stays visible",
        run: () => {
            const html =
                `<section id="a4.panel" role="region" aria-label="${PANEL_TITLE}">` +
                `<h2 id="a4.heading" role="heading" aria-label="Not the title">Not the title</h2>` +
                `</section>`;
            const mounted = mount(html);
            try {
                const node = mountedNode(mounted, "a4.heading");
                assert(
                    !node.classList.contains(VISUALLY_HIDDEN_CLASS),
                    "a text mismatch is left alone — matched on rendered text, never on a node id",
                );
                assert(!isVisuallyHiddenButAccessible(node), "and stays genuinely visible");
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "a4: a heading with the panel's title, but NOT the panel root's first child, stays visible",
        run: () => {
            const html =
                `<section id="a4.panel" role="region" aria-label="${PANEL_TITLE}">` +
                `<div id="a4.lead" role="group"></div>` +
                `<h2 id="a4.heading" role="heading" aria-label="${PANEL_TITLE}">${PANEL_TITLE}</h2>` +
                `</section>`;
            const mounted = mount(html);
            try {
                const node = mountedNode(mounted, "a4.heading");
                assert(
                    !node.classList.contains(VISUALLY_HIDDEN_CLASS),
                    "only the panel root's FIRST child is ever a candidate",
                );
                assert(!isVisuallyHiddenButAccessible(node), "and stays genuinely visible");
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        // Mirrors `tilemap_paint_panel.cpp:60` exactly: `set_label` only, no `set_text` — the model
        // renders an empty `<h2>`, so the text-equality rule correctly does nothing there. This is NOT
        // a miss — see `01-current-architecture.md` §10 and `07-ui-states.md` §5.
        name: "a4: Tilemap Painter's shape (a label-only heading) never duplicates anything and stays visible",
        run: () => {
            const html =
                `<section id="a4.panel" role="region" aria-label="Tilemap Painter">` +
                `<h2 id="a4.heading" role="heading" aria-label="Tilemap Painter"></h2>` +
                `</section>`;
            const mounted = mount(html, "Tilemap Painter");
            try {
                const node = mountedNode(mounted, "a4.heading");
                assert(
                    !node.classList.contains(VISUALLY_HIDDEN_CLASS),
                    "an empty rendered text never equals a non-empty title",
                );
                assertEqual(
                    node.getAttribute("aria-label"),
                    "Tilemap Painter",
                    "the accessible label is untouched either way",
                );
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "a4: the hidden class survives an incremental re-render (patch), not only the first mount",
        run: () => {
            const html =
                `<section id="a4.panel" role="region" aria-label="${PANEL_TITLE}">` +
                `<h2 id="a4.heading" role="heading" aria-label="${PANEL_TITLE}">${PANEL_TITLE}</h2>` +
                `<output id="a4.status" role="status" aria-label="Status">v1</output>` +
                `</section>`;
            const mounted = mount(html);
            try {
                const before = mountedNode(mounted, "a4.heading");
                assert(before.classList.contains(VISUALLY_HIDDEN_CLASS), "hidden on the first mount");

                // A second revision — the SAME complete panel markup, `render.html` is never a partial
                // diff — with an unrelated status change, so the patcher reuses rather than remounts.
                const patched = html.replace(">v1<", ">v2<");
                mounted.runtime.apply(render(patched, 2));

                const after = mountedNode(mounted, "a4.heading");
                assert(after === before, "the SAME element is reused across the patch, not recreated");
                assert(
                    after.classList.contains(VISUALLY_HIDDEN_CLASS),
                    "and it is still hidden after the patch — #hideDuplicateHeading re-derives it on " +
                        "every #parse, and #syncAttributes keeps a reused element's class in step",
                );
            } finally {
                mounted.dispose();
            }
        },
    },
];
