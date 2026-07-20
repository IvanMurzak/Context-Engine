// editor-core's boot handshake (M9 e05c).
//
// e05c's job is the CHANNEL, not the app: this proves — inside the real e04 Shell window, over the
// real `context-editor://` scheme, under the real CSP — that the bundle loaded and that the bridge
// round-trips native<->JS. The PanelHost, the hydration runtime and layout persistence are e05d's;
// deliberately none of that is here.
//
// The `editor-cef-smoke-shell` ctest asserts the native side of this exchange, which is what makes
// it an end-to-end proof rather than two halves that were each tested alone:
//
//   1. JS  -> native   `shell.hello`  — "the bundle executed and can reach the Shell"
//   2. native -> JS    the reply       — carries a nonce only the Shell knows
//   3. JS  -> native   `shell.ready`  — echoes that nonce back
//
// Step 3 is the load-bearing one: the native side only accepts a `shell.ready` whose nonce matches
// the one it just minted, so the sequence cannot pass unless a value made the FULL round trip
// through the renderer. A one-way "JS called us" ping would pass with a broken response path.

import { BridgeError, ShellBridge, isRecord } from "./bridge.js";
import { editorCoreInfo } from "./info.js";

/** What `bootEditorCore` did — returned so a caller (and a test) can assert on it. */
export interface BootReport {
    /** False when the bundle is running outside the Shell (no injected query function). */
    readonly attached: boolean;
    /** True once the full JS -> native -> JS -> native handshake completed. */
    readonly ready: boolean;
    /** Populated when the handshake failed; empty otherwise. */
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
        return { attached: false, ready: false, error: "" };
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
            return { attached: true, ready: false, error: "shell.hello returned no nonce" };
        }

        await bridge.call("shell.ready", { nonce });
        markDocument("ready", "");
        return { attached: true, ready: true, error: "" };
    } catch (error) {
        const detail =
            error instanceof BridgeError
                ? `${error.reason}: ${error.message}`
                : error instanceof Error
                  ? error.message
                  : String(error);
        markDocument("error", detail);
        return { attached: true, ready: false, error: detail };
    }
}
