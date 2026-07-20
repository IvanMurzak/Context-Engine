// The privileged Shell bridge, JS side (M9 e05c, design 04 §1 / 08 §1).
//
// This is editor-core's ONLY way out. It has no daemon socket and no attach token — the Shell holds
// both — so every call to the daemon AND every call to the Shell's own state (window registry, drag
// sessions, region maps) travels through here. The CSP the assets are served under sets
// `connect-src 'none'`, so there is deliberately no `fetch`/WebSocket alternative: this channel or
// nothing.
//
// TRANSPORT. CEF's message router injects a query function onto `window` in every frame whose
// origin the browser side accepts. It is NOT a URL fetch — `BRIDGE_ENDPOINT` names the channel's
// identity (and is what the Shell's origin check is written against), not something we navigate to.
//
// ENVELOPE. JSON-RPC 2.0, mirroring the daemon's own shape, so e05d's client can layer on this
// without a translation step: request `{jsonrpc, id, method, params}`, response `{jsonrpc, id,
// result}` or `{jsonrpc, id, error:{code, message, data:{reason}}}`.

/** The scheme editor-core's assets and channel live under. Mirrors `kAppScheme` (C++). */
export const BRIDGE_SCHEME = "context-editor";

/** editor-core's own origin — the ONLY origin the Shell accepts a privileged query from. */
export const BRIDGE_ORIGIN = "context-editor://app";

/** The privileged channel's identity. Mirrors `kIpcEndpoint` (C++). */
export const BRIDGE_ENDPOINT = "context-editor://ipc";

/**
 * The name of the query function the Shell's message router injects onto `window`.
 *
 * Deliberately NOT CEF's default `cefQuery`: the default name is what every CEF sample and every
 * piece of drive-by injection probes for, and a distinctive name makes "is this the Context Shell"
 * answerable rather than guessable.
 *
 * MUST match `kBridgeQueryFunction` in src/editor/shell/cef/src/cef_shell.cpp — the
 * `webui-scheme-contract` gate re-checks that from the built asset (tools/check_webui_assets.py).
 */
export const BRIDGE_QUERY_FUNCTION = "contextEditorQuery";

/** The companion cancel function's name (CEF injects both or neither). */
export const BRIDGE_CANCEL_FUNCTION = "contextEditorQueryCancel";

/** The shape CEF's injected query function takes. */
export interface BridgeQuery {
    readonly request: string;
    readonly persistent: boolean;
    readonly onSuccess: (response: string) => void;
    readonly onFailure: (errorCode: number, errorMessage: string) => void;
}

/** The injected function itself; returns the query id (usable with the cancel function). */
export type BridgeQueryFunction = (query: BridgeQuery) => number;

/** The `error` member of a refused response. */
export interface BridgeErrorPayload {
    readonly code: number;
    readonly message: string;
    /** `data.reason` — the Shell's machine-readable classification (`bridge.unknown_method`, …). */
    readonly reason: string;
}

/**
 * A refused call.
 *
 * Carries the Shell's own `reason` rather than only a message, so a caller can branch on the class
 * of refusal without parsing prose — the same discipline the daemon's error catalog enforces one
 * layer down.
 */
export class BridgeError extends Error {
    readonly code: number;
    readonly reason: string;

    constructor(payload: BridgeErrorPayload) {
        super(payload.message);
        this.name = "BridgeError";
        this.code = payload.code;
        this.reason = payload.reason;
    }
}

export function isRecord(value: unknown): value is Record<string, unknown> {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}

/**
 * Read the injected query function off a global object.
 *
 * Returns `undefined` rather than throwing when it is absent: the bundle must stay loadable outside
 * the Shell (a plain browser, a test harness) instead of dying at import time with a stack trace
 * nobody can act on.
 */
export function detectBridgeQuery(scope: unknown = globalThis): BridgeQueryFunction | undefined {
    if (!isRecord(scope)) {
        return undefined;
    }
    const candidate = scope[BRIDGE_QUERY_FUNCTION];
    return typeof candidate === "function" ? (candidate as BridgeQueryFunction) : undefined;
}

/** The promise-based client over the injected query function. */
export class ShellBridge {
    readonly endpoint = BRIDGE_ENDPOINT;

    #query: BridgeQueryFunction;
    #nextId = 1;

    constructor(query: BridgeQueryFunction) {
        this.#query = query;
    }

