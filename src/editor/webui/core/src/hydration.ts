// The HYDRATION RUNTIME v1 (M9 e05d1, design 04 §4) — the layer that turns a C++ panel's uitree
// render into live DOM and turns user interaction back into commands and gesture verbs.
//
// THE SHAPE OF THE THING. Design 04 §4 defines four steps, and this file is all four:
//
//   1. request the panel's uitree render over the bridge          -> `PanelClient.render` (panels.ts)
//   2. mount it; bind interactions                                -> `mount` + `#bindInteractions`
//        * focusables follow `focus_order`                        -> `#focusOrder`, arrow/Home/End nav
//        * activation dispatches the node's bound command id      -> `#activate`
//        * continuous gestures map to begin/extend/commit/cancel  -> `#bindGestures`
//   3. re-renders are INCREMENTAL DOM patches keyed by stable node ids -> `#patchElement`
//   4. rich widgets are widget classes keyed by node ROLE/TYPE, presentation-only -> `WIDGET_CLASSES`
//
// IT IS PANEL-AGNOSTIC, STRUCTURALLY. There is no panel id anywhere in this file, no branch on one,
// and no knowledge of what a diagnostic, an entity or a component is. Everything it acts on comes
// from the markup the C++ model produced: `data-command` says what a node dispatches, `role` says
// which widget class it gets, `focusOrder` says how the keyboard walks it. Adding a panel — which is
// exactly what e05d3 does for Scene tree and Inspector — requires ZERO change here. A reviewer
// should be able to confirm that by grepping this file for a panel id and finding none.
//
// ── ON MOUNTING HTML FROM THE BRIDGE ────────────────────────────────────────────────────────────
// `PanelRender.html` is assigned through `innerHTML`, and that is a deliberate, layered decision
// rather than a convenience:
//
//   * THE CONTROL is the C-F6 escaping contract on the NATIVE side: every interpolation in
//     `uitree::render_html` — id, label, command, text — goes through `escape_html_text`, which
//     escapes the five HTML-significant characters and replaces C0 controls. Authored project
//     strings (entity names, `notes`, any string field) are the threat, and they are neutralised
//     before they ever reach this file. `gui/uitree/tests/test_escaping.cpp` asserts that with
//     adversarial inputs.
//   * THE BACKSTOP is the CSP the assets are served under: `script-src 'self'` with no
//     `unsafe-inline` (see `app_csp_header()`). HTML parsing via innerHTML does not execute
//     `<script>` at all, and the CSP additionally blocks inline event-handler attributes — so even a
//     leak in the escaping contract cannot execute script in the trusted editor-core zone.
//   * THE TAG SET is closed: `render_html` emits only the twelve tags `role_html_tag` maps to.
//
// WHAT WOULD MAKE THIS UNSAFE, stated so a future edit cannot drift into it by accident: mounting
// HTML from any source OTHER than a `panel.render` response — a third-party iframe panel's content
// (04 §5), a daemon payload, anything a package contributed. Those live behind a sandboxed iframe on
// a DIFFERENT origin by design, and must never be routed through this function.
// ─────────────────────────────────────────────────────────────────────────────────────────────────

import type { GestureVerb, PanelClient, PanelRender } from "./panels.js";

/**
 * Presentation-only widget classes, keyed by node ROLE (04 §4 step 4).
 *
 * The VALUE semantics stay in the C++ model and the write path — these classes carry appearance and
 * nothing else, which is why a missing entry is harmless rather than a defect. Keyed by role because
 * role is what the uitree vocabulary actually publishes (`uitree::role_token`); keying on anything
 * derived from a panel id would be the special-casing this runtime exists to avoid.
 */
export const WIDGET_CLASSES: Readonly<Record<string, string>> = {
    tree: "ctx-widget-tree",
    treeitem: "ctx-widget-treeitem",
    list: "ctx-widget-list",
    listitem: "ctx-widget-listitem",
    button: "ctx-widget-button",
    textbox: "ctx-widget-textbox",
    checkbox: "ctx-widget-checkbox",
    heading: "ctx-widget-heading",
    status: "ctx-widget-status",
    region: "ctx-widget-region",
    group: "ctx-widget-group",
    text: "ctx-widget-text",
};

/** The attribute carrying a node's STABLE model id, which every DOM patch is keyed by. */
export const NODE_ID_ATTRIBUTE = "data-node-id";

/** The attribute `uitree::render_html` emits for a node's bound command. */
export const COMMAND_ATTRIBUTE = "data-command";

