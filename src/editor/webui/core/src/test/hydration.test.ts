// T1 for the HYDRATION RUNTIME's VALUE CHANNEL (M9 e09e-1, design 05 §8 / 04 §4) — the DOM half of
// the canonical write chain:
//
//   DOM input -> hydration -> command "inspector.edit" (WITH its value) -> stage_edit
//              -> gesture end -> commit -> RPC edit
//
// WHY THIS TIER, AND WHAT IT CAN PROVE. The first three links are pure browser behaviour — a real
// `<input>`, a real `input`/`change` event, real bubbling to a delegated listener, the real
// `PanelClient` marshalling real JSON onto a real `ShellBridge`. All of that runs here, in the real
// browser the `webui-ts-unit` tier already provisions, in milliseconds. What it deliberately does NOT
// claim is the C++ half: the Shell below is a MOCK, written to mirror `inspector_feed.cpp`'s
// `make_provider()` decisions (a dispatch with no parseable `value` is DECLINED; `commit` with
// nothing staged reports `dispatched:false`). The live pair is `editor-cef-smoke-shell`'s to prove,
// and the C++ side of the same seam is already pinned by `test_e09b_concurrent_cas.cpp`, which drives
// `panel.command inspector.edit` -> `panel.gesture commit` against a REAL daemon and real bytes.
//
// THE PROPERTY THAT MADE THIS FILE NECESSARY. Before e09e-1, `PanelClient.command` posted only
// `{panelId, commandId, nodeId}` and nothing in the runtime listened to `input`/`change` at all — so
// a human editing an Inspector field could not reach `inspector.edit` in any way, and every layer
// below it (the L-20 staging, the L-30 rebase-or-drop engine, the wire write) sat unreachable behind
// a link that was never built. Every case here is written so that removing one of the two halves
// makes it RED.
//
// ⚠ NON-VACUITY PROVEN BY PLANTING, each plant reverted byte-exact. Recorded inline as
// `⚠ PLANT (n)` against the case that catches it — every plant has a marker, so a case with none is
// a gap, not a plant that was merely not written down.

import { assert, assertEqual, delay, waitFor, type TestCase } from "./harness.js";
import { ShellBridge, type BridgeQuery, type BridgeQueryFunction } from "../bridge.js";
import { commandValueFor, HydrationRuntime } from "../hydration.js";
import {
    PANEL_COMMAND_METHOD,
    PANEL_GESTURE_METHOD,
    PanelClient,
    type PanelRender,
} from "../panels.js";

// --------------------------------------------------------------------------------- the mock Shell

const PANEL_ID = "builtin.inspector";
const EDIT_COMMAND = "inspector.edit";
/** Mirrors `panels::kInspectorWidgetPrefix` — the node-id -> field-pointer mapping's one dependency. */
const WIDGET_PREFIX = "inspector.widget.";

interface RecordedCall {
    readonly method: string;
    readonly params: Record<string, unknown>;
    readonly dispatched: boolean;
}

interface MockShell {
    readonly bridge: ShellBridge;
    readonly calls: readonly RecordedCall[];
    /** Every value the mock model has COMMITTED, in order — the "real bytes would have moved" record. */
    readonly writes: readonly string[];
    /** The currently staged edit, mirroring `InspectorPanel::staged_`. */
    readonly staged: string | null;
    /** Calls to one method, in order. */
    of(method: string): readonly RecordedCall[];
}

/**
 * DELIBERATELY A SECOND COPY of `hydration.ts`'s helper rather than an import of it — do not dedupe.
 *
 * This one stands in for the C++ `serializer::parse_json` INSIDE the mock Shell: it belongs to the
 * code under test's COUNTERPARTY, not to the code under test. Importing the renderer's own helper
 * would make the model's accept/refuse decision derive from the very function the encoder consults,
 * so a mutation to it would move both sides together and the encoder cases below would quietly stop
 * discriminating — the duplication is what keeps the two halves independently falsifiable.
 */
function parsesAsJson(text: string): boolean {
    try {
        JSON.parse(text);
        return true;
    } catch {
        return false;
    }
}

