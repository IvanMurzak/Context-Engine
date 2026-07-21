// editor-core's boot sequence (M9 e05c handshake, extended by e05d1 to bring up the app).
//
// e05c's job was the CHANNEL: proving — inside the real e04 Shell window, over the real
// `context-editor://` scheme, under the real CSP — that the bundle loaded and that the bridge
// round-trips native<->JS. e05d1 adds THE APP on top of that channel: once the handshake completes,
// PanelHost creates the docking root and every Shell-hostable panel mounts and hydrates.
//
// The `editor-cef-smoke-shell` ctest asserts the native side of the handshake, which is what makes
// it an end-to-end proof rather than two halves that were each tested alone:
//
//   1. JS  -> native   `shell.hello`  — "the bundle executed and can reach the Shell"
//   2. native -> JS    the reply       — carries a nonce only the Shell knows
//   3. JS  -> native   `shell.ready`  — echoes that nonce back
//
// Step 3 is the load-bearing one: the native side only accepts a `shell.ready` whose nonce matches
// the one it just minted, so the sequence cannot pass unless a value made the FULL round trip
// through the renderer. A one-way "JS called us" ping would pass with a broken response path.
//
// e05d1 extends that proof one link further: the same smoke now also asserts the Shell's PanelHost
// SERVED `panel.list` and `panel.render`, which can only be true if the sequence below actually ran
// in the live renderer. That is the end-to-end evidence for "Problems hydrates from the live daemon
// via the bridge" — a claim no local test can make, since the local gate cannot link CEF at all.
//
// ORDERING IS LOAD-BEARING: panels are brought up AFTER `shell.ready`, never before. The handshake
// is what proves the response path works; mounting panels first would fire a burst of `panel.*`
// calls down a channel nothing has yet shown to be bidirectional, turning a clean "the bridge is
// broken" diagnosis into a pile of unexplained panel failures.

import { BridgeError, ShellBridge, isRecord } from "./bridge.js";
import { EditorStateClient, LayoutPersistence } from "./editorstate.js";
import { editorCoreInfo } from "./info.js";
import { PanelClient } from "./panels.js";
import { PanelHost } from "./panelhost.js";

/** The element the docking root mounts into. Mirrors `app/index.html`'s `<main id="editor-root">`. */
export const EDITOR_ROOT_ID = "editor-root";

/** What `bootEditorCore` did — returned so a caller (and a test) can assert on it. */
export interface BootReport {
    /** False when the bundle is running outside the Shell (no injected query function). */
    readonly attached: boolean;
    /** True once the full JS -> native -> JS -> native handshake completed. */
    readonly ready: boolean;
    /** How many panels PanelHost mounted. 0 when the app layer did not come up. */
    readonly panelsMounted: number;
    /**
     * Rostered panels this build cannot host (`hosted: false`) — today Scene tree, Inspector and the
     * six panels with no provider yet. REPORTED rather than silently skipped: "a panel is missing"
     * must be an observable fact. e05d3 shrinks this list.
     */
    readonly panelsUnavailable: readonly string[];
    /** Populated when the handshake or the app bring-up failed; empty otherwise. */
    readonly error: string;
}

/** Marks the document so the boot state is inspectable from DevTools and from a DOM scan. */
function markDocument(state: string, detail: string): void {
    if (typeof document === "undefined") {
        return;
    }
    const root = document.documentElement;
    root.setAttribute("data-editor-core", state);
    if (detail !== "") {
        root.setAttribute("data-editor-core-detail", detail);
    }
}

/**
 * Run the boot handshake.
 *
 * NEVER throws and never rejects: a failure here must leave a diagnosable document rather than an
 * unhandled rejection in a renderer nobody is watching.
 */
export async function bootEditorCore(bridge = ShellBridge.detect()): Promise<BootReport> {
    if (bridge === undefined) {
        // Loaded outside the Shell — a plain browser or a harness. Honest, not fatal.
        markDocument("detached", "no shell bridge on this global");
        return { attached: false, ready: false, panelsMounted: 0, panelsUnavailable: [], error: "" };
    }

    // Marked BEFORE the first await. `index.html` ships the literal `data-editor-core="loading"`, so
    // without this a bundle that never executed and a handshake that hung present the SAME document
    // state — in a renderer whose only diagnostic channel is this attribute. "booting" means the
    // module ran and the bridge was found.
    markDocument("booting", "");

    try {
        const info = editorCoreInfo();
        const hello = await bridge.call("shell.hello", {
            protocolMajor: info.protocolMajor,
            clientSchemaVersion: info.clientSchemaVersion,
            rpcMethodCount: info.rpcMethodCount,
        });

        // The Shell's reply carries a nonce. Echoing it back is what proves the response path, so a
        // missing one is a hard failure rather than something to paper over with a default.
        const nonce = isRecord(hello) ? hello["nonce"] : undefined;
        if (typeof nonce !== "string" || nonce === "") {
            markDocument("error", "shell.hello returned no nonce");
            return {
                attached: true,
                ready: false,
                panelsMounted: 0,
                panelsUnavailable: [],
                error: "shell.hello returned no nonce",
            };
        }

        await bridge.call("shell.ready", { nonce });

        // --- the app layer (e05d1) ----------------------------------------------------------------
        // The channel is proven; bring up the panels. A failure HERE is reported but does NOT undo
        // `ready`: the bridge genuinely does round-trip, and conflating "the editor has no panels"
        // with "the editor cannot talk to the Shell" would send the next diagnosis in exactly the
        // wrong direction.
        const panels = await startPanels(bridge);
        markDocument("ready", panels.error);
        return {
            attached: true,
            ready: true,
            panelsMounted: panels.mounted,
            panelsUnavailable: panels.unavailable,
            error: panels.error,
        };
    } catch (error) {
        const detail =
            error instanceof BridgeError
                ? `${error.reason}: ${error.message}`
                : error instanceof Error
                  ? error.message
                  : String(error);
        markDocument("error", detail);
        return {
            attached: true,
            ready: false,
            panelsMounted: 0,
            panelsUnavailable: [],
            error: detail,
        };
    }
}

