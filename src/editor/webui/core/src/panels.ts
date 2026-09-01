// The `panel.*` bridge surface, JS side (M9 e05d1, design 04 §3-§4).
//
// This is the TS mirror of `src/editor/shell/include/context/editor/shell/panel_host.h`. Two facts
// about it are load-bearing:
//
//   1. THE VOCABULARY IS CROSS-LANGUAGE AND GATED. The method names and gesture verbs below are
//      byte-compared against the C++ constants by `tools/check_webui_assets.py --panel-contract`
//      (ctest `webui-panel-contract`), reading the values out of the BUILT bundle. That is the same
//      discipline e05c applied to the scheme vocabulary, and it exists because a rename on either
//      side otherwise unbinds the panel surface SILENTLY — the renderer would call a method the
//      Shell no longer routes and the editor would come up empty with no build error anywhere.
//
//   2. EVERY PARSER IS TOTAL. The Shell is trusted, but a response is still validated structurally:
//      a silently-accepted malformed payload surfaces later as `undefined` in the DOM, far from the
//      cause. `parse*` returns `null` on anything it cannot read, and the caller degrades honestly.
//
// NOTHING HERE KNOWS A PANEL ID. The roster arrives from `panel.list`; the app renders whatever it
// contains. That is the property e05d3 depends on — it hosts Scene tree and Inspector by binding two
// C++ providers, with no change to this file or to the hydration runtime.

import type { ShellBridge } from "./bridge.js";
import { BridgeError, isRecord } from "./bridge.js";

// --------------------------------------------------------------------------- the wire vocabulary
// MUST match panel_host.h's kPanel*Method. See note 1 above.

export const PANEL_LIST_METHOD = "panel.list";
export const PANEL_RENDER_METHOD = "panel.render";
export const PANEL_COMMAND_METHOD = "panel.command";
export const PANEL_GESTURE_METHOD = "panel.gesture";
export const PANEL_STATE_GET_METHOD = "panel.state.get";
export const PANEL_STATE_SET_METHOD = "panel.state.set";
/**
 * The instance-lifecycle verb (editor-UX c3). Mirrors `kPanelInstanceCloseMethod`.
 *
 * ONE verb, and it is the CLOSE half: instance creation is implicit — this side owns panel lifecycle,
 * mints the instance id, and the first `panel.render` carrying it materialises the Shell-side model
 * — so an `open` verb would only buy a round trip `PanelHost.open` would have to await, turning a
 * synchronous call async across every caller. Release cannot be implicit the same way: nothing else
 * on the wire says "this copy is gone", so without this the Shell's instance table would only grow
 * and a `limited` panel would exhaust its ceiling over the session rather than holding `max` LIVE.
 */
export const PANEL_INSTANCE_CLOSE_METHOD = "panel.instance.close";

/**
 * The separator between a panel KIND and its instance ordinal (`builtin.problems#1`). Mirrors
 * `kPanelInstanceSeparator` and is byte-compared by `webui-panel-contract`.
 *
 * WHY AN ID IS COMPOSED RATHER THAN OPAQUE. Dockview restores a persisted arrangement BY PANEL ID and
 * calls `createComponent` for each one before this app has registered anything, so the KIND has to be
 * recoverable from the id alone or a restore cannot know which renderer to build. Both sides compose
 * AND decompose with this, which is why a drift is the quietest failure in the panel family: every
 * instance would resolve to a kind that does not exist, with no method refusing.
 */
export const PANEL_INSTANCE_SEPARATOR = "#";

/**
 * Compose an instance id. `ordinal` is 1-based and per KIND, never global, so the FIRST copy of every
 * panel is `<id>#1` on every boot — which is what makes a persisted single-instance arrangement
 * restore unchanged.
 */
export function makeInstanceId(panelId: string, ordinal: number): string {
    return `${panelId}${PANEL_INSTANCE_SEPARATOR}${String(ordinal)}`;
}