/**
 * A Shell that decides `panel.command` / `panel.gesture` the way `inspector_feed.cpp` does.
 *
 * FAITHFUL ON THE THREE DECISIONS THAT MATTER HERE, each traceable to a line of C++:
 *   * `invoke` refuses a command that is not `inspector.edit`, or a node that is not an inspector
 *     widget (`inspector_widget_pointer` -> nullopt).
 *   * `invoke` refuses a dispatch whose `value` does not parse as JSON — `contract::Json::at` is
 *     TOTAL, so a MISSING key arrives as the empty string and `parse_json("")` fails. That is the
 *     exact behaviour the pre-e09e-1 renderer hit on every edit.
 *   * `gesture commit` with nothing staged is `Status::none` -> `dispatched:false`, and a resolved
 *     commit CONSUMES the staged edit (so a second commit writes nothing).
 *
 * ⚠ AND DELIBERATELY NOT FAITHFUL BEYOND THEM — read `dispatched:true` here as "the wire carried
 * something the provider could parse", never as "the model would accept this edit". The real chain
 * has a FOURTH gate this mock does not model: `inspector_feed.cpp` hands off to
 * `InspectorPanel::stage_edit`, which additionally refuses an unknown pointer, a non-`editable`
 * field and a `readonly` kind ("stage_edit's own field/editable checks still apply to the rest", as
 * that file puts it). There is no field roster and no editable bit here, so any well-prefixed node
 * id with a parseable value is accepted. Nor can it produce the provider's ERROR-shaped refusals —
 * `panel.unknown_command`, `panel.bad_gesture`, `panel.bad_params` are BridgeErrors, not
 * `dispatched:false`, so `PanelClient`'s error->`null` path is not exercised from here.
 */
function mockShell(): MockShell {
    const calls: RecordedCall[] = [];
    const writes: string[] = [];
    let staged: string | null = null;

    const isWidget = (params: Record<string, unknown>): boolean => {
        const nodeId = params["nodeId"];
        return typeof nodeId === "string" && nodeId.startsWith(WIDGET_PREFIX);
    };

    const query: BridgeQueryFunction = (request: BridgeQuery): number => {
        const parsed = JSON.parse(request.request) as {
            id: number;
            method: string;
            params: Record<string, unknown>;
        };
        const params = parsed.params;
        let dispatched = false;
        if (parsed.method === PANEL_COMMAND_METHOD) {
            const value = params["value"];
            dispatched =
                params["commandId"] === EDIT_COMMAND &&
                isWidget(params) &&
                typeof value === "string" &&
                parsesAsJson(value);
            if (dispatched) {
                staged = params["value"] as string;
            }
        } else if (parsed.method === PANEL_GESTURE_METHOD) {
            const verb = params["verb"];
            if (verb === "commit") {
                dispatched = staged !== null;
                if (staged !== null) {
                    writes.push(staged);
                    staged = null;
                }
            } else if (verb === "cancel") {
                dispatched = staged !== null;
                staged = null;
            } else {
                dispatched = isWidget(params);
            }
        }
        calls.push({ method: parsed.method, params, dispatched });
        request.onSuccess(
            JSON.stringify({
                jsonrpc: "2.0",
                id: parsed.id,
                result: { dispatched, revision: 1 },
            }),
        );
        return parsed.id;
    };

    return {
        bridge: new ShellBridge(query),
        calls,
        writes,
        get staged(): string | null {
            return staged;
        },
        of(method: string): readonly RecordedCall[] {
            return calls.filter((call) => call.method === method);
        },
    };
}

// ------------------------------------------------------------------------------- the mounted panel

interface Mounted {
    readonly shell: MockShell;
    readonly runtime: HydrationRuntime;
    readonly container: HTMLElement;
    /** A mounted node by its MODEL id (what `data-node-id` carries). */
    node(nodeId: string): HTMLElement;
    input(nodeId: string): HTMLInputElement;
    dispose(): void;
}

/**
 * The markup below is what `uitree::render_html` actually emits for an Inspector field row —
 * checked against BOTH halves, since a fixture that drifts from the renderer silently tests a shape
 * production never mounts. `InspectorPanel::build_panel` emits each row as `Role::group` (NOT a
 * listitem) inside a `Role::list`, with a `Role::text` label child; `role_html_tag` then maps those
 * to `<div>` / `<ul>` / `<span>`, and a `textbox` role to a VOID `<input>` whose text rides the
 * `value` attribute, a `checkbox` additionally carrying `type="checkbox"` and `checked` presence.
 * Hand-written here rather than fetched, because the C++ renderer's own output is pinned on the C++
 * side (`test_node.cpp`) and duplicating that assertion here would test the fixture, not the runtime.
 */