/** What the app-layer bring-up produced. Internal to `bootEditorCore`'s two return paths. */
interface PanelBringUp {
    readonly mounted: number;
    readonly unavailable: readonly string[];
    readonly error: string;
}

/**
 * Create the PanelHost and open every hostable panel.
 *
 * Separated from `bootEditorCore` so the handshake stays legible as the three-leg exchange it is,
 * and so a panel failure has one obvious place to be handled rather than being tangled into the
 * nonce logic. Like its caller it NEVER throws.
 */
async function startPanels(bridge: ShellBridge): Promise<PanelBringUp> {
    if (typeof document === "undefined") {
        return { mounted: 0, unavailable: [], error: "no document to mount into" };
    }
    const container = document.getElementById(EDITOR_ROOT_ID);
    if (container === null) {
        // The served document owns this element; its absence means the HTML and the bundle have
        // drifted apart, which is worth naming precisely rather than failing on a null deref.
        return { mounted: 0, unavailable: [], error: `no #${EDITOR_ROOT_ID} element in the document` };
    }
    try {
        const client = new PanelClient(bridge);
        const host = new PanelHost({ container, client });
        const report = await host.start();

        // --- layout persistence + region maps (e05d2) --------------------------------------------
        // The panels are up; now make the ARRANGEMENT durable. Restore the persisted layout + per-
        // panel D6 state OVER the defaults `start` just opened (a fresh project restores nothing and
        // the defaults stand), then start persisting future changes and publishing region maps on
        // every layout change. NEVER fatal: a persistence failure — a stale blob, a full disk — must
        // leave a working editor, so `ready` does NOT depend on it. `restore` and `attach` are called
        // only once the docking root is up (`report.started`); the LayoutPersistence stays reachable
        // through the Dockview + `pagehide` listeners `attach` registers, the same way `host` stays
        // reachable through the mounted DOM.
        if (report.started) {
            try {
                const stateClient = new EditorStateClient(bridge);
                const persistence = new LayoutPersistence({
                    panelHost: host,
                    panelClient: client,
                    stateClient,
                });
                const restoreReport = await persistence.restore();
                // Report the restore OUTCOME to the Shell (e05d4). The restart smoke asserts this is
                // `layoutRestored:false` on a fresh boot and `true` on a boot that reapplied a
                // persisted arrangement — the end-to-end proof that the arrangement round-tripped
                // through the Shell's editor-state store. Best-effort: `reportRestore` swallows a
                // Shell refusal, so it can never keep `attach` from running below.
                await stateClient.reportRestore(restoreReport);
                persistence.attach();
                // e05d4 restart-smoke seam: only when the boot URL carries `?ctx-smoke-arrange`,
                // perform ONE deterministic docking change so the arrangement that gets persisted
                // differs from the fresh-boot default — which is what makes the restart proof
                // meaningful. A no-op with no flag, so nothing in the shipping editor is affected.
                applySmokeArrangement(host);
            } catch {
                // Reported by absence of persistence, never fatal — the editor is up and usable.
            }
        }

        return { mounted: report.mounted, unavailable: report.unavailable, error: report.error };
    } catch (error) {
        return {
            mounted: 0,
            unavailable: [],
            error: error instanceof Error ? error.message : String(error),
        };
    }
}

/**
 * The M9 e05d4 restart-smoke seam — a NO-OP unless the boot URL carries `?ctx-smoke-arrange`.
 *
 * The restart smoke needs the FIRST boot to persist a REAL, non-default arrangement so its restart
 * proof is not indistinguishable from a fresh boot. There is no command registry yet to drive a dock
 * change from the Shell (design 09 §1 makes T2 command-driven; that arrives with e06), so a URL flag
 * is the v1 seam. It closes the LAST docked panel — a deterministic dock-arrangement change Dockview
 * serialises distinctly and restores cleanly — which fires `onDidLayoutChange`, so LayoutPersistence
 * publishes the new arrangement (debounced) with no further prompting.
 *
 * Guarded three ways so it is inert in the shipping editor: it requires the explicit flag, it needs
 * more than one panel (never empties the layout), and it is total — any failure just leaves the
 * default arrangement, which the smoke would then catch as a missing non-default publish.
 */
function applySmokeArrangement(host: PanelHost): void {
    if (typeof location === "undefined" || !location.search.includes("ctx-smoke-arrange")) {
        return;
    }
    const mounted = host.mounted;
    const last = mounted.length > 1 ? mounted[mounted.length - 1] : undefined;
    if (last !== undefined) {
        host.close(last);
    }
}