/**
 * The KIND an instance id names, or the whole string when it carries no separator.
 *
 * Splits on the LAST separator, mirroring `panel_id_of_instance`: a panel id is free to contain one
 * (nothing in the registry forbids it), and splitting on the first would resolve `a#b#1` to the kind
 * `a` — which does not exist — rather than to `a#b`, which does. A bare id (a persisted arrangement
 * written before instances existed) reads as the kind itself, which is the honest restore.
 */
export function panelIdOfInstance(instanceId: string): string {
    const at = instanceId.lastIndexOf(PANEL_INSTANCE_SEPARATOR);
    return at === -1 ? instanceId : instanceId.slice(0, at);
}

/**
 * The continuous-gesture verbs (04 §4). A CLOSED set, mirroring `shell::GestureVerb` — the C++ panel
 * models were designed against exactly these four, and the Shell REFUSES anything else, so inventing
 * a fifth here would produce a runtime refusal rather than a new capability.
 */
export const GESTURE_VERBS = ["begin", "extend", "commit", "cancel"] as const;
export type GestureVerb = (typeof GESTURE_VERBS)[number];

/** The persisted-blob member names. Mirrors `contract::kState*Key` (gui/contract/panel_state.h). */
export const STATE_SCHEMA_VERSION_KEY = "schemaVersion";
export const STATE_DATA_KEY = "data";

// -------------------------------------------------------------------------------- the roster types

/** Where a panel docks by default (`contract::DockZone`'s token vocabulary). */
export type DockZone = "left" | "right" | "top" | "bottom" | "center";

/**
 * How a panel's content is delivered (`contract::PanelContentType`'s closed vocabulary).
 *
 * Modelled as a closed union — like `DockZone` — rather than a bare string BECAUSE IT GATES AN
 * HTML SINK. A `uitree` payload is mounted through `innerHTML`, which is safe only because it came
 * from `render_html`'s escaping contract; an `iframe` panel's content is third-party (04 §5) and
 * belongs in a sandboxed iframe on a different origin. `unknown` is the honest parse of a token
 * this build does not recognise, and it is NOT hostable — an unrecognised content type must fail
 * closed rather than defaulting into the sink.
 *
 * `local` (M9 e06d) is editor-core's OWN content: a panel this bundle renders itself from the
 * component kit, for the narrow case where the panel's subject is the renderer's own state (Settings
 * — the active theme IS the CSS custom properties on this document). It touches no HTML sink at all:
 * PanelHost hands the panel's registered factory a DOM element and the factory builds nodes. So the
 * innerHTML reasoning above simply does not apply to it, which is why it is a distinct token rather
 * than a flavour of `uitree`.
 */
export type PanelContentType = "uitree" | "iframe" | "local" | "unknown";

export interface PanelDock {
    readonly zone: DockZone;
    readonly minWidth: number;
    readonly minHeight: number;
}

/**
 * How many live copies of a panel kind may exist (`contract::InstanceMode`'s closed vocabulary,
 * manifest v3 / D6).
 *
 * ⚠ THIS REPLACED `dock.singleton`, which is GONE from the wire — `PanelHost::list` no longer emits
 * it. It moved out of `dock` because "how many copies" was never a docking fact, and it moved in the
 * SAME change as the C++ struct: a parser left reading a member the projection no longer writes reads
 * `false` for every panel, silently, with nothing failing.
 *
 * A closed union like `DockZone`, and parsed the same way — by searching the closed list — so the
 * accepted tokens are exactly the ones `instance_mode_token` emits.
 */
export const PANEL_INSTANCE_MODES = ["singleton", "limited", "unlimited"] as const;
export type PanelInstanceMode = (typeof PANEL_INSTANCE_MODES)[number];

export interface PanelInstances {
    readonly mode: PanelInstanceMode;
    /** The ceiling for `limited`, and 0 on the other two (the C++ registry refuses it there). */
    readonly max: number;
}