const FIELDS_HTML = [
    '<section id="inspector.panel" role="region" aria-label="Inspector">',
    '<ul id="inspector.fields" role="list" aria-label="Component fields">',
    // a STRING field: `render_value` prints a string's characters BARE
    '<div id="inspector.field./name" role="group">',
    '<span id="inspector.label./name" role="text">/name</span>',
    `<input id="${WIDGET_PREFIX}/name" role="textbox" aria-label="/name" tabindex="0"`,
    ' data-command="inspector.edit" value="Player One">',
    "</div>",
    // a NUMBER field: its rendered value IS a JSON literal
    '<div id="inspector.field./speed" role="group">',
    `<input id="${WIDGET_PREFIX}/speed" role="textbox" aria-label="/speed" tabindex="0"`,
    ' data-command="inspector.edit" value="1.5">',
    "</div>",
    // an EMPTY string field: `render_html` omits `value=` entirely for empty text
    '<div id="inspector.field./notes" role="group">',
    `<input id="${WIDGET_PREFIX}/notes" role="textbox" aria-label="/notes" tabindex="0"`,
    ' data-command="inspector.edit">',
    "</div>",
    // a STRING field whose rendered value LOOKS like a JSON literal — the #434 case. `render_value`
    // is lossy here, so the encoder cannot tell it from a number field. Pinned, not fixed.
    '<div id="inspector.field./tag" role="group">',
    `<input id="${WIDGET_PREFIX}/tag" role="textbox" aria-label="/tag" tabindex="0"`,
    ' data-command="inspector.edit" value="42">',
    "</div>",
    // a TOGGLE field
    '<div id="inspector.field./visible" role="group">',
    `<input id="${WIDGET_PREFIX}/visible" role="checkbox" type="checkbox" aria-label="/visible"`,
    ' tabindex="0" data-command="inspector.edit" checked>',
    "</div>",
    // a plain, VALUELESS affordance — the "every other dispatch is unchanged" control case.
    // ⚠ SYNTHETIC: the real Inspector renders no button node. It stands in for every OTHER panel's
    // valueless affordance, which is what the byte-identity case below is actually about.
    '<div id="inspector.field./reset" role="group">',
    '<button id="inspector.action.reset" role="button" aria-label="Reset" tabindex="0"',
    ' data-command="inspector.edit">Reset</button>',
    "</div>",
    "</ul>",
    "</section>",
].join("");

/**
 * The SAME panel, one revision later: `/speed` moved, and a field APPEARED.
 *
 * Both halves matter to the patch case below. The moved value proves the incoming tree was actually
 * READ (an attribute sync happened); the new row proves an INSERT happened, which "nothing was
 * removed" alone cannot show.
 */
const PATCHED_HTML = FIELDS_HTML.replace(' value="1.5"', ' value="2.5"').replace(
    "</ul>",
    [
        '<div id="inspector.field./fresh" role="group">',
        `<input id="${WIDGET_PREFIX}/fresh" role="textbox" aria-label="/fresh" tabindex="0"`,
        ' data-command="inspector.edit" value="7">',
        "</div>",
        "</ul>",
    ].join(""),
);

function render(): PanelRender {
    return {
        panelId: PANEL_ID,
        revision: 1,
        html: FIELDS_HTML,
        focusOrder: [`${WIDGET_PREFIX}/name`, `${WIDGET_PREFIX}/speed`],
        // The runtime refuses to dispatch a command the render did not declare, so this is
        // load-bearing rather than decoration.
        commands: [{ id: EDIT_COMMAND, title: "Edit field" }],
    };
}

