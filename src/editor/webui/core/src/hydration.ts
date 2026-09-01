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
//        * a value control STAGES on `input` and ENDS on `change` -> `#stageFrom` / `#commit`
//          (M9 e09e-1, design 05 §8 — the DOM half of the write chain; `commandValueFor` encodes
//           what the human entered as the JSON literal the panel's C++ provider reads)
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
import { VISUALLY_HIDDEN_CLASS, WIDGET_CLASSES, widgetClassForRole } from "../../kit/src/index.js";

/**
 * The presentation-only widget classes, keyed by node ROLE (04 §4 step 4).
 *
 * ⚠ THE KIT OWNS THIS MAP SINCE M9 e06c1 — `kit/src/index.ts`, beside the stylesheet that styles it.
 * It is re-exported here so every importer and the `index.ts` barrel are unchanged, and because this
 * is where a reader of the hydration runtime expects to find it.
 *
 * The move was the point of e06c1, not bookkeeping. The map lived here while `app/app.css` styled
 * NINE of its twelve classes: two owners, in two packages, with nothing tying them together — a role
 * could gain a class and never gain an appearance, and nothing would report it. With the names and
 * the rules in one package, `webui-kit-role-coverage` can assert the three-way agreement (the C++
 * `uitree::role_token` vocabulary ↔ the map ↔ `kit.css`'s selectors) on every `build` leg, and
 * `webui-kit-tokens-only` can hold the kit to tokens with no raw literal anywhere in it.
 *
 * A missing entry is therefore NO LONGER "harmless" as this comment used to claim: it is a red
 * build. That is the whole improvement — an unthemed role was previously invisible.
 */
export { WIDGET_CLASSES };

/** The attribute carrying a node's STABLE model id, which every DOM patch is keyed by. */
export const NODE_ID_ATTRIBUTE = "data-node-id";

/** The attribute `uitree::render_html` emits for a node's bound command. */
export const COMMAND_ATTRIBUTE = "data-command";

/**
 * Attributes the patcher synchronises on a reused element. Everything else is structural.
 *
 * `class` is in the list because the WIDGET CLASS MUST FOLLOW THE ROLE on a reused element.
 * `render_html` never emits `class` (see `uitree::render_into` — it writes only id / role /
 * aria-label / tabindex / data-command, plus type / value / checked on the void `<input>` roles),
 * so the sole class an incoming element carries is the one `#normalise` derives from its role.
 * Syncing it therefore replaces a stale class automatically: three role pairs share an HTML tag and
 * so survive the reuse test — tree/list (`ul`), treeitem/listitem (`li`), textbox/checkbox
 * (`input`) — and without this an element whose role changed within a pair would keep the previous
 * class forever.
 *
 * `type` / `value` / `checked` carry the void `<input>` roles' state (M9 e05d3: a void element has
 * no text child for `#syncOwnText` to update, so a changed field value or checkbox state moves ONLY
 * through these attributes — without them a reused input would show the stale value forever).
 */
const SYNCED_ATTRIBUTES =
    ["role", "aria-label", "tabindex", "class", "type", "value", "checked", COMMAND_ATTRIBUTE] as const;

/**
 * Is this event target a text-entry control?
 *
 * Used to keep panel-level keyboard navigation from swallowing keys that belong to the caret. Only
 * the genuinely text-like `<input>` types qualify — a checkbox or a button `<input>` is an ordinary
 * activatable affordance and must keep its Space/Enter handling.
 */
function isTextEntry(target: EventTarget | null): boolean {
    if (target instanceof HTMLTextAreaElement) {
        return true;
    }
    if (target instanceof HTMLElement && target.isContentEditable) {
        return true;
    }
    if (!(target instanceof HTMLInputElement)) {
        return false;
    }
    return !["checkbox", "radio", "button", "submit", "reset"].includes(target.type);
}