/**
 * One rostered panel, as `panel.list` reports it.
 *
 * `hosted` / `gestures` / `persists` are HOST facts, not manifest ones: they say what THIS build can
 * actually do with the panel. A rostered panel with no provider reports `hosted: false` and is still
 * listed — which is how the editor shows its whole panel set while two panels are still blocked
 * behind the D10 boundary refactor (e05d3). The app must therefore never assume a listed panel is
 * mountable; `hosted` is the gate.
 */
export interface PanelManifest {
    readonly id: string;
    readonly kind: string;
    readonly title: string;
    readonly icon: string;
    readonly contractVersion: number;
    readonly dock: PanelDock;
    /**
     * Manifest v3 `instances` (04 §2). Parsed here; CONSUMED by the instance runtime (c3) — until
     * then this is a declaration the renderer carries and does not act on, which is expected.
     */
    readonly instances: PanelInstances;
    /**
     * Manifest v3 `path` (04 §2) — slash-separated DISPLAY grouping for the Window menu's panel tree
     * (d1). Empty means top level. NOT a filesystem path and nothing resolves it.
     */
    readonly path: string;
    readonly contentType: PanelContentType;
    /**
     * The manifest's `content.entry` (M9 e13a-2) — the URL an `iframe` panel loads, mirroring the
     * C++ `ContentSpec::entry`. EMPTY for every other content type, which the C++ registry
     * validation already enforces (`registry.cpp`: `uitree`/`local` with a non-empty entry is a
     * rejected contribution), so an entry arriving on a `uitree` manifest is a Shell-side defect and
     * this field is simply never read for one.
     *
     * CARRIED RAW AND UNVALIDATED HERE — deliberately. `parsePanelManifest` is a structural parser;
     * whether this string is a URL editor-core may put in a frame is a SECURITY question with its
     * own grammar, its own tests and its own module (`extpanel.ts` `parseExtPanelEntry`). Splitting
     * them keeps the parser total (it never has to decide what a "safe" URL is) and keeps the
     * grammar in one place instead of half-applied at parse time.
     */
    readonly contentEntry: string;
    readonly schemaVersion: number;
    readonly capabilities: readonly string[];
    /** The manifest-declared commands (manifest v2 `commands`, 04 §3), the e07b registry's source (c). */
    readonly commands: readonly PanelManifestCommand[];
    readonly hosted: boolean;
    readonly gestures: boolean;
    readonly persists: boolean;
    readonly revision: number;
}

export interface PanelRoster {
    readonly contractMajor: number;
    readonly panels: readonly PanelManifest[];
}

/** One command a panel exposes (`uitree::Command`). */
export interface PanelCommand {
    readonly id: string;
    readonly title: string;
}

/**
 * One command a panel DECLARES IN ITS MANIFEST (manifest v2 `commands`, 04 §3), mirroring the C++
 * `contract::CommandContribution` `{ id, title, when }`.
 *
 * DISTINCT from `PanelCommand` (a render-time command a uitree node activates): manifest commands are
 * the panel-manifest source of the e07b command registry, declared once in the manifest — chiefly for
 * iframe contributions, which have no C++ model to read commands from. `when` is an optional context
 * clause (`""` = always), mirroring the C++ field's "empty = always" contract.
 */
export interface PanelManifestCommand {
    readonly id: string;
    readonly title: string;
    readonly when: string;
}

/** The hydration payload for one panel (`shell::PanelRender`). */
export interface PanelRender {
    readonly panelId: string;
    /**
     * WHICH COPY this payload is of (c3) — echoed by the Shell even for a call that named none, so a
     * host holding several instances of one kind routes it to the right DOM slot instead of to the
     * first slot whose KIND matches.
     */
    readonly instanceId: string;
    readonly revision: number;
    /**
     * Semantic HTML from the C++ `uitree::render_html`. Every interpolated value has ALREADY been
     * through the C-F6 escaping contract on the native side (`uitree::escape_html_text`), and the
     * strict no-inline-script CSP is the backstop — see `hydration.ts` on why this is safe to mount
     * and what would make it stop being safe.
     */
    readonly html: string;
    /** Node ids in the panel model's declared keyboard order (`uitree::focus_order`). */
    readonly focusOrder: readonly string[];
    readonly commands: readonly PanelCommand[];
}