function mount(gestures = true): Mounted {
    const shell = mockShell();
    const container = document.createElement("div");
    document.body.appendChild(container);
    const runtime = new HydrationRuntime(container, new PanelClient(shell.bridge), PANEL_ID, {
        gestures,
    });
    runtime.apply(render());
    const node = (nodeId: string): HTMLElement => {
        const found = container.querySelector<HTMLElement>(`[data-node-id="${nodeId}"]`);
        assert(found !== null, `the fixture must mount a node ${nodeId}`);
        return found as HTMLElement;
    };
    return {
        shell,
        runtime,
        container,
        node,
        input(nodeId: string): HTMLInputElement {
            const found = node(nodeId);
            assert(found instanceof HTMLInputElement, `${nodeId} must mount as an <input>`);
            return found as HTMLInputElement;
        },
        dispose(): void {
            runtime.dispose();
            container.remove();
        },
    };
}

/**
 * Let every pending bridge turn settle, for the cases that assert an ABSENCE.
 *
 * `waitFor` cannot express those: its predicate is already true on entry, so it returns without ever
 * yielding and the assertion passes vacuously — proving nothing about a send that was merely still
 * queued. A real yield is the only way to give the send a chance to happen and then observe that it
 * did not.
 */
async function settle(): Promise<void> {
    await delay(25);
}

/** Type into a control the way a human does: the live value changes, the ATTRIBUTE does not. */
function type(control: HTMLInputElement, text: string): void {
    control.value = text;
    control.dispatchEvent(new Event("input", { bubbles: true }));
}

/** The browser's "this value is committed now" — Enter or blur on a text field, a toggle's own click. */
function commitValue(control: HTMLInputElement): void {
    control.dispatchEvent(new Event("change", { bubbles: true }));
}

function pressEnter(control: HTMLInputElement): boolean {
    const event = new KeyboardEvent("keydown", { key: "Enter", bubbles: true, cancelable: true });
    control.dispatchEvent(event);
    return event.defaultPrevented;
}

/** The `value` member of the LAST `panel.command` recorded, or `undefined` when it carried none. */
function lastCommandValue(shell: MockShell): unknown {
    const commands = shell.of(PANEL_COMMAND_METHOD);
    const last = commands[commands.length - 1];
    assert(last !== undefined, "expected at least one panel.command");
    return last?.params["value"];
}