    /**
     * Bind to the Shell if this document is running inside it.
     *
     * `undefined` outside the Shell — see `detectBridgeQuery` on why that is not an exception.
     */
    static detect(scope: unknown = globalThis): ShellBridge | undefined {
        const query = detectBridgeQuery(scope);
        return query === undefined ? undefined : new ShellBridge(query);
    }

    /** How many requests this client has sent (the next id, minus the 1 it starts at). */
    get sentCount(): number {
        return this.#nextId - 1;
    }

    /**
     * Call one bridge method.
     *
     * Resolves with the `result` member; rejects with a `BridgeError` for a refusal (either the
     * Shell's — a malformed envelope, an unknown or forbidden method — or the handler's own).
     */
    call(method: string, params: Record<string, unknown> = {}): Promise<unknown> {
        const id = this.#nextId++;
        const request = JSON.stringify({ jsonrpc: "2.0", id, method, params });

        return new Promise<unknown>((resolve, reject) => {
            this.#query({
                request,
                persistent: false,
                onSuccess: (response: string) => {
                    try {
                        resolve(this.#unwrap(response, id));
                    } catch (error) {
                        reject(error);
                    }
                },
                // The transport itself failed (the Shell refused the query outright — e.g. the
                // frame is not editor-core's origin). Distinct from a refusal INSIDE a delivered
                // envelope, and reported as such.
                onFailure: (errorCode: number, errorMessage: string) => {
                    reject(
                        new BridgeError({
                            code: errorCode,
                            message: errorMessage,
                            reason: "bridge.transport",
                        }),
                    );
                },
            });
        });
    }

    /**
     * Validate and destructure one response envelope.
     *
     * The Shell is trusted, but this still checks the shape: a silently-accepted malformed response
     * would surface later as an undefined field far from the cause.
     */
    #unwrap(response: string, expectedId: number): unknown {
        let parsed: unknown;
        try {
            parsed = JSON.parse(response);
        } catch {
            throw new BridgeError({
                code: -32700,
                message: "the Shell returned text that is not JSON",
                reason: "bridge.client_parse",
            });
        }
        if (!isRecord(parsed) || parsed["jsonrpc"] !== "2.0") {
            throw new BridgeError({
                code: -32600,
                message: "the Shell returned a non-JSON-RPC response",
                reason: "bridge.client_shape",
            });
        }
        // THE ERROR IS READ BEFORE THE ID, deliberately. The Shell cannot correlate a message it
        // could not parse, so `BridgeRouter::dispatch` answers `refuse(0, ...)` for its pre-id
        // refusal classes — `too_large` above all, which a well-formed caller reaches simply by
        // sending more than kMaxBridgeMessageBytes of params. Checking the id first turned every one
        // of those into `bridge.client_id_mismatch`, discarding the real code and the real reason:
        // exactly the diagnostic BridgeError exists to carry.
        const error = parsed["error"];
        if (isRecord(error)) {
            const data = error["data"];
            throw new BridgeError({
                code: typeof error["code"] === "number" ? error["code"] : -32603,
                message: typeof error["message"] === "string" ? error["message"] : "bridge refusal",
                reason:
                    isRecord(data) && typeof data["reason"] === "string"
                        ? data["reason"]
                        : "bridge.unknown",
            });
        }
        // A present-but-malformed `error` is a refusal too, and must never fall through to the
        // success path below — that would turn a refusal into `resolve(undefined)`.
        if (error !== undefined) {
            throw new BridgeError({
                code: -32603,
                message: "the Shell returned a malformed error member",
                reason: "bridge.client_shape",
            });
        }
        if (parsed["id"] !== expectedId) {
            throw new BridgeError({
                code: -32600,
                message: `response id ${String(parsed["id"])} does not match request ${expectedId}`,
                reason: "bridge.client_id_mismatch",
            });
        }
        // JSON-RPC 2.0 requires exactly one of `result` / `error`. Without this, a response carrying
        // neither resolves successfully with `undefined`, and the caller discovers the problem as a
        // missing field far from the cause — the very failure this method exists to prevent.
        if (!Object.prototype.hasOwnProperty.call(parsed, "result")) {
            throw new BridgeError({
                code: -32603,
                message: "the Shell returned neither a result nor an error",
                reason: "bridge.client_shape",
            });
        }
        return parsed["result"];
    }
}