/** The outcome of a `panel.command` / `panel.gesture` call. */
export interface PanelDispatchResult {
    /** The PANEL's verdict. `false` is an ordinary outcome (a click on a dead row), not an error. */
    readonly dispatched: boolean;
    /** The copy the Shell addressed (c3) — the id sent, or the kind's default instance. */
    readonly instanceId: string;
    readonly revision: number;
}

/** The outcome of a `panel.state.set` call — the D6 restore contract. */
export interface PanelRestoreResult {
    readonly restored: boolean;
    /** The copy the Shell addressed (c3). */
    readonly instanceId: string;
    /** Empty when restored; else `gui.state_schema_mismatch` / `gui.state_malformed`. */
    readonly code: string;
    /** Empty when restored; else the human/AI-readable reason the panel got NULL state instead. */
    readonly diagnostic: string;
    readonly revision: number;
}

// ------------------------------------------------------------------------------- total parsers

function readString(source: Record<string, unknown>, key: string, fallback = ""): string {
    const value = source[key];
    return typeof value === "string" ? value : fallback;
}

function readNumber(source: Record<string, unknown>, key: string, fallback = 0): number {
    const value = source[key];
    return typeof value === "number" && Number.isFinite(value) ? value : fallback;
}

function readBoolean(source: Record<string, unknown>, key: string): boolean {
    return source[key] === true;
}

function readStringArray(source: Record<string, unknown>, key: string): string[] {
    const value = source[key];
    if (!Array.isArray(value)) {
        return [];
    }
    // Non-string members are DROPPED rather than coerced: a capability token that is not a string is
    // not a capability, and stringifying it would invent a grant nobody declared.
    return value.filter((item): item is string => typeof item === "string");
}

const DOCK_ZONES: readonly DockZone[] = ["left", "right", "top", "bottom", "center"];

function readDock(source: Record<string, unknown>): PanelDock {
    const dock = isRecord(source["dock"]) ? source["dock"] : {};
    const zoneToken = readString(dock, "zone", "center");
    // An unrecognized zone falls back to `center` rather than being trusted into a layout call that
    // would throw: the manifest's vocabulary is closed, so anything else is drift, not a new zone.
    const zone = DOCK_ZONES.find((candidate) => candidate === zoneToken) ?? "center";
    return {
        zone,
        minWidth: readNumber(dock, "minWidth"),
        minHeight: readNumber(dock, "minHeight"),
    };
}

/**
 * Parse a manifest's `instances` block (manifest v3), FAILING CLOSED to `singleton`.
 *
 * Deliberately NOT defaulted the permissive way. `readDock`'s zone fallback is `center` because the
 * cost of being wrong there is cosmetic — where a panel first appears. Here the cost is how many
 * copies of a panel may exist, so an absent, non-record or unrecognised `mode` reads as the MOST
 * RESTRICTIVE answer, which is also `InstanceSpec`'s own C++ default. `max` is clamped at 0 for the
 * same reason the C++ side refuses a negative: a ceiling below one is not a ceiling.
 *
 * `max` is ALSO dropped on any mode but `limited`, mirroring the C++ registry, which REFUSES a `max`
 * stated elsewhere rather than ignoring it. Carrying it through would matter precisely when the mode
 * failed closed: `{mode: "many", max: 5}` would parse as `{mode: "singleton", max: 5}`, and the c3
 * instance runtime reading `max` without re-checking `mode` would apply a ceiling no manifest the
 * registry accepts could ever have declared.
 */
function readInstances(source: Record<string, unknown>): PanelInstances {
    const instances = isRecord(source["instances"]) ? source["instances"] : {};
    const token = readString(instances, "mode");
    const mode = PANEL_INSTANCE_MODES.find((candidate) => candidate === token) ?? "singleton";
    return { mode, max: mode === "limited" ? Math.max(0, readNumber(instances, "max")) : 0 };
}

const CONTENT_TYPES: readonly PanelContentType[] = ["uitree", "iframe", "local"];