/**
 * The VALUE-CARRYING controls a panel model can render (M9 e09e-1).
 *
 * `uitree::role_html_tag` maps the `textbox` and `checkbox` roles onto `<input>`, so those are the
 * only two shapes reachable today; `<textarea>` is accepted alongside them because `isTextEntry`
 * already recognises one and a future multi-line role would arrive as one.
 *
 * ⚠ THAT FUTURE ROLE MUST ALSO NARROW `#handleKey`'S ENTER BRANCH. Enter there is the commit gesture
 * and is `preventDefault`ed — harmless in a single-line `<input>`, whose Enter has no default action
 * outside a form, but in a `<textarea>` it is the keystroke that inserts a newline, so a multi-line
 * field would become unable to hold one. Unreachable today (no role maps to `<textarea>`), and
 * recorded here rather than pre-decided: whether such a field commits on Enter, Shift+Enter or blur
 * is that role's design question, not this helper's.
 *
 * DELIBERATELY STRUCTURAL, not panel-specific: this file knows no panel id and no field (see the
 * header), so "does this node carry a value" is asked of the DOM element, never of a roster entry.
 */
function valueControl(target: EventTarget | null): HTMLInputElement | HTMLTextAreaElement | null {
    if (target instanceof HTMLInputElement || target instanceof HTMLTextAreaElement) {
        return target;
    }
    return null;
}

/** Does `text` read as a JSON literal? The one question the encoder below asks of a rendered value. */
function parsesAsJson(text: string): boolean {
    try {
        JSON.parse(text);
        return true;
    } catch {
        return false;
    }
}

/**
 * Encode a control's CURRENT value as the JSON LITERAL STRING `PanelClient.command` carries
 * (M9 e09e-1, design 05 §8) — the DOM half of the write chain's first link.
 *
 * A checkbox is unambiguous: its state IS a JSON boolean.
 *
 * A TEXT CONTROL IS NOT, and the encoding has to decide one thing — whether the human's text is a
 * JSON *string* (quote + escape it) or some other JSON literal to be passed through (`1.5`, `true`,
 * `{"a":1}`). The signal used is the MODEL'S OWN RENDERED VALUE — the `value` ATTRIBUTE, which
 * `uitree::render_html` wrote and which the browser leaves untouched while the user types (that is
 * `defaultValue`, as against the live `value` property). `inspector_panel.cpp`'s `render_value` (a
 * file-static, not a member) prints a string's characters BARE and every other type as its canonical
 * literal, so "the rendered value does not parse as JSON" is exactly "this field currently holds a
 * string" — and the edit is encoded to the SAME JSON type the field already has.
 *
 * EXPORTED so the T1 tier can drive THIS function rather than a copy of it (`hydration.test.ts`),
 * following `boot.ts`'s precedent. It is not panel-facing API.
 *
 * WHY NOT KEY OFF THE USER'S OWN TEXT (parse it, else quote it)? Because that mis-types in the
 * DANGEROUS direction: typing `abc` into a number field would be quoted into a valid string literal
 * and staged, silently changing the field's type. Keying off the model instead makes that case
 * unparseable on the C++ side, so it is REFUSED rather than written — fail-closed, which is the whole
 * discipline `parse_widget_kind` / `stage_edit` already apply one layer down.
 *
 * ⚠ ONE CASE STAYS UNDECIDABLE HERE (#434), and it is NOT a coincidence this runtime merely runs
 * into — THE WRITE PATH REACHES IT ON ITS OWN, in two ordinary edits, so state it that way rather
 * than as "a value that happens to look like a literal":
 *
 *   1. a STRING field holding `Player One`; the human types `42` -> the rendered value does not
 *      parse -> staged as the JSON string `"42"`. Correct.
 *   2. read-your-writes re-reads the model, `render_value` prints that string BARE, and
 *      `#syncAttributes` copies the new `value="42"` onto the reused element.
 *   3. the human edits the SAME field again -> `42` now parses -> the next edit is passed through
 *      BARE, and `stage_edit` type-checks nothing, so the field's stored type flips to number.
 *
 * `render_value` is lossy for exactly those values, and NOTHING in the mounted DOM carries the
 * field's widget kind — `role_for` collapses text/number/json onto `textbox` and no `type` is
 * emitted. The fix is a kind discriminator in the markup (a `type="number"`, or a `data-kind`),
 * which is a uitree/panel-model contract change — out of this renderer-only task's scope. FILED as
 * #434, and the current, known-wrong behaviour is PINNED by a characterization case in
 * `hydration.test.ts` so the fix flips a RED test instead of changing behaviour silently.
 */
