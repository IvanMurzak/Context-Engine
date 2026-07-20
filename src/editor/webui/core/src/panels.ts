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
 */
export type PanelContentType = "uitree" | "iframe" | "unknown";

export interface PanelDock {
    readonly zone: DockZone;
    readonly singleton: boolean;
    readonly minWidth: number;
    readonly minHeight: number;
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
    readonly contentType: PanelContentType;
    readonly schemaVersion: number;
    readonly capabilities: readonly string[];
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

/** The hydration payload for one panel (`shell::PanelRender`). */
export interface PanelRender {
    readonly panelId: string;
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
    readonly revision: number;
}

/** The outcome of a `panel.state.set` call — the D6 restore contract. */
export interface PanelRestoreResult {
    readonly restored: boolean;
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
        singleton: readBoolean(dock, "singleton"),
        minWidth: readNumber(dock, "minWidth"),
        minHeight: readNumber(dock, "minHeight"),
    };
}

const CONTENT_TYPES: readonly PanelContentType[] = ["uitree", "iframe"];

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
        contentType: readContentType(content),
        schemaVersion: readNumber(state, STATE_SCHEMA_VERSION_KEY, 1),
        capabilities: readStringArray(value, "capabilities"),
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
    async render(panelId: string): Promise<PanelRender | null> {
        try {
            return parsePanelRender(await this.#bridge.call(PANEL_RENDER_METHOD, { panelId }));
        } catch (error) {
            if (error instanceof BridgeError) {
                return null;
            }
            throw error;
        }
    }

    /** Dispatch a bound command. `nodeId` is the activated node — the panel maps it to its own model. */
    async command(
        panelId: string,
        commandId: string,
        nodeId: string,
    ): Promise<PanelDispatchResult | null> {
        try {
            const result = await this.#bridge.call(PANEL_COMMAND_METHOD, {
                panelId,
                commandId,
                nodeId,
            });
            if (!isRecord(result)) {
                return null;
            }
            return {
                dispatched: readBoolean(result, "dispatched"),
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
    ): Promise<PanelDispatchResult | null> {
        try {
            const result = await this.#bridge.call(PANEL_GESTURE_METHOD, {
                panelId,
                verb,
                ...detail,
            });
            if (!isRecord(result)) {
                return null;
            }
            return {
                dispatched: readBoolean(result, "dispatched"),
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
    async getState(panelId: string): Promise<unknown> {
        try {
            const result = await this.#bridge.call(PANEL_STATE_GET_METHOD, { panelId });
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
    async setState(panelId: string, state: unknown): Promise<PanelRestoreResult | null> {
        try {
            const result = await this.#bridge.call(PANEL_STATE_SET_METHOD, { panelId, state });
            if (!isRecord(result)) {
                return null;
            }
            return {
                restored: readBoolean(result, "restored"),
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
}