/**
 * Parse a panel's content type, FAILING CLOSED.
 *
 * Deliberately NOT defaulted to `uitree`. Every other reader here defaults to the permissive value
 * because the cost of a wrong default is a cosmetic one; here the cost is routing content of an
 * unknown provenance into an `innerHTML` sink. A manifest with a missing, non-string, or
 * unrecognised `content.type` therefore reads as `unknown`, which `PanelHost` refuses to mount.
 */
function readContentType(content: Record<string, unknown>): PanelContentType {
    const token = readString(content, "type");
    return CONTENT_TYPES.find((candidate) => candidate === token) ?? "unknown";
}

/**
 * Parse a manifest's `commands` (04 §3), TOTAL and fail-closed. Non-record entries and entries with
 * no usable id are DROPPED (never coerced) — the same discipline `parsePanelRender` applies to
 * render-time commands. A missing/non-array `commands` member yields the empty list.
 */
function readManifestCommands(source: Record<string, unknown>): PanelManifestCommand[] {
    const raw = source["commands"];
    if (!Array.isArray(raw)) {
        return [];
    }
    const commands: PanelManifestCommand[] = [];
    for (const entry of raw) {
        if (!isRecord(entry)) {
            continue;
        }
        const id = readString(entry, "id");
        if (id === "") {
            continue;
        }
        commands.push({ id, title: readString(entry, "title", id), when: readString(entry, "when") });
    }
    return commands;
}

/** Parse one roster entry. `null` when it carries no usable id. */
export function parsePanelManifest(value: unknown): PanelManifest | null {
    if (!isRecord(value)) {
        return null;
    }
    const id = readString(value, "id");
    if (id === "") {
        return null;
    }
    const content = isRecord(value["content"]) ? value["content"] : {};
    const state = isRecord(value["state"]) ? value["state"] : {};
    return {
        id,
        kind: readString(value, "kind", "panel"),
        title: readString(value, "title", id),
        icon: readString(value, "icon"),
        contractVersion: readNumber(value, "contractVersion"),
        dock: readDock(value),
        instances: readInstances(value),
        path: readString(value, "path"),
        contentType: readContentType(content),
        contentEntry: readString(content, "entry"),
        schemaVersion: readNumber(state, STATE_SCHEMA_VERSION_KEY, 1),
        capabilities: readStringArray(value, "capabilities"),
        commands: readManifestCommands(value),
        hosted: readBoolean(value, "hosted"),
        gestures: readBoolean(value, "gestures"),
        persists: readBoolean(value, "persists"),
        revision: readNumber(value, "revision"),
    };
}

/** Parse a `panel.list` result. `null` when the envelope is unreadable; entries that are not are skipped. */
export function parsePanelRoster(value: unknown): PanelRoster | null {
    if (!isRecord(value) || !Array.isArray(value["panels"])) {
        return null;
    }
    const panels: PanelManifest[] = [];
    for (const entry of value["panels"]) {
        const parsed = parsePanelManifest(entry);
        if (parsed !== null) {
            panels.push(parsed);
        }
    }
    return { contractMajor: readNumber(value, "contractMajor"), panels };
}

/** Parse a `panel.render` result. `null` when it carries no panel id. */
export function parsePanelRender(value: unknown): PanelRender | null {
    if (!isRecord(value)) {
        return null;
    }
    const panelId = readString(value, "panelId");
    if (panelId === "") {
        return null;
    }
    const commands: PanelCommand[] = [];
    const rawCommands = value["commands"];
    if (Array.isArray(rawCommands)) {
        for (const entry of rawCommands) {
            if (!isRecord(entry)) {
                continue;
            }
            const id = readString(entry, "id");
            if (id !== "") {
                commands.push({ id, title: readString(entry, "title", id) });
            }
        }
    }
    return {
        panelId,
        // ⚠ DEFAULTED TO THE PANEL ID, not to the empty string. A Shell that predates c3 — or a
        // harness that answers a bare render envelope — carries no `instanceId`, and the honest
        // reading of that is "the kind's one copy", which is exactly what the panel id names in a
        // single-instance world. Defaulting to `""` would instead produce a render nothing could be
        // keyed to, and the caller would drop a payload that is perfectly usable.
        instanceId: readString(value, "instanceId", panelId),
        revision: readNumber(value, "revision"),
        html: readString(value, "html"),
        focusOrder: readStringArray(value, "focusOrder"),
        commands,
    };
}