export function commandValueFor(control: HTMLInputElement | HTMLTextAreaElement): string {
    if (control instanceof HTMLInputElement && control.type === "checkbox") {
        return control.checked ? "true" : "false";
    }
    // `defaultValue` IS the `value` content attribute for an `<input>` (and `""` when the attribute is
    // absent — `render_html` omits `value=` for empty text, and an empty field is an empty STRING
    // field, which is the right answer). Preferred over `getAttribute("value")` because a
    // `<textarea>`, which `valueControl` admits, carries NO `value` attribute at all — its default
    // value is its child text — so the attribute read would be permanently `null` there and every
    // multi-line edit would be quoted into a string. Identical behaviour for the `<input>` shapes
    // reachable today.
    const rendered = control.defaultValue;
    return parsesAsJson(rendered) ? control.value : JSON.stringify(control.value);
}

/**
 * What a hydrated panel reports about itself.
 *
 * NO READER EXISTS YET — not for want of a tier. The TypeScript test tier DOES exist (`webui-ts-unit`;
 * this file's own `hydration.test.ts` runs in it), but its cases assert the WIRE — the calls the mock
 * Shell recorded — which is the stronger surface, so these counters stay unread; and the live smoke
 * asserts the C++-side counters (`PanelHost::lists_served` / `renders_served`) instead. Recorded
 * honestly rather than described as "the surface a test reads", because the distinction matters:
 * these counters are the read surface a TS test harness WOULD use, and `reused` in particular is
 * the only way to observe that the patcher patched incrementally rather than full-replacing —
 * something no pixel assertion can distinguish.
 */
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
    /**
     * WHICH COPY of the panel kind this runtime is bound to (editor-UX c3, design 04 §3).
     *
     * Optional, and its absence means "the kind's one copy" — every pre-c3 caller keeps meaning what
     * it meant, and the Shell's own "no id ⇒ the default instance" path answers it. `PanelHost`
     * always supplies one.
     *
     * ⚠ IT IS ALSO THE DOM-ID SCOPE, which is the half a reader is likeliest to miss. `#normalise`
     * rewrites every model node id to `<scope>::<nodeId>` so two panels cannot both own `id="root"`;
     * with two copies of ONE kind mounted in one document, a scope of the PANEL id makes that
     * collision come back — duplicate ids across the two slots, which is invalid HTML and actively
     * breaks assistive technology, since it resolves references by id. Scoping by INSTANCE is what
     * keeps the document valid once a kind can exist twice.
     */
    readonly instanceId?: string;
    /**
     * The panel's roster title (`manifest.title`), used ONLY to detect and visually hide a duplicated
     * heading (a4, editor-UX `07-ui-states.md` §5): eight of the nine C++ panel models emit a
     * `Role::heading` as the first child of the panel root with text equal to their roster title, and
     * Dockview separately renders the SAME title into the tab — so it prints twice. When the panel's
     * mounted root's first child is a heading whose rendered text equals this string, `#normalise`
     * marks it `VISUALLY_HIDDEN_CLASS` (present in the accessibility tree, absent from the picture).
     *
     * Absent or empty means no heading is ever hidden — never guessed from the DOM. Matched on
     * RENDERED TEXT, never on a node id: ids differ per panel and a package panel will have its own,
     * so keying on one would be the special-casing `kit.css`'s header forbids.
     */
    readonly panelTitle?: string;
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
    /**
     * The copy this runtime ADDRESSES ON THE WIRE — `""` when the host bound none.
     *
     * ⚠ EMPTY IS NOT THE SAME AS `#idScope` BELOW, and keeping them apart is what makes this change
     * additive. An unscoped runtime sends NO `instanceId` member at all (`PanelClient` omits the key
     * for `""`), so a valueless dispatch stays byte-identical to the pre-c3 wire and the Shell's "no
     * id ⇒ the default copy" path answers it — while the DOM still needs a non-empty scope, which is
     * the panel id, exactly as it was.
     */
    readonly #instanceId: string;
    /** The DOM-id scope (see `HydrationOptions.instanceId`). Never empty. */
    readonly #idScope: string;
    readonly #gesturesEnabled: boolean;
    readonly #onDispatched: ((revision: number) => void) | undefined;
    /** See `HydrationOptions.panelTitle`. `""` means "never hide a heading". */
    readonly #panelTitle: string;

    #focusOrder: readonly string[] = [];
    #commands: ReadonlySet<string> = new Set();
    #revision = -1;
    #gestureActive = false;
    #disposed = false;
    /** Monotonic refresh ticket — see `refresh`'s out-of-order guard. */
    #generation = 0;
    /**
     * The node a gesture STARTED on, held for its whole lifetime.
     *
     * Every verb after `begin` reports this node rather than whatever is under the pointer now: a
     * drag that crosses onto a sibling must not silently re-target the gesture mid-stroke, and a
     * release outside the container must still be able to name the node it began on.
     */
    #gestureNode: Element | null = null;

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
    // THE VALUE CHANNEL (M9 e09e-1, design 05 §8). `input` STAGES what the human has entered;
    // `change` — the browser's own "the value is committed now", i.e. Enter or blur on a text field
    // and the toggle itself on a checkbox — is the L-20 GESTURE END. Two events, because those are
    // exactly the two moments the model distinguishes (`stage_edit` then `commit`).
    readonly #onInput = (event: Event): void => {
        void this.#stageFrom(event.target);
    };
    readonly #onChange = (event: Event): void => {
        void this.#commitFrom(event.target);
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
        this.#instanceId = options.instanceId ?? "";
        // The SCOPE falls back to the panel id: an unscoped runtime is the single-copy world this
        // class shipped in, and the panel id is exactly the scope it used then.
        this.#idScope = this.#instanceId === "" ? panelId : this.#instanceId;
        this.#gesturesEnabled = options.gestures;
        this.#onDispatched = options.onDispatched;
        this.#panelTitle = options.panelTitle ?? "";
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
        // GENERATION GUARD against an out-of-order response. Refreshes are concurrent by
        // construction — a dispatch-triggered refresh can overlap an onShow or a start-time one —
        // and the bridge does not promise completion order. Without this, an older response
        // resolving last would patch the DOM BACKWARDS and rewind `#revision`, which then makes the
        // newer revision look already-applied so nothing heals it until the next user action.
        // Revision comparison alone cannot substitute: `apply` is also called directly.
        const generation = ++this.#generation;
        const render = await this.#client.render(this.#panelId, this.#instanceId);
        if (render === null || generation !== this.#generation || this.#disposed) {
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
            // ONE O(n) index of the PRE-PATCH mounted tree, built up front (M9 e05d3 inherited perf
            // fix): the patcher previously ran a container-wide `querySelector` per incoming node —
            // O(n) × n nodes = O(n²), quadratic in exactly the panel the Scene tree makes big.
            // First occurrence wins, mirroring querySelector's first-match rule; elements IMPORTED
            // during the patch are deliberately absent (a duplicate model id can therefore never
            // re-claim a just-inserted element — strictly safer than the live lookup, see
            // #patchElement's reuse-hazard note).
            //
            // ⚠ `.content`, NOT the template ELEMENT (M9 e09e-3, and it was a real defect for as long
            // as this method has existed). `#parse` returns an `HTMLTemplateElement`, and a template's
            // parsed markup lives in its `content` DocumentFragment — its own `children` is ALWAYS
            // empty. Passing the element gave `#patchElement` an EMPTY source: every incoming node
            // loop body was skipped and its trailing-removal loop then deleted every child of the
            // panel body, so a patch WIPED the panel instead of updating it. It looked survivable
            // because the wipe leaves `childElementCount === 0`, so the NEXT render re-mounts through
            // `#adoptAll` — a panel whose model changes twice in a row therefore healed itself and
            // only a panel that re-renders ONCE and is not refreshed again stayed blank. Nothing
            // caught it: `#adoptAll` (which does use `.content`) covers the first render, every unit
            // test here applies a single revision, and the C++ suites drive the Shell side of the
            // seam, never this runtime. `editor-cef-smoke-shell-inspector-fanout` is what found it —
            // its window B staged one DOM edit, the resulting refresh emptied the Inspector, and with
            // no field left to type into nothing could ever refresh it again.
            this.#patchElement(this.#container, incoming.content, this.#indexMounted());
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
        this.#container.removeEventListener("input", this.#onInput);
        this.#container.removeEventListener("change", this.#onChange);
        this.#container.replaceChildren();
    }

    // --------------------------------------------------------------------------- interaction

    #bindInteractions(): void {
        // DELEGATED at the container, not per node: the tree is re-rendered constantly, and
        // per-node listeners would have to be re-attached on every patch — the exact bookkeeping
        // that goes wrong and leaves a panel half-live after a re-render.
        this.#container.addEventListener("click", this.#onClick);
        // key-handler-ok: Enter/Space ARIA activation of a focusable node, delegated at the panel body
        // — it dispatches the node's BOUND command (04 §2 / #handleKey), never a global chord bypassing
        // the keymap/command path (05 §6). The `webui-no-raw-key-handlers` T1 lint requires this marker.
        this.#container.addEventListener("keydown", this.#onKeyDown);
        // BOUND UNCONDITIONALLY, unlike the pointer gestures below. Staging is an ordinary COMMAND
        // (`inspector.edit`), which any panel declaring one may receive; only the commit half is a
        // gesture verb, and `#commit` is what gates on the manifest's `gestures` capability. Both
        // events bubble from the controls, so this stays one delegated pair like the rest.
        this.#container.addEventListener("input", this.#onInput);
        this.#container.addEventListener("change", this.#onChange);
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
        const control = valueControl(event.target);

        // ENTER IN A TEXT FIELD IS THE GESTURE END (M9 e09e-1, design 05 §8's "gesture end ->
        // commit"). Sent EXPLICITLY rather than left to the browser's implicit change-on-Enter, so
        // the commit is deterministic instead of resting on an engine behaviour this tier cannot
        // assert (an untrusted key event triggers no default text-control action, so no test here
        // could ever prove it). A `change` that follows anyway is harmless ON EVERY RESOLVED
        // OUTCOME: `InspectorPanel::commit` consumes the staged gesture for applied / rebased /
        // dropped, so the second commit finds nothing staged, reports `dispatched:false` and writes
        // nothing. NOT unconditional, and the exception is deliberate — on `Status::error` the panel
        // KEEPS the staged gesture "for the caller to fix", so a trailing `change` re-attempts that
        // same failed write. A retry of a refused write, not a double write; both paths still
        // coexist, but the property is narrower than "by construction".
        if (control !== null && isTextEntry(control) && event.key === "Enter") {
            const node = this.#nodeFrom(control);
            if (node !== null && node.hasAttribute(COMMAND_ATTRIBUTE)) {
                event.preventDefault();
                void this.#commit(node);
            }
            return;
        }

        // TEXT ENTRY IS NEVER HIJACKED. `role_html_tag` maps the `textbox` role onto a real
        // `<input>`, so text-entry nodes are ordinary focusables inside this container. Without
        // this bail-out, Home/End/Arrow would move PANEL focus and `preventDefault()` the caret,
        // and Space would dispatch a bound command instead of typing a space.
        if (isTextEntry(event.target)) {
            return;
        }

        // ACTIVATION: Enter and Space, the ARIA-standard pair. Together with the click handler this
        // is what makes every command-bound affordance reachable without a pointer — R-CLI-001
        // CLI-completeness as a structural accessibility property, which `uitree::audit_a11y`
        // already asserts on the model side.
        if (event.key === "Enter" || event.key === " ") {
            // A NON-TEXT VALUE CONTROL (a checkbox) ACTIVATES THROUGH ITS OWN `change`, so its keys
            // must reach the browser: `preventDefault()` here would stop Space from toggling it at
            // all, and the dispatch below would send a command with no value the model can only
            // decline. Arrow/Home/End navigation below still applies to it — the caret argument that
            // bails text entry out entirely does not.
            if (control !== null) {
                return;
            }
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
            //
            // Cancelled against the GESTURE'S OWN node, not the focused one: a pointer-initiated
            // drag usually leaves focus untouched, so keying off focus meant the common case
            // resolved to "" — which cleared the local flag WITHOUT ever telling the Shell, leaving
            // it holding an open gesture the renderer believed it had cancelled.
            const nodeId = this.#gestureNode?.getAttribute(NODE_ID_ATTRIBUTE)
                ?? this.#focusedNodeId()
                ?? "";
            this.#gestureActive = false;
            this.#gestureNode = null;
            event.preventDefault();
            void this.#sendGesture("cancel", nodeId, 0, 0);
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
        // ADVANCE PAST UNREACHABLE ENTRIES. `focus_order` may name an id that `render_html` did not
        // mark focusable, or that a patch removed. Stopping at the first failure would strand the
        // user pressing an arrow key that never moves focus (and, un-prevented, scrolls the panel
        // instead), so keep walking in the direction of travel until one lands.
        const step = event.key === "End" ? -1 : event.key === "Home" ? 1 : delta;
        for (let i = next; i >= 0 && i < this.#focusOrder.length; i += step) {
            const target = this.#focusOrder[i];
            if (target !== undefined && this.focusNode(target)) {
                event.preventDefault();
                return;
            }
        }
    }

    async #activateFrom(target: EventTarget | null): Promise<void> {
        // A VALUE CONTROL IS NOT ACTIVATED BY ITS CLICK (M9 e09e-1). Its edit ALREADY travels on
        // `input`/`change`, so dispatching from here as well would send the same command TWICE for
        // one interaction. That is the whole reason, and it is exactly what PLANT (4) reds on — a
        // second `panel.command`, not a wrong value. The command still reaches the model; it just
        // reaches it from the one channel that carries the value.
        //
        // Deliberately NOT justified by a claim about WHEN the browser toggles. A checkbox's
        // pre-click behaviour and its activation behaviour run at different points of event
        // dispatch, and nothing in this tier pins which value a click handler would observe — the
        // duplicate dispatch is reason enough, and it is the half a test here can actually prove.
        if (valueControl(target) !== null) {
            return;
        }
        const node = this.#nodeFrom(target);
        if (node !== null && node.hasAttribute(COMMAND_ATTRIBUTE)) {
            await this.#activate(node);
        }
    }

    /** STAGE a control's current value on the model — 05 §8's `DOM input -> "inspector.edit"`. */
    async #stageFrom(target: EventTarget | null): Promise<void> {
        const control = valueControl(target);
        if (control === null) {
            return;
        }
        const node = this.#nodeFrom(control);
        if (node !== null && node.hasAttribute(COMMAND_ATTRIBUTE)) {
            await this.#activate(node, commandValueFor(control));
        }
    }

    /** END the gesture on a control whose value the browser just committed (Enter, blur, a toggle). */
    async #commitFrom(target: EventTarget | null): Promise<void> {
        const control = valueControl(target);
        if (control === null) {
            return;
        }
        const node = this.#nodeFrom(control);
        if (node !== null && node.hasAttribute(COMMAND_ATTRIBUTE)) {
            await this.#commit(node);
        }
    }

    /**
     * Send the L-20 gesture END for a value control.
     *
     * GATED ON THE MANIFEST'S `gestures`, exactly like the pointer handlers: a panel whose provider
     * bound no write gateway reports `gestures:false` and the Shell REFUSES every verb, so sending
     * one would only manufacture a refusal. Staging stays available there — it is a declared command,
     * and whether an unbindable panel wants one is the panel's decision, not this runtime's.
     *
     * DELIBERATELY OUTSIDE THE POINTER GESTURE STATE MACHINE: it touches neither `#gestureActive` nor
     * `#gestureNode`, because a value edit's gesture is opened by `stage_edit` on the MODEL, not by a
     * `begin` verb from here — there is no local flag to own. The consequence to know is that a
     * commit sent while a POINTER gesture happens to be open names this field's node while the
     * runtime still believes that other gesture is in flight. Unreachable today (an Inspector field
     * is not a drag target), and written down because this file's state reasoning is otherwise total.
     */
    async #commit(node: Element): Promise<void> {
        if (!this.#gesturesEnabled) {
            return;
        }
        await this.#sendGesture("commit", node.getAttribute(NODE_ID_ATTRIBUTE) ?? "", 0, 0);
    }

    async #activate(node: Element, value?: string): Promise<void> {
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
        // CAUGHT HERE because every caller reaches this through `void` (a DOM event handler cannot
        // await). `PanelClient` swallows only `BridgeError` and RETHROWS anything else, so without
        // this an unexpected transport throw becomes an unhandled promise rejection in a renderer
        // that has no console to report it — the runtime would go quiet with no diagnosis.
        try {
            const result = await this.#client.command(
                this.#panelId,
                commandId,
                nodeId,
                value,
                this.#instanceId,
            );
            // Counted only when the command ACTUALLY dispatched: a refusal or a transport failure
            // is not a dispatch, and inflating the counter would misreport what the panel did.
            if (result !== null && result.dispatched) {
                this.#dispatches += 1;
                this.#onDispatched?.(result.revision);
            }
        } catch {
            // A failed dispatch is not fatal to the panel: the next render reconciles it.
        }
    }

    // ------------------------------------------------------------------------------ gestures

    #handlePointer(event: PointerEvent, verb: GestureVerb): void {
        // THE ORIGIN NODE OWNS THE WHOLE GESTURE. Only `begin` resolves a node from the event
        // target; `extend`/`commit`/`cancel` reuse it. Re-deriving per event had two failure modes:
        // a drag onto a sibling silently re-targeted the gesture (and reported coordinates relative
        // to the WRONG node), and a pointer released where `#nodeFrom` yields null returned early —
        // before `#gestureActive` was cleared — wedging the flag `true` forever, so `commit` never
        // reached the Shell and every later `begin` was swallowed.
        let node: Element | null;
        if (verb === "begin") {
            if (this.#gestureActive) {
                return;
            }
            node = this.#nodeFrom(event.target);
            if (node === null || (node.getAttribute(NODE_ID_ATTRIBUTE) ?? "") === "") {
                return;
            }
            this.#gestureActive = true;
            this.#gestureNode = node;
            // Capture routes every later pointer event here even once the pointer leaves the
            // container, so a release outside it still commits instead of stranding the gesture.
            try {
                node.setPointerCapture(event.pointerId);
            } catch {
                // A capture refusal (a stale pointer id) is not a reason to drop the gesture.
            }
        } else {
            node = this.#gestureNode;
            if (node === null) {
                // No gesture in flight — a stray move/up. Keep the flag honest and drop it.
                this.#gestureActive = false;
                return;
            }
            if (verb === "commit" || verb === "cancel") {
                this.#gestureActive = false;
                this.#gestureNode = null;
            }
        }
        // Non-empty on both paths by construction: `begin` rejected an empty id above, and the
        // continuation path reads it off the node `begin` accepted (nothing strips `data-node-id`).
        const nodeId = node.getAttribute(NODE_ID_ATTRIBUTE) ?? "";
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
            this.#gestureNode = null;
            return;
        }
        // Caught for the same reason as `#activate` — every caller reaches this through `void`.
        try {
            const result = await this.#client.gesture(
                this.#panelId,
                verb,
                { nodeId, x, y },
                this.#instanceId,
            );
            this.#gestures += 1;
            if (result !== null && result.dispatched) {
                this.#onDispatched?.(result.revision);
            }
        } catch {
            // A gesture that could not be delivered must not leave the local flag stuck true.
            if (verb === "begin") {
                this.#gestureActive = false;
                this.#gestureNode = null;
            }
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
        this.#hideDuplicateHeading(template.content);
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
            element.setAttribute("id", `${this.#idScope}::${nodeId}`);
            const role = element.getAttribute("role") ?? "";
            // Through the kit's own accessor rather than by indexing the map: it is the surface the
            // kit publishes, and routing the runtime's only lookup through it keeps that surface
            // exercised by every hydration test instead of being an export nothing calls.
            const widgetClass = widgetClassForRole(role);
            if (widgetClass !== undefined) {
                element.classList.add(widgetClass);
            }
        }
    }

    /**
     * Hide a panel's OWN duplicated title (a4, `07-ui-states.md` §5, `HydrationOptions.panelTitle`).
     *
     * Eight of the nine C++ panel models emit a `Role::heading` as the FIRST CHILD of the panel root
     * with text equal to their roster title, and Dockview separately renders the same title into the
     * tab — so the title prints twice. When that shape is found, the heading is marked
     * `VISUALLY_HIDDEN_CLASS`: present in the accessibility tree (the model still owns an `aria-label`
     * — `role_requires_name(Role::heading)` is true on the C++ side), absent from the picture.
     *
     * STRUCTURAL, not panel-specific, exactly like the rest of this file (see the header): the match
     * is "panel root's first child is a heading whose TEXT equals `#panelTitle`", never a node id.
     * Tilemap Painter's heading carries a label but no text (`tilemap_paint_panel.cpp`), so its text
     * never equals the title and this correctly leaves it alone; a heading that is not the panel's
     * first child, or whose text differs, is likewise correctly left visible.
     *
     * Runs on EVERY `#parse` — a full mount and a patched re-render both carry the panel's complete
     * markup (`render.html` is never a partial diff) — so a reused element's `class` attribute stays
     * consistent across renders via `#syncAttributes` (`class` is a SYNCED_ATTRIBUTE).
     */
    #hideDuplicateHeading(root: ParentNode): void {
        if (this.#panelTitle === "") {
            return;
        }
        const panelRoot = root.firstElementChild;
        const firstChild = panelRoot?.firstElementChild ?? null;
        if (
            firstChild !== null &&
            firstChild.getAttribute("role") === "heading" &&
            (firstChild.textContent ?? "") === this.#panelTitle
        ) {
            firstChild.classList.add(VISUALLY_HIDDEN_CLASS);
        }
    }

    #adoptAll(template: HTMLTemplateElement): void {
        this.#container.replaceChildren(document.importNode(template.content, true));
    }

    /**
     * Index every mounted element carrying a model node id, in document order, first occurrence
     * wins. Built ONCE per patch pass — one O(n) traversal replacing the per-incoming-node
     * container-wide `querySelector` that made the patch O(n²) (the Scene tree is the panel whose
     * node count makes that bite). A snapshot of the PRE-PATCH tree by construction, which is
     * exactly the reuse-candidate set: elements imported mid-patch must never be re-claimed.
     */
    #indexMounted(): Map<string, HTMLElement> {
        const index = new Map<string, HTMLElement>();
        for (const element of this.#container.querySelectorAll<HTMLElement>(
            `[${NODE_ID_ATTRIBUTE}]`,
        )) {
            const nodeId = element.getAttribute(NODE_ID_ATTRIBUTE) ?? "";
            if (nodeId !== "" && !index.has(nodeId)) {
                index.set(nodeId, element);
            }
        }
        return index;
    }

    /**
     * Incremental DOM patch, keyed by stable node ids (04 §4 step 3).
     *
     * Reuses an existing element when the SAME node id reappears with the same tag, syncing only the
     * attributes and own text that changed, and adopts a fresh subtree otherwise. Reuse is what
     * preserves scroll position, focus, text selection and CSS transition state across a re-render —
     * a full replace loses all four, which in a live editor reads as the panel "flickering" every
     * time an unrelated diagnostic arrives.
     *
     * `mounted` is the ONE pre-built id -> element snapshot from `#indexMounted` (O(1) lookups, so
     * the whole patch is O(n)); `claimed` marks what this pass already reused.
     */
    #patchElement(target: Element | DocumentFragment, source: ParentNode,
                  mounted: Map<string, HTMLElement>, claimed: Set<Element> = new Set()): void {
        const sourceChildren = Array.from(source.children);
        for (let index = 0; index < sourceChildren.length; index += 1) {
            const incoming = sourceChildren[index];
            if (incoming === undefined) {
                continue;
            }
            const nodeId = incoming.getAttribute(NODE_ID_ATTRIBUTE) ?? "";
            const candidate = nodeId === "" ? null : (mounted.get(nodeId) ?? null);
            const anchor = target.children[index] ?? null;

            // TWO REUSE HAZARDS the lookup must survive:
            //
            //  * DUPLICATE IDS. The index keeps the FIRST match (querySelector's rule), so a model
            //    that emitted the same id twice (an `audit_a11y` violation, but `render` does not
            //    run the audit — a buggy provider ships it) would have the second node reuse the
            //    SAME element, MOVING it instead of adding a sibling. `claimed` makes each element
            //    reusable at most once per pass. (Since the index snapshots the PRE-patch tree,
            //    a just-imported element can never be a candidate at all — the live-query variant
            //    of this hazard is structurally gone.)
            //  * ANCESTOR REUSE. The match may be an ANCESTOR of `target`, and re-parenting a node
            //    under its own descendant throws `HierarchyRequestError` — which escapes `apply`
            //    mid-patch, leaves the DOM half-updated with `#revision` unadvanced, and replays on
            //    every later refresh. A re-parent with stable ids (a scene tree — e05d3's shape) is
            //    exactly how that arises.
            //
            // Either way we fall through to `importNode`, which is always correct, just not reusing.
            const existing = candidate !== null
                && !claimed.has(candidate)
                && !candidate.contains(target instanceof Element ? target : null)
                ? candidate
                : null;

            if (existing !== null && existing.tagName === incoming.tagName) {
                claimed.add(existing);
                this.#syncAttributes(existing, incoming);
                this.#syncOwnText(existing, incoming);
                this.#patchElement(existing, incoming, mounted, claimed);
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