/** Attributes the patcher synchronises on a reused element. Everything else is structural. */
const SYNCED_ATTRIBUTES = ["role", "aria-label", "tabindex", COMMAND_ATTRIBUTE] as const;

/** What a hydrated panel reports about itself — the surface a test or a diagnostic reads. */
export interface HydrationStats {
    /** Full mounts (a first render, or a re-render the patcher could not reuse anything from). */
    readonly mounts: number;
    /** Incremental patches applied. */
    readonly patches: number;
    /** Elements the patcher REUSED rather than recreating — the measure of "incremental". */
    readonly reused: number;
    /** Commands dispatched through the bridge. */
    readonly dispatches: number;
    /** Gesture verbs sent. */
    readonly gestures: number;
}

export interface HydrationOptions {
    /** Whether this panel accepts gestures. From the manifest's `gestures` — never guessed. */
    readonly gestures: boolean;
    /** Called after any successful dispatch, so the host can re-render on the new revision. */
    readonly onDispatched?: (revision: number) => void;
}

/**
 * One panel's live DOM binding.
 *
 * Owns exactly one container element for its whole life. `dispose()` releases every listener — a
 * panel closed without disposing would keep its handlers attached to a detached tree, which is the
 * ordinary way a docking editor leaks.
 */
export class HydrationRuntime {
    readonly #container: HTMLElement;
    readonly #client: PanelClient;
    readonly #panelId: string;
    readonly #gesturesEnabled: boolean;
    readonly #onDispatched: ((revision: number) => void) | undefined;

    #focusOrder: readonly string[] = [];
    #commands: ReadonlySet<string> = new Set();
    #revision = -1;
    #gestureActive = false;
    #disposed = false;

    #mounts = 0;
    #patches = 0;
    #reused = 0;
    #dispatches = 0;
    #gestures = 0;

