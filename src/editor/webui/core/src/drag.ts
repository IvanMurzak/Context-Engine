// The CROSS-WINDOW DRAG probe/answer, JS side (M9 e10c, design 04 §2).
//
// This is the TS mirror of the `drag.*` half of `src/editor/shell/include/context/editor/shell/
// window_bridge.h`, and it is the TARGET window's side of a Shell-mediated cross-window drag. When a
// panel is dragged out of ANOTHER window and the cursor crosses into THIS one, the Shell publishes a
// hover; this window's editor-core reads it on its poll (`drag.probe`), hit-tests its OWN layout for a
// drop zone, and reports the answer back (`drag.report-zone`). That round trip is the whole novel seam
// of e10c — it is genuinely cross-origin (e08a: origin is per wire connection), because the drag's
// SOURCE runs in a different window's editor-core over a different bridge.
//
// TWO FACTS ARE LOAD-BEARING, exactly as window.ts / panels.ts state:
//
//   1. THE VOCABULARY IS CROSS-LANGUAGE AND GATED. `DRAG_PROBE_METHOD` / `DRAG_REPORT_ZONE_METHOD` are
//      byte-compared against the C++ constants (`kDragProbeMethod` / `kDragReportZoneMethod`) by
//      `tools/check_webui_assets.py` (ctest `webui-panel-contract`), reading the values out of the
//      BUILT bundle. A rename on either side would otherwise unbind the drag surface SILENTLY.
//
//   2. IN-WINDOW DOCKVIEW DnD IS UNTOUCHED. This module NEVER runs when no cross-window drag is active
//      (`probe.active === false` ⇒ it does nothing), and a cross-window drop rehomes through e10b's
//      move path — it never calls Dockview's own drop API. Dragging a panel WITHIN one window stays
//      entirely Dockview's, exactly as design 04 §2 requires.

import type { ShellBridge } from "./bridge.js";
import { BridgeError, isRecord } from "./bridge.js";

// --------------------------------------------------------------------------- the wire vocabulary
// MUST match window_bridge.h's kDragProbeMethod / kDragReportZoneMethod. See fact 1 above.

export const DRAG_PROBE_METHOD = "drag.probe";
export const DRAG_REPORT_ZONE_METHOD = "drag.report-zone";

// -------------------------------------------------------------------------------- the wire shapes

/**
 * What `drag.probe` reports: whether a cross-window drag is over THIS window and, if so, which panel is
 * being dragged and where the cursor is in this window's CLIENT pixels. `generation` is echoed back in
 * the zone report so the Shell drops a stale answer for a hover the cursor has already moved past.
 */
export interface DragProbe {
    readonly active: boolean;
    readonly panelId: string;
    readonly x: number;
    readonly y: number;
    readonly generation: number;
}

const INACTIVE_PROBE: DragProbe = { active: false, panelId: "", x: 0, y: 0, generation: 0 };

/** This window's answer: which drop zone the cursor is over (if any). `zoneId` is a cosmetic label. */
export interface DropZone {
    readonly valid: boolean;
    readonly zoneId: string;
}

/**
 * Where in THIS window a cross-window drag at `(x, y)` local pixels would land. A COARSE, whole-window
 * answer by design: a cross-window drop rehomes through e10b's move path (not Dockview's own drop), so
 * the precise in-window slot stays Dockview's job — this only decides WHICH window accepts the panel.
 */
export type DropZoneHitTest = (x: number, y: number) => DropZone;

// ------------------------------------------------------------------------------- total parsers

function readString(source: Record<string, unknown>, key: string): string {
    const value = source[key];
    return typeof value === "string" ? value : "";
}

function readNumber(source: Record<string, unknown>, key: string): number {
    const value = source[key];
    return typeof value === "number" && Number.isFinite(value) ? value : 0;
}

/** Parse a `drag.probe` result. Any unreadable / inactive shape is the INACTIVE probe (do nothing). */
export function parseProbe(value: unknown): DragProbe {
    if (!isRecord(value) || value["active"] !== true) {
        return INACTIVE_PROBE;
    }
    return {
        active: true,
        panelId: readString(value, "panelId"),
        x: readNumber(value, "x"),
        y: readNumber(value, "y"),
        generation: readNumber(value, "generation"),
    };
}

// ------------------------------------------------------------------------------- the client

/**
 * The typed client over the `drag.*` bridge methods. Thin and total, like `WindowClient`: it validates
 * envelopes and holds no drag state (the session lives in the Shell). A REFUSAL — an older Shell with no
 * drag surface, or one that installed the surface with no session — is an ordinary "no drag" state, so a
 * `BridgeError` resolves to the INACTIVE probe rather than throwing. A transport failure still rejects.
 */
export class DragClient {
    readonly #bridge: ShellBridge;

    constructor(bridge: ShellBridge) {
        this.#bridge = bridge;
    }

    /** Is a cross-window drag over THIS window? The INACTIVE probe when not (or when unsupported). */
    async probe(): Promise<DragProbe> {
        try {
            return parseProbe(await this.#bridge.call(DRAG_PROBE_METHOD));
        } catch (error) {
            if (error instanceof BridgeError) {
                return INACTIVE_PROBE;
            }
            throw error;
        }
    }

    /** Report this window's drop zone for hover `generation`. A refusal is swallowed — a best-effort. */
    async reportZone(generation: number, zone: DropZone): Promise<void> {
        try {
            await this.#bridge.call(DRAG_REPORT_ZONE_METHOD, {
                valid: zone.valid,
                zoneId: zone.zoneId,
                generation,
            });
        } catch (error) {
            if (error instanceof BridgeError) {
                return;
            }
            throw error;
        }
    }
}

/**
 * One poll of the cross-window drag probe (M9 e10c). When a drag from ANOTHER window is over this one,
 * hit-test the local drop zone and report it back — the genuine cross-origin round trip. Returns the
 * probe (for the smoke / tests). NEVER throws, and does NOTHING when no cross-window drag is active — so
 * in-window Dockview DnD is never intercepted (fact 2 above).
 */
export async function pumpCrossWindowDrag(
    client: DragClient,
    hitTest: DropZoneHitTest,
): Promise<DragProbe> {
    const probe = await client.probe();
    if (!probe.active) {
        return probe;
    }
    const zone = hitTest(probe.x, probe.y);
    await client.reportZone(probe.generation, zone);
    return probe;
}

/**
 * The default hit-tester: the whole editor window is a valid rehome target (`"center"`). `root` is the
 * element whose client box bounds "inside this window"; a cross-window drop anywhere inside it rehomes
 * the panel INTO this window through e10b's move path. Deliberately coarse (see `DropZoneHitTest`): it
 * reads only a bounding box and NEVER touches Dockview's drop machinery, so in-window docking is
 * unaffected. A null root (no DOM yet) answers "no zone" rather than throwing.
 */
export function makeDropZoneHitTest(resolveRoot: () => HTMLElement | null): DropZoneHitTest {
    return (x, y): DropZone => {
        const root = resolveRoot();
        if (root === null) {
            return { valid: false, zoneId: "" };
        }
        const rect = root.getBoundingClientRect();
        const inside = x >= 0 && y >= 0 && x <= rect.width && y <= rect.height;
        return inside ? { valid: true, zoneId: "center" } : { valid: false, zoneId: "" };
    };
}