export const hydrationTests: readonly TestCase[] = [
    {
        // ⚠ PLANT (1): drop `value` from `PanelClient.command`'s params -> the edit is REFUSED
        // (`dispatched:false`, nothing staged, nothing written) and this case reds. That is exactly
        // the state the shipping build was in before e09e-1.
        // ⚠ PLANT (2): remove the `change` listener -> the stage lands but no `commit` verb is ever
        // sent, `writes` stays empty and this case reds.
        name: "hydration: a typed edit reaches `inspector.edit` WITH its value, and `change` commits it",
        run: async () => {
            const panel = mount();
            try {
                const field = panel.input(`${WIDGET_PREFIX}/name`);
                type(field, "Player Two");
                await waitFor(
                    "the staged edit to reach the Shell",
                    () => panel.shell.of(PANEL_COMMAND_METHOD).length === 1,
                    5_000,
                    () => `calls=${JSON.stringify(panel.shell.calls)}`,
                );

                const staged = panel.shell.of(PANEL_COMMAND_METHOD)[0];
                assertEqual(staged?.params["panelId"], PANEL_ID, "the dispatch names its panel");
                assertEqual(staged?.params["commandId"], EDIT_COMMAND, "the bound command id");
                assertEqual(
                    staged?.params["nodeId"],
                    `${WIDGET_PREFIX}/name`,
                    "the widget node the field maps from",
                );
                assertEqual(
                    staged?.params["value"],
                    '"Player Two"',
                    "the value the human entered, as the JSON literal the C++ provider parses",
                );
                assert(staged?.dispatched === true, "the model must ACCEPT a valued edit");
                assertEqual(panel.shell.writes, [], "staging alone must write NOTHING (L-20)");

                commitValue(field);
                await waitFor(
                    "the gesture end to reach the Shell",
                    () => panel.shell.of(PANEL_GESTURE_METHOD).length === 1,
                    5_000,
                    () => `calls=${JSON.stringify(panel.shell.calls)}`,
                );
                const commit = panel.shell.of(PANEL_GESTURE_METHOD)[0];
                assertEqual(commit?.params["verb"], "commit", "the gesture END is what writes");
                assert(commit?.dispatched === true, "the commit must resolve the staged edit");
                assertEqual(
                    panel.shell.writes,
                    ['"Player Two"'],
                    "the committed write carries the human's value, once",
                );
                assertEqual(panel.shell.staged, null, "a resolved commit CONSUMES the staged edit");
            } finally {
                panel.dispose();
            }
        },
    },
    {
        // ⚠ PLANT (3): key `commandValueFor` off the USER's text (parse it, else quote it) instead of
        // the model's rendered value -> the `42` typed into the STRING field below is staged as a
        // NUMBER, and this case reds. THAT ORDERING MATTERS: the three obvious inputs (a string field
        // given prose, a number field given a number, an empty field given prose) agree under BOTH
        // rules, so a table without a disagreeing pair would pass the plant and prove nothing about
        // which signal the encoder actually reads. The two that disagree are here and in the
        // refusal case below — one for each direction of the mis-typing.
        name: "hydration: the value is typed from the MODEL's rendered value, not from the text typed",
        run: async () => {
            const panel = mount();
            try {
                // A STRING field: `render_value` printed it bare, so the edit is quoted + escaped.
                type(panel.input(`${WIDGET_PREFIX}/name`), 'a "quoted" name');
                await waitFor(
                    "the string field's edit",
                    () => panel.shell.of(PANEL_COMMAND_METHOD).length === 1,
                );
                assertEqual(
                    lastCommandValue(panel.shell),
                    '"a \\"quoted\\" name"',
                    "a string field's edit is a JSON STRING literal, escaped",
                );

                // A NUMBER field: its rendered value parses, so the human's literal passes through.
                type(panel.input(`${WIDGET_PREFIX}/speed`), "2.5");
                await waitFor(
                    "the number field's edit",
                    () => panel.shell.of(PANEL_COMMAND_METHOD).length === 2,
                );
                assertEqual(
                    lastCommandValue(panel.shell),
                    "2.5",
                    "a JSON-typed field's edit passes through UNQUOTED",
                );

                // An EMPTY string field (`render_html` omits `value=`): still a string.
                type(panel.input(`${WIDGET_PREFIX}/notes`), "now filled");
                await waitFor(
                    "the empty field's edit",
                    () => panel.shell.of(PANEL_COMMAND_METHOD).length === 3,
                );
                assertEqual(
                    lastCommandValue(panel.shell),
                    '"now filled"',
                    "an absent `value` attribute reads as an empty STRING field, not as JSON",
                );

                // THE DISAGREEING PAIR, direction one: a STRING field given text that is itself a
                // JSON literal. The field's type must survive — writing the bare `42` would silently
                // turn a string field into a number one on disk.
                type(panel.input(`${WIDGET_PREFIX}/name`), "42");
                await waitFor(
                    "the JSON-shaped text typed into a string field",
                    () => panel.shell.of(PANEL_COMMAND_METHOD).length === 4,
                );
                assertEqual(
                    lastCommandValue(panel.shell),
                    '"42"',
                    "a string field keeps its type even when the text reads as a literal",
                );

                // A CHECKBOX: its state IS a boolean literal, read off `checked`, not off `value`.
                const toggle = panel.input(`${WIDGET_PREFIX}/visible`);
                assertEqual(commandValueFor(toggle), "true", "a checked box encodes to `true`");
                toggle.checked = false;
                assertEqual(commandValueFor(toggle), "false", "an unchecked box encodes to `false`");
            } finally {
                panel.dispose();
            }
        },
    },
    {
        name: "hydration: an edit that cannot be typed to the field is REFUSED, never coerced",
        run: async () => {
            const panel = mount();
            try {
                // `/speed` is JSON-typed, so `abc` is passed through as written — and refused by the
                // model. The dangerous alternative (quoting it into a valid string literal) would
                // have silently changed the field's type on disk.
                const field = panel.input(`${WIDGET_PREFIX}/speed`);
                type(field, "abc");
                await waitFor(
                    "the refused edit",
                    () => panel.shell.of(PANEL_COMMAND_METHOD).length === 1,
                );
                assertEqual(lastCommandValue(panel.shell), "abc", "sent verbatim, not quoted");
                assert(
                    panel.shell.of(PANEL_COMMAND_METHOD)[0]?.dispatched === false,
                    "an unparseable value must be DECLINED by the model",
                );
                assertEqual(panel.shell.staged, null, "nothing may be staged from a refused edit");

                commitValue(field);
                await waitFor(
                    "the gesture end",
                    () => panel.shell.of(PANEL_GESTURE_METHOD).length === 1,
                );
                assert(
                    panel.shell.of(PANEL_GESTURE_METHOD)[0]?.dispatched === false,
                    "a commit with nothing staged is an ordinary `dispatched:false`",
                );
                assertEqual(panel.shell.writes, [], "and NOTHING is written");
            } finally {
                panel.dispose();
            }
        },
    },
    {
        // THE ONE CASE DRIVEN BY A REAL ACTIVATION rather than by synthesised value events: a click
        // dispatched at a checkbox runs the browser's own activation behaviour, so the toggle, the
        // `input` and the `change` below are all the PLATFORM's, in the platform's order — which is
        // also how this case measured that the activation runs AFTER the click finishes dispatching
        // (an earlier fixture toggled by hand FIRST and was double-toggled by the click).
        //
        // ⚠ PLANT (4): let `#activateFrom` dispatch for a value control as well -> the click's own
        // handler stages first, for the PRE-toggle state, and the activation's `input` stages again:
        // two `panel.command` calls instead of one, and this case reds.
        name: "hydration: a checkbox commits through its OWN activation, and its click stages nothing extra",
        run: async () => {
            const panel = mount();
            try {
                const toggle = panel.input(`${WIDGET_PREFIX}/visible`);
                assert(toggle.checked, "the fixture's toggle starts checked");
                toggle.dispatchEvent(new MouseEvent("click", { bubbles: true }));

                await waitFor(
                    "the toggle to stage and commit",
                    () => panel.shell.writes.length === 1,
                    5_000,
                    () => `calls=${JSON.stringify(panel.shell.calls)}`,
                );
                await settle();
                assertEqual(
                    panel.shell.writes,
                    ["false"],
                    "the state the activation left it in is what commits, once",
                );
                assertEqual(
                    panel.shell.of(PANEL_COMMAND_METHOD).length,
                    1,
                    "exactly ONE stage — the click must not dispatch a second one",
                );
                assertEqual(panel.shell.staged, null, "and nothing is left staged");
            } finally {
                panel.dispose();
            }
        },
    },
    {
        // ⚠ PLANT (5): drop the Enter branch from `#handleKey` -> Enter no longer commits (the tier
        // cannot trigger the browser's implicit change-on-Enter with an untrusted event), `writes`
        // stays empty and this case reds.
        name: "hydration: Enter is the gesture end, and a `change` after it writes nothing more",
        run: async () => {
            const panel = mount();
            try {
                const field = panel.input(`${WIDGET_PREFIX}/name`);
                type(field, "Enter Committed");
                await waitFor("the stage", () => panel.shell.of(PANEL_COMMAND_METHOD).length === 1);

                assert(pressEnter(field), "Enter must be consumed once it is handled as the commit");
                await waitFor(
                    "Enter's commit",
                    () => panel.shell.writes.length === 1,
                    5_000,
                    () => `calls=${JSON.stringify(panel.shell.calls)}`,
                );
                assertEqual(panel.shell.writes, ['"Enter Committed"'], "Enter committed the edit");

                // The browser may ALSO fire `change` for the same Enter. It must be harmless.
                commitValue(field);
                await waitFor(
                    "the redundant commit",
                    () => panel.shell.of(PANEL_GESTURE_METHOD).length === 2,
                );
                assert(
                    panel.shell.of(PANEL_GESTURE_METHOD)[1]?.dispatched === false,
                    "the second commit finds nothing staged",
                );
                assertEqual(panel.shell.writes.length, 1, "so exactly one write happened, not two");
            } finally {
                panel.dispose();
            }
        },
    },
    {
        // ⚠ PLANT (6): make the key UNCONDITIONAL AND NON-UNDEFINED in `PanelClient.command`
        // (`params["value"] = value ?? ""`) -> every valueless dispatch in the app grows a member the
        // C++ side then tries to parse, and this case reds.
        //
        // ⚠ AND THE MUTATION THIS CASE CANNOT SEE, written down so no stronger claim gets recorded
        // than it supports: a bare `params["value"] = value;` with `value === undefined` is
        // byte-identical on the wire, because `ShellBridge.call` serialises through `JSON.stringify`,
        // which DROPS an `undefined` member. That variant is not a behaviour change at all — which
        // is exactly what "byte-identical" asserts — so it is nothing for a guard to catch.
        name: "hydration: a valueless dispatch is byte-identical to before (no `value` member at all)",
        run: async () => {
            const panel = mount();
            try {
                panel.node("inspector.action.reset").dispatchEvent(
                    new MouseEvent("click", { bubbles: true }),
                );
                await waitFor(
                    "the plain command",
                    () => panel.shell.of(PANEL_COMMAND_METHOD).length === 1,
                );
                const params = panel.shell.of(PANEL_COMMAND_METHOD)[0]?.params ?? {};
                assertEqual(
                    Object.keys(params).sort(),
                    ["commandId", "nodeId", "panelId"],
                    "a non-value affordance must send the pre-e09e-1 params, unchanged",
                );
                assert(!("value" in params), "no `value` member may appear for a valueless dispatch");
            } finally {
                panel.dispose();
            }
        },
    },
    {
        // ⚠ PLANT (7): drop `#commit`'s `#gesturesEnabled` guard -> a panel whose provider bound no
        // write gateway starts sending a `commit` verb the Shell can only refuse, and this case reds.
        name: "hydration: a panel WITHOUT gestures stages but never sends a verb the Shell would refuse",
        run: async () => {
            const panel = mount(false);
            try {
                const field = panel.input(`${WIDGET_PREFIX}/name`);
                type(field, "no gateway here");
                await waitFor("the stage", () => panel.shell.of(PANEL_COMMAND_METHOD).length === 1);
                commitValue(field);
                await settle();
                assertEqual(
                    panel.shell.of(PANEL_GESTURE_METHOD).length,
                    0,
                    "`gestures:false` must send no gesture verb",
                );
                assertEqual(panel.shell.writes, [], "and nothing is written");
            } finally {
                panel.dispose();
            }
        },
    },
    {
        // ⚠ NO PLANT, DELIBERATELY — this is a CHARACTERIZATION case. It pins behaviour that is
        // KNOWN WRONG (#434) so the eventual fix flips a RED test instead of changing the write path
        // silently. It asserts what the code DOES, not what it should do.
        //
        // `/tag` is a STRING field whose rendered value is `42`, because `render_value` prints every
        // string bare. Nothing in the mounted DOM says "string", so the encoder reads the field as
        // JSON-typed and passes the next edit through UNQUOTED — retyping the field on disk. This is
        // not a contrived fixture: typing `42` into ANY string field once leaves it rendered exactly
        // like this, so the write path reaches this state on its own (`commandValueFor`'s ⚠ block).
        //
        // WHEN #434 LANDS: INVERT this case to expect `'"7"'`. Do not delete it.
        name: "hydration: ⚠ KNOWN WRONG (#434) — a string field rendered as a literal is retyped",
        run: async () => {
            const panel = mount();
            try {
                type(panel.input(`${WIDGET_PREFIX}/tag`), "7");
                await waitFor(
                    "the edit to the literal-looking string field",
                    () => panel.shell.of(PANEL_COMMAND_METHOD).length === 1,
                );
                assertEqual(
                    lastCommandValue(panel.shell),
                    "7",
                    "TODAY: sent BARE, so a string field silently becomes a number — the #434 defect",
                );
            } finally {
                panel.dispose();
            }
        },
    },
    {
        // ⚠ PLANT (8): drop the `input`/`change` `removeEventListener` calls from `dispose()` -> the
        // re-attached control's events reach the still-bound delegated listeners and this case reds.
        //
        // THE RE-ATTACH IS THE WHOLE CASE, and without it the assertion proves nothing. `dispose()`
        // also calls `replaceChildren()`, which DETACHES every control from the container — and an
        // event dispatched on a detached node never reaches a container listener, released or not.
        // Asserting on the field where dispose left it would therefore stay GREEN under the very
        // mutation this case names, recording only that `replaceChildren` ran. Putting the control
        // back is what leaves the listeners as the one thing that can still keep the runtime silent.
        //
        // The POSITIVE CONTROL is the other half: "sent nothing" is equally satisfied by a runtime
        // that bound nothing at all, or by a `type()` helper that stopped working. Proving the same
        // control was LIVE one line earlier is what excludes both.
        name: "hydration: dispose releases the value listeners",
        run: async () => {
            const panel = mount();
            try {
                const field = panel.input(`${WIDGET_PREFIX}/name`);

                type(field, "before dispose");
                await waitFor("the LIVE stage", () => panel.shell.calls.length === 1);

                panel.runtime.dispose();
                panel.container.appendChild(field);
                assert(
                    panel.container.contains(field),
                    "the control must be back INSIDE the container, or the listeners are not under test",
                );
                type(field, "after dispose");
                commitValue(field);
                assert(!pressEnter(field), "a disposed runtime must not consume Enter either");
                await settle();
                assertEqual(
                    panel.shell.calls.length,
                    1,
                    "a disposed runtime must send nothing MORE than the pre-dispose control",
                );
            } finally {
                panel.dispose();
            }
        },
    },
    {
        // THE PATCH PATH, which until M9 e09e-3 nothing here ever entered: every other case in this
        // file applies exactly ONE revision, so `apply` took its `#adoptAll` branch and the whole
        // incremental patcher — the thing 04 §4 step 3 is about — ran in no test at all. It was
        // broken the entire time: `apply` handed `#patchElement` the `<template>` ELEMENT, whose own
        // `children` is always empty (a template's parsed markup lives in `content`), so the patch
        // read an EMPTY source and its trailing-removal loop deleted every mounted child. The wipe
        // was survivable-looking because it leaves `childElementCount === 0`, so the NEXT render
        // re-mounts — which is why a panel that re-renders TWICE healed and only a panel that
        // re-renders ONCE stayed blank. `editor-cef-smoke-shell-inspector-fanout` is what found it.
        //
        // ⚠ PLANT (9): pass `incoming` instead of `incoming.content` in `apply` (the defect verbatim)
        // -> the container is emptied and `panel.node` reds on the FIRST assertion below.
        // ⚠ PLANT (10): drop `"value"` from `SYNCED_ATTRIBUTES` -> the tree survives and the identity
        // holds, but the moved value never lands and the third assertion reds.
        name: "hydration: a second revision PATCHES the mounted tree in place (it must not wipe it)",
        run: async () => {
            const panel = mount();
            try {
                const speed = panel.input(`${WIDGET_PREFIX}/speed`);
                assertEqual(speed.value, "1.5", "the fixture's authored value, before the patch");
                const mountedBefore =
                    panel.container.querySelectorAll("[data-node-id]").length;
                assert(mountedBefore > 1, "the fixture must mount a tree worth patching");

                panel.runtime.apply({
                    panelId: PANEL_ID,
                    revision: 2,
                    html: PATCHED_HTML,
                    focusOrder: [`${WIDGET_PREFIX}/name`],
                    commands: [{ id: EDIT_COMMAND, title: "Edit field" }],
                });

                // 1. THE TREE SURVIVED. `node()` throws when the id is not mounted, so this is the
                //    assertion the wipe reds on — and it is first deliberately.
                panel.node("inspector.panel");
                panel.node(`${WIDGET_PREFIX}/name`);
                assert(
                    panel.container.querySelectorAll("[data-node-id]").length > mountedBefore,
                    "the patched tree must keep every node it had and gain the new one",
                );
                // 2. …AS THE SAME ELEMENTS. A patch that re-imported everything would satisfy (1)
                //    while losing exactly what keying by node id exists to preserve (focus, scroll,
                //    selection, transition state).
                assert(
                    panel.input(`${WIDGET_PREFIX}/speed`) === speed,
                    "the re-rendered field must be the SAME element, reused",
                );
                // 3. …CARRYING THE NEW VALUE. Read off the attribute as well as the property: the
                //    property reflects the attribute only while the control is not dirty, and the
                //    attribute is what `#syncAttributes` actually writes.
                assertEqual(speed.getAttribute("value"), "2.5", "the moved value reached the DOM");
                assertEqual(speed.value, "2.5", "…and the control displays it");
                // 4. …AND THE NEW ROW WAS INSERTED.
                assertEqual(
                    panel.input(`${WIDGET_PREFIX}/fresh`).value,
                    "7",
                    "a field the new revision added must be mounted",
                );
            } finally {
                panel.dispose();
            }
        },
    },
];