    // Bound once and reused, so `removeEventListener` in `dispose` can actually match them. A fresh
    // arrow function per addEventListener call is the classic reason a "disposed" component keeps
    // receiving events.
    readonly #onClick = (event: MouseEvent): void => {
        void this.#activateFrom(event.target);
    };
    readonly #onKeyDown = (event: KeyboardEvent): void => {
        this.#handleKey(event);
    };
    readonly #onPointerDown = (event: PointerEvent): void => {
        this.#handlePointer(event, "begin");
    };
    readonly #onPointerMove = (event: PointerEvent): void => {
        if (this.#gestureActive) {
            this.#handlePointer(event, "extend");
        }
    };
    readonly #onPointerUp = (event: PointerEvent): void => {
        if (this.#gestureActive) {
            this.#handlePointer(event, "commit");
        }
    };
    readonly #onPointerCancel = (event: PointerEvent): void => {
        if (this.#gestureActive) {
            this.#handlePointer(event, "cancel");
        }
    };

    constructor(
        container: HTMLElement,
        client: PanelClient,
        panelId: string,
        options: HydrationOptions,
    ) {
        this.#container = container;
        this.#client = client;
        this.#panelId = panelId;
        this.#gesturesEnabled = options.gestures;
        this.#onDispatched = options.onDispatched;
        this.#bindInteractions();
    }

    get stats(): HydrationStats {
        return {
            mounts: this.#mounts,
            patches: this.#patches,
            reused: this.#reused,
            dispatches: this.#dispatches,
            gestures: this.#gestures,
        };
    }

    /** The revision currently mounted; `-1` before the first render. */
    get revision(): number {
        return this.#revision;
    }

    /**
     * Fetch and apply the panel's current render.
     *
     * Returns false when the Shell could not render it (unknown, not hosted, transport refusal) —
     * an ordinary answer for a panel still blocked behind the D10 boundary, so the caller degrades
     * rather than throwing.
     */
    async refresh(): Promise<boolean> {
        if (this.#disposed) {
            return false;
        }
        const render = await this.#client.render(this.#panelId);
        if (render === null) {
            return false;
        }
        this.apply(render);
        return true;
    }

    /**
     * Apply a render payload: a full mount the first time, an incremental patch afterwards.
     *
     * A payload whose revision is already mounted is a NO-OP. That is what makes an idle editor cost
     * nothing: the host may poll freely, and only a real model change touches the DOM.
     */
    apply(render: PanelRender): void {
        if (this.#disposed || render.revision === this.#revision) {
            return;
        }
        this.#focusOrder = render.focusOrder;
        this.#commands = new Set(render.commands.map((command) => command.id));

        const incoming = this.#parse(render.html);
        if (this.#container.childElementCount === 0) {
            this.#adoptAll(incoming);
            this.#mounts += 1;
        } else {
            // FOCUS IS PRESERVED ACROSS A PATCH, by node id rather than by element identity: the
            // patcher may move or replace elements, and a user typing into a panel that re-rendered
            // because a diagnostic arrived elsewhere must not lose their place.
            const focusedNodeId = this.#focusedNodeId();
            this.#patchElement(this.#container, incoming);
            this.#patches += 1;
            if (focusedNodeId !== null) {
                this.#element(focusedNodeId)?.focus();
            }
        }
        this.#revision = render.revision;
    }

    /** Move keyboard focus to a node id, if it is currently mounted and focusable. */
    focusNode(nodeId: string): boolean {
        const element = this.#element(nodeId);
        if (element === null || !element.hasAttribute("tabindex")) {
            return false;
        }
        element.focus();
        return true;
    }

    dispose(): void {
        if (this.#disposed) {
            return;
        }
        this.#disposed = true;
        this.#container.removeEventListener("click", this.#onClick);
        this.#container.removeEventListener("keydown", this.#onKeyDown);
        this.#container.removeEventListener("pointerdown", this.#onPointerDown);
        this.#container.removeEventListener("pointermove", this.#onPointerMove);
        this.#container.removeEventListener("pointerup", this.#onPointerUp);
        this.#container.removeEventListener("pointercancel", this.#onPointerCancel);
        this.#container.replaceChildren();
    }

    // --------------------------------------------------------------------------- interaction

    #bindInteractions(): void {
        // DELEGATED at the container, not per node: the tree is re-rendered constantly, and
        // per-node listeners would have to be re-attached on every patch — the exact bookkeeping
        // that goes wrong and leaves a panel half-live after a re-render.
        this.#container.addEventListener("click", this.#onClick);
        this.#container.addEventListener("keydown", this.#onKeyDown);
        if (this.#gesturesEnabled) {
            // Bound ONLY when the panel's manifest declares gestures. A panel that reports
            // `gestures: false` — Problems, every read-only observer — never even installs these,
            // so it cannot emit a verb the Shell would only refuse.
            this.#container.addEventListener("pointerdown", this.#onPointerDown);
            this.#container.addEventListener("pointermove", this.#onPointerMove);
            this.#container.addEventListener("pointerup", this.#onPointerUp);
            this.#container.addEventListener("pointercancel", this.#onPointerCancel);
        }
    }

    #handleKey(event: KeyboardEvent): void {
        // ACTIVATION: Enter and Space, the ARIA-standard pair. Together with the click handler this
        // is what makes every command-bound affordance reachable without a pointer — R-CLI-001
        // CLI-completeness as a structural accessibility property, which `uitree::audit_a11y`
        // already asserts on the model side.
        if (event.key === "Enter" || event.key === " ") {
            const node = this.#nodeFrom(event.target);
            if (node !== null && node.hasAttribute(COMMAND_ATTRIBUTE)) {
                // Only prevented once we KNOW we are handling it: swallowing Space unconditionally
                // would break typing in a textbox node.
                event.preventDefault();
                void this.#activate(node);
            }
            return;
        }

        if (this.#gestureActive && event.key === "Escape") {
            // Escape cancels an in-flight gesture — the keyboard half of `pointercancel`, and the
            // reason a drag cannot be left permanently open by a user who changes their mind.
            event.preventDefault();
            void this.#sendGesture("cancel", this.#focusedNodeId() ?? "", 0, 0);
            return;
        }

        // KEYBOARD NAVIGATION along the MODEL's declared focus order (04 §4 step 2). Deliberately
        // not re-derived from the DOM: `uitree::focus_order` is the panel's authority on what order
        // its focusables are in, and a runtime that invented its own would disagree with the a11y
        // audit that gates the panel.
        const delta =
            event.key === "ArrowDown" ? 1 : event.key === "ArrowUp" ? -1 : 0;
        if (delta === 0 && event.key !== "Home" && event.key !== "End") {
            return;
        }
        if (this.#focusOrder.length === 0) {
            return;
        }
        const current = this.#focusedNodeId();
        const index = current === null ? -1 : this.#focusOrder.indexOf(current);
        let next: number;
        if (event.key === "Home") {
            next = 0;
        } else if (event.key === "End") {
            next = this.#focusOrder.length - 1;
        } else {
            // Clamped, NOT wrapped: wrapping from the last row to the first reads as a jump the user
            // did not ask for, and screen-reader users lose their place in the list.
            next = Math.min(Math.max(index + delta, 0), this.#focusOrder.length - 1);
        }
        const target = this.#focusOrder[next];
        if (target !== undefined && this.focusNode(target)) {
            event.preventDefault();
        }
    }

    async #activateFrom(target: EventTarget | null): Promise<void> {
        const node = this.#nodeFrom(target);
        if (node !== null && node.hasAttribute(COMMAND_ATTRIBUTE)) {
            await this.#activate(node);
        }
    }

    async #activate(node: Element): Promise<void> {
        const commandId = node.getAttribute(COMMAND_ATTRIBUTE) ?? "";
        const nodeId = node.getAttribute(NODE_ID_ATTRIBUTE) ?? "";
        if (commandId === "" || nodeId === "") {
            return;
        }
        // CHECKED AGAINST THE MODEL'S OWN COMMAND SET before it leaves the renderer. The mounted DOM
        // can outlive the render it came from (a patch is in flight, a click lands mid-update), and
        // dispatching a command the panel no longer exposes would be acting on a stale tree. The
        // Shell refuses it too — this is the cheap half of a check worth having on both sides.
        if (!this.#commands.has(commandId)) {
            return;
        }
        const result = await this.#client.command(this.#panelId, commandId, nodeId);
        this.#dispatches += 1;
        if (result !== null && result.dispatched) {
            this.#onDispatched?.(result.revision);
        }
    }

    // ------------------------------------------------------------------------------ gestures

    #handlePointer(event: PointerEvent, verb: GestureVerb): void {
        const node = this.#nodeFrom(event.target);
        if (node === null) {
            return;
        }
        const nodeId = node.getAttribute(NODE_ID_ATTRIBUTE) ?? "";
        if (nodeId === "") {
            return;
        }
        if (verb === "begin") {
            if (this.#gestureActive) {
                return;
            }
            this.#gestureActive = true;
        } else if (verb === "commit" || verb === "cancel") {
            this.#gestureActive = false;
        }
        // Coordinates are node-RELATIVE, so a model reasoning about a paint stroke or a drag never
        // has to know where the panel sits in the window — the property that makes a gesture survive
        // a dock, a split or a resize unchanged.
        const bounds = node.getBoundingClientRect();
        void this.#sendGesture(
            verb,
            nodeId,
            Math.round(event.clientX - bounds.left),
            Math.round(event.clientY - bounds.top),
        );
    }

    async #sendGesture(verb: GestureVerb, nodeId: string, x: number, y: number): Promise<void> {
        if (nodeId === "") {
            this.#gestureActive = false;
            return;
        }
        const result = await this.#client.gesture(this.#panelId, verb, { nodeId, x, y });
        this.#gestures += 1;
        if (result !== null && result.dispatched) {
            this.#onDispatched?.(result.revision);
        }
    }

    // ---------------------------------------------------------------------------- DOM plumbing

    /**
     * Parse a render payload into an INERT tree.
     *
     * `<template>` rather than a detached `<div>`: template contents are parsed into an inert
     * document, so nothing in them can load a resource or run at parse time. Every node is then
     * id-normalised before it is adopted.
     */
    #parse(html: string): HTMLTemplateElement {
        const template = document.createElement("template");
        template.innerHTML = html;
        this.#normalise(template.content);
        return template;
    }

    /**
     * Move each node's model id to `data-node-id` and rewrite the DOM `id` to a panel-scoped one.
     *
     * WHY: `uitree::render_html` emits the model's node id as the DOM `id` verbatim, and model ids
     * are unique only WITHIN a panel — two panels mounted in one document would both carry
     * `id="root"`. Duplicate ids are invalid HTML and actively harmful to assistive technology,
     * which resolves references by id. Scoping them keeps the document valid; keeping the original
     * on `data-node-id` is what every patch and every dispatch below is keyed by, so the STABLE id
     * the model owns is never lost — it just stops being the thing the browser dedupes on.
     */
    #normalise(root: ParentNode): void {
        for (const element of root.querySelectorAll("[id]")) {
            const nodeId = element.getAttribute("id") ?? "";
            if (nodeId === "") {
                continue;
            }
            element.setAttribute(NODE_ID_ATTRIBUTE, nodeId);
            element.setAttribute("id", `${this.#panelId}::${nodeId}`);
            const role = element.getAttribute("role") ?? "";
            const widgetClass = WIDGET_CLASSES[role];
            if (widgetClass !== undefined) {
                element.classList.add(widgetClass);
            }
        }
    }

    #adoptAll(template: HTMLTemplateElement): void {
        this.#container.replaceChildren(document.importNode(template.content, true));
    }

    /**
     * Incremental DOM patch, keyed by stable node ids (04 §4 step 3).
     *
     * Reuses an existing element when the SAME node id reappears with the same tag, syncing only the
     * attributes and own text that changed, and adopts a fresh subtree otherwise. Reuse is what
     * preserves scroll position, focus, text selection and CSS transition state across a re-render —
     * a full replace loses all four, which in a live editor reads as the panel "flickering" every
     * time an unrelated diagnostic arrives.
     */
    #patchElement(target: Element | DocumentFragment, source: ParentNode): void {
        const sourceChildren = Array.from(source.children);
        for (let index = 0; index < sourceChildren.length; index += 1) {
            const incoming = sourceChildren[index];
            if (incoming === undefined) {
                continue;
            }
            const nodeId = incoming.getAttribute(NODE_ID_ATTRIBUTE) ?? "";
            const existing = nodeId === "" ? null : this.#element(nodeId);
            const anchor = target.children[index] ?? null;

            if (existing !== null && existing.tagName === incoming.tagName) {
                this.#syncAttributes(existing, incoming);
                this.#syncOwnText(existing, incoming);
                this.#patchElement(existing, incoming);
                this.#reused += 1;
                if (anchor !== existing) {
                    target.insertBefore(existing, anchor);
                }
                continue;
            }
            target.insertBefore(document.importNode(incoming, true), anchor);
        }
        // Trailing elements the new tree does not have. Removed from the END so the indices above
        // stay valid while this runs.
        while (target.children.length > sourceChildren.length) {
            target.lastElementChild?.remove();
        }
    }

    #syncAttributes(target: Element, source: Element): void {
        for (const name of SYNCED_ATTRIBUTES) {
            const value = source.getAttribute(name);
            if (value === null) {
                target.removeAttribute(name);
            } else if (target.getAttribute(name) !== value) {
                target.setAttribute(name, value);
            }
        }
    }

    /**
     * Sync a node's OWN text — the leading text node `uitree::render_html` writes before an element's
     * children — without touching the child elements the patcher is about to reconcile.
     *
     * `textContent = x` would be catastrophic here: it destroys every child element, turning every
     * patch into a full subtree replace and defeating the point of keying by node id.
     */
    #syncOwnText(target: Element, source: Element): void {
        const incoming = source.firstChild;
        const incomingText =
            incoming !== null && incoming.nodeType === Node.TEXT_NODE ? (incoming.nodeValue ?? "") : null;
        const existing = target.firstChild;
        const existingIsText = existing !== null && existing.nodeType === Node.TEXT_NODE;

        if (incomingText === null) {
            if (existingIsText) {
                existing.remove();
            }
            return;
        }
        if (existingIsText) {
            if (existing.nodeValue !== incomingText) {
                existing.nodeValue = incomingText;
            }
            return;
        }
        target.insertBefore(document.createTextNode(incomingText), target.firstChild);
    }

    /**
     * Find a mounted element by its STABLE model node id.
     *
     * Scoped to this panel's container and matched on `data-node-id`, never `getElementById`: ids
     * are panel-scoped in the document but the MODEL id is what every caller here holds, and a
     * document-wide lookup would find another panel's node of the same name.
     */
    #element(nodeId: string): HTMLElement | null {
        return this.#container.querySelector<HTMLElement>(
            `[${NODE_ID_ATTRIBUTE}="${CSS.escape(nodeId)}"]`,
        );
    }

    /** The nearest ancestor carrying a model node id — the node an event actually happened on. */
    #nodeFrom(target: EventTarget | null): Element | null {
        if (!(target instanceof Element)) {
            return null;
        }
        const node = target.closest(`[${NODE_ID_ATTRIBUTE}]`);
        return node !== null && this.#container.contains(node) ? node : null;
    }

    #focusedNodeId(): string | null {
        const active = document.activeElement;
        if (!(active instanceof Element) || !this.#container.contains(active)) {
            return null;
        }
        return active.getAttribute(NODE_ID_ATTRIBUTE);
    }
}