// ------------------------------------------------------------------------------- the client

/**
 * The typed client over the `panel.*` bridge methods.
 *
 * Thin on purpose: it validates envelopes and narrows types, and holds NO panel state — the models
 * live in C++ (D17: "the C++ panel models stay the logic + a11y authority"), so a cache here would
 * be a second source of truth free to disagree with them.
 */
export class PanelClient {
    readonly #bridge: ShellBridge;

    constructor(bridge: ShellBridge) {
        this.#bridge = bridge;
    }

    /** The whole roster, hosted and not. `null` when the Shell answered something unreadable. */
    async list(): Promise<PanelRoster | null> {
        return parsePanelRoster(await this.#bridge.call(PANEL_LIST_METHOD));
    }

    /**
     * Render one panel.
     *
     * A REFUSAL IS RETURNED, NOT THROWN: `panel.not_hosted` is the expected answer for the panels
     * still blocked behind the D10 boundary (e05d3), and making the caller catch an exception for an
     * ordinary, designed state would push try/catch into every mount path. A transport failure still
     * rejects — that is not an ordinary state.
     */
    async render(panelId: string, instanceId?: string): Promise<PanelRender | null> {
        try {
            return parsePanelRender(
                await this.#bridge.call(PANEL_RENDER_METHOD, instanceParams(panelId, instanceId)),
            );
        } catch (error) {
            if (error instanceof BridgeError) {
                return null;
            }
            throw error;
        }
    }

    /**
     * Dispatch a bound command. `nodeId` is the activated node — the panel maps it to its own model.
     *
     * `value` is the OPTIONAL edit payload (M9 e09e-1, design 05 §8's first link). A command bound to
     * a value-carrying affordance — the Inspector's fields are the shipped case — has to say WHAT the
     * human entered, and this is the only channel: `PanelHost::invoke` forwards the whole params
     * object verbatim, so the panel's C++ provider reads it straight off (`inspector_feed.cpp` reads
     * `params["value"]` and DECLINES a dispatch that carries none — until this parameter existed, a
     * DOM edit could not reach `inspector.edit` at all).
     *
     * THE KEY IS OMITTED ENTIRELY WHEN UNDEFINED, not sent as `null`/`""`. Every valueless dispatch
     * (every other panel, every non-field affordance in this one) therefore stays byte-identical on
     * the wire, so no other provider starts seeing a member it never agreed to read — and the
     * C++ side's "no parseable value" refusal keeps meaning exactly what it meant before.
     *
     * WHAT THE STRING IS: a JSON LITERAL, in a string — `"1.5"`, `"true"`, `"\"a name\""` — mirroring
     * what `context set --value` accepts (`set_command.cpp`: "pass a JSON literal"). It is NOT the
     * display text. `hydration.ts` § `commandValueFor` owns the DOM -> literal encoding and states why.
     */
    async command(
        panelId: string,
        commandId: string,
        nodeId: string,
        value?: string,
        instanceId?: string,
    ): Promise<PanelDispatchResult | null> {
        try {
            const params: Record<string, unknown> = {
                ...instanceParams(panelId, instanceId),
                commandId,
                nodeId,
            };
            if (value !== undefined) {
                params["value"] = value;
            }
            const result = await this.#bridge.call(PANEL_COMMAND_METHOD, params);
            if (!isRecord(result)) {
                return null;
            }
            return {
                dispatched: readBoolean(result, "dispatched"),
                instanceId: readString(result, "instanceId", panelId),
                revision: readNumber(result, "revision"),
            };
        } catch (error) {
            if (error instanceof BridgeError) {
                return null;
            }
            throw error;
        }
    }

    /** Dispatch a gesture verb. Only called for panels whose manifest reports `gestures: true`. */
    async gesture(
        panelId: string,
        verb: GestureVerb,
        detail: Record<string, unknown>,
        instanceId?: string,
    ): Promise<PanelDispatchResult | null> {
        try {
            const result = await this.#bridge.call(PANEL_GESTURE_METHOD, {
                ...instanceParams(panelId, instanceId),
                verb,
                ...detail,
            });
            if (!isRecord(result)) {
                return null;
            }
            return {
                dispatched: readBoolean(result, "dispatched"),
                instanceId: readString(result, "instanceId", panelId),
                revision: readNumber(result, "revision"),
            };
        } catch (error) {
            if (error instanceof BridgeError) {
                return null;
            }
            throw error;
        }
    }

    /** Read a panel's D6 state blob. `null` when it persists none. */
    async getState(panelId: string, instanceId?: string): Promise<unknown> {
        try {
            const result = await this.#bridge.call(
                PANEL_STATE_GET_METHOD,
                instanceParams(panelId, instanceId),
            );
            return isRecord(result) ? (result["state"] ?? null) : null;
        } catch (error) {
            if (error instanceof BridgeError) {
                return null;
            }
            throw error;
        }
    }

    /**
     * Restore a panel's D6 state blob.
     *
     * A schemaVersion mismatch is NOT a failure here: the Shell answers `restored: false` plus a
     * diagnostic and the panel rebuilds from its defaults (04 §3). e05d2's layout restore depends on
     * that being an ordinary result — one stale panel blob must not discard a whole layout.
     */
    async setState(
        panelId: string,
        state: unknown,
        instanceId?: string,
    ): Promise<PanelRestoreResult | null> {
        try {
            const result = await this.#bridge.call(PANEL_STATE_SET_METHOD, {
                ...instanceParams(panelId, instanceId),
                state,
            });
            if (!isRecord(result)) {
                return null;
            }
            return {
                restored: readBoolean(result, "restored"),
                instanceId: readString(result, "instanceId", panelId),
                code: readString(result, "code"),
                diagnostic: readString(result, "diagnostic"),
                revision: readNumber(result, "revision"),
            };
        } catch (error) {
            if (error instanceof BridgeError) {
                return null;
            }
            throw error;
        }
    }

    /**
     * Release one instance's Shell-side model (c3) — the CLOSE half of the instance lifecycle.
     *
     * `true` when the Shell released a live copy; `false` is ORDINARY (a double close, a close racing
     * a window teardown, a panel with no C++ model at all), never an error, so no caller has to
     * catch. `PanelHost.close` fires it without awaiting: a released model is not something the DOM
     * is waiting on, and blocking a close on a round trip would make tearing a panel down feel slow.
     */
    async closeInstance(panelId: string, instanceId: string): Promise<boolean> {
        try {
            const result = await this.#bridge.call(PANEL_INSTANCE_CLOSE_METHOD, {
                panelId,
                instanceId,
            });
            return isRecord(result) && readBoolean(result, "closed");
        } catch (error) {
            if (error instanceof BridgeError) {
                return false;
            }
            throw error;
        }
    }
}

/**
 * The `{panelId}` / `{panelId, instanceId}` param pair every panel method sends.
 *
 * ⚠ THE KEY IS OMITTED ENTIRELY WHEN THERE IS NO INSTANCE, never sent as `""` — the same discipline
 * `command`'s optional `value` follows, and for the same reason: a caller that does not address a
 * copy stays byte-identical on the wire, so the Shell's "no id ⇒ the default instance" path keeps
 * meaning exactly what it meant. An empty STRING would take the same branch today, but only because
 * the Shell reads emptiness as absence; sending it would make that an assumption instead of a
 * property of the payload.
 */
function instanceParams(panelId: string, instanceId?: string): Record<string, unknown> {
    return instanceId === undefined || instanceId === "" ? { panelId } : { panelId, instanceId };
}
