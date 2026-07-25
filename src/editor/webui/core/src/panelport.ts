// The THIRD-PARTY PANEL PORT — one authenticated `MessagePort` per iframe panel, and the versioned
// envelope over it (M9 e13b-1, design 04 §5 / 08 §1-§3, parent DoD "Bridge auth" / B-F6).
//
// WHAT THIS MODULE IS FOR. e13a-2 landed the iframe HOST and deliberately installed NO `message`
// listener, because listening before an authenticated transport existed would mean accepting
// unauthenticated postMessages from untrusted code for no capability in return. This is that
// transport. It ships ZERO capability: every verb answers the same versioned refusal, so the thing
// under review here is the AUTHENTICATION and the LIFECYCLE, not a feature.
//
// ⚠ THE PROBLEM, AND WHY THE OBVIOUS ANSWERS DO NOT WORK. `ext_scheme.h` records the obligation as
// "key off the handshake's verified `event.origin`". That control does not exist:
//
//   * `event.origin` is the literal string `"null"` for EVERY panel, because `IFRAME_SANDBOX`
//     (extpanel.ts) never carries `allow-same-origin` — the opaque origin that isolates packages from
//     each other is the same reason none of them can be named. One string shared by all discriminates
//     nothing.
//   * `event.source` / `frame.contentWindow` identity is NOT a document identity either. An iframe's
//     `WindowProxy` is STABLE ACROSS SAME-SLOT NAVIGATIONS (a web-platform property), so a message
//     from the third document loaded into a frame compares EQUAL to `frame.contentWindow` exactly as
//     the first document's does. This is the trap the obligation warns about, and reaching for
//     `event.source` is how one would walk straight into it while believing it closed.
//   * A URL-borne secret (`?nonce=…`) is READABLE BY THE PACKAGE'S OWN CODE (`location.search`), so a
//     malicious panel can copy it onto another package's URL and navigate there. It authenticates the
//     URL, which is not the thing in question.
//
// ⚠ WHAT DOES WORK — a SHELL-INJECTED, ALWAYS-FIRST BOOTSTRAP, and this module is only half of it.
// The Shell splices `<script src="/.context-panel-port.js">` ahead of everything a panel document
// carries and serves that script itself (`ext_inject_port_bootstrap` / `ext_port_bootstrap_script`).
// An external CLASSIC script blocks parsing until it runs, so it executes before ANY script the
// package wrote — which means the FIRST handshake a frame ever emits comes from the document the host
// navigated to, and a package cannot decline to emit one in order to leave the grant unclaimed for a
// document it navigates to later. THE CHILD creates the `MessageChannel` and transfers a port UP; the
// host never posts a port DOWN. That direction is load-bearing: a host that replied to a handshake by
// posting a port to `event.source` would be naming that stable WindowProxy, and a navigation
// completing between the handshake and the reply would drop the port into the NEW document. Here the
// port the host keeps is entangled with one that only ever existed in the handshaking document's
// realm, so that document's navigation kills it and no other realm can obtain it (`MessagePort` is not
// structured-cloneable, and `frame-src`/`child-src`/`worker-src 'none'` leave a panel no second realm).
//
// THIS MODULE SUPPLIES THE TWO REMAINING HALVES:
//   * THE ONE-SHOT. The first conforming handshake per frame is accepted and every later one is
//     REFUSED, with the latch never cleared. This is what makes "our script runs first" load-bearing
//     rather than merely true: without it, a second document could simply ask again.
//   * REVOCATION ON RE-NAVIGATION. A second `load` event on the frame element closes the port. The
//     first `load` is the entry document's own completion (the bootstrap has already run and posted by
//     then — it is a head script, `load` fires after parsing), so a second one can only mean another
//     document took the slot.
//
// ⚠ WHAT IS NOT CLAIMED (design 09 §3). This binds the port to the FIRST DOCUMENT LOADED INTO THE
// FRAME. It does not prove to the host WHICH PACKAGE that document belonged to — nothing in the
// browser can, since the host chose the URL and the Shell served exactly that URL, making the link a
// Shell/editor-core agreement rather than a browser-verified fact. It is recorded as such rather than
// described as authentication of the package.
//
// ⚠ NOT `IframeThemeChannel` (theme.ts), and the two must not be merged by reflex. That one is an
// UNAUTHENTICATED BROADCAST of non-secret theme tokens to every mounted frame — a materially easier
// problem, and its `targetOrigin: "*"` is correct there for the same opaque-origin reason. What is
// reused here is that ONE conclusion (a panel's origin cannot be targeted); what is NOT reused is its
// model, because a per-instance capability grant broadcast to every frame is not a grant at all.

/**
 * The handshake message's `ctx` discriminator. MIRRORS C++ `kExtPortHandshakeTag` (ext_scheme.h) and
 * is byte-compared against it by `tools/check_webui_assets.py --scheme-contract` (ctest
 * `webui-scheme-contract`) — the same cross-language gate the scheme vocabulary rides.
 *
 * THE VERSION IS INSIDE THE STRING ON PURPOSE: the contract gate reads STRING constants, so a
 * separate numeric version could drift between the Shell's injected script and this check with the
 * gate none the wiser — and that failure is total and silent (every handshake refused, every panel
 * portless). Folding it in makes a version bump a rename the gate cannot miss.
 */
export const EXT_PORT_HANDSHAKE_TAG = "context.panel-port.v1";

/**
 * The envelope discriminator for everything that travels ON the port, once granted.
 *
 * DISTINCT FROM the handshake tag above, deliberately. The handshake is a `window` message (the only
 * thing a document can send before it holds a port) and its tag is a cross-language constant the
 * SHELL emits; this one is the PACKAGE-facing wire format and both ends of it are reached only over
 * the port. Sharing one tag would make "arrived on the window" and "arrived on the port" spellings of
 * the same thing, and the whole point of the one-shot is that they are not.
 */
export const PANEL_BRIDGE_TAG = "context.panel-bridge";

/** The envelope version. Bumped only with a documented migration; every reply echoes it. */
export const PANEL_BRIDGE_VERSION = 1;

/**
 * The CLOSED refusal-code set. Frozen and exported because the SHAPE of a refusal is this task's
 * deliverable: e13b-2 (`bridge.commands.*`, `bridge.ui.subscribe`), e13c (`bridge.call`,
 * `bridge.events.subscribe`) and e13d (`bridge.theme.tokens`, `bridge.state.get/set`) each fill verbs
 * into the table below, and none of them should have to renegotiate what a refused call looks like.
 *
 * `verb_not_granted` is the DENY-ALL answer and the only one a well-formed request can get today. The
 * rest name a fault in the request itself or in the port's state, so a panel author can tell "you may
 * not do that" apart from "you asked wrongly" and from "your port is gone".
 */
export const PANEL_BRIDGE_REFUSALS = Object.freeze({
    /** A well-formed request for a verb this build grants nothing for. THE e13b-1 answer. */
    verbNotGranted: "bridge.verb_not_granted",
    /** The envelope was addressed and correlatable but structurally invalid. */
    malformedRequest: "bridge.malformed_request",
    /** `v` was present and was not `PANEL_BRIDGE_VERSION`. */
    unsupportedVersion: "bridge.unsupported_version",
    /** The port was revoked (re-navigation, dispose, or an unanswered handshake window). */
    portRevoked: "bridge.port_revoked",
    /** A host-side call made before any port was granted. */
    portUnavailable: "bridge.port_unavailable",
    /** A host-side call whose reply did not arrive inside its budget. */
    timeout: "bridge.timeout",
} as const);

/**
 * How long the grant window stays open after the frame is pointed at the package.
 *
 * GENEROUS ON PURPOSE, and bounded on purpose. The bootstrap is the document's FIRST script and posts
 * within milliseconds of the navigation starting, so a real panel never comes close; the bound exists
 * so a frame that never produced a handshake (a refused package, a non-HTML entry, a document that
 * failed to parse) does not leave a grant claimable indefinitely by whatever eventually lands in that
 * slot. Closing it costs a healthy panel nothing and removes an unbounded window.
 */
export const PANEL_PORT_HANDSHAKE_TIMEOUT_MS = 15_000;

/** Default budget for a HOST-issued request. A panel that does not answer must not pin a pending entry. */
export const PANEL_BRIDGE_REQUEST_TIMEOUT_MS = 5_000;

/** The longest `id` / `verb` this side will correlate or echo. An untrusted peer chooses both. */
export const PANEL_BRIDGE_TOKEN_MAX_LENGTH = 128;

/**
 * The frame element's port-state attribute — a DIAGNOSTIC, mirroring `data-panel-unavailable`'s role
 * in panelhost.ts.
 *
 * It is also what makes the panelhost WIRING assertable at the T1 tier: nothing local can load a
 * `context-ext://` document, so "the renderer really built a bridge, before `src`, and really tore it
 * down on dispose" is otherwise provable only by the CI-only live smoke. Reflecting the state onto the
 * rendered element is the same move `extpanel.ts` makes with `isSandboxSafe` — assert what the DOM
 * says, not what a constant says.
 */
export const PANEL_PORT_STATE_ATTRIBUTE = "data-panel-port";

/** The port lifecycle, as reflected onto the frame element. */
export type PanelPortState = "pending" | "granted" | "revoked";

/** One reply, as the host produces it and as a host-issued `request` resolves to. */
export interface PanelBridgeReply {
    readonly ok: boolean;
    /** Present exactly when `ok` is false. */
    readonly error?: PanelBridgeError;
    /** Present exactly when `ok` is true. Nothing in e13b-1 produces one. */
    readonly result?: unknown;
}

export interface PanelBridgeError {
    /** One of `PANEL_BRIDGE_REFUSALS`. */
    readonly code: string;
    /** The verb the refusal is about, when the request named a usable one. Bounded — see below. */
    readonly verb?: string;
    /** Human-readable, never machine-parsed. Carries no host state. */
    readonly message: string;
}

/**
 * A verb handler. EMPTY in production for e13b-1 — the table exists so the deny-all answer comes from
 * a LOOKUP MISS rather than from a hardcoded refusal, which is what lets a T1 case prove the table is
 * consulted at all (a hardcoded refusal passes every negative test vacuously).
 */
export type PanelVerbHandler = (params: unknown) => unknown | Promise<unknown>;

export interface PanelPortBridgeOptions {
    /** The panel frame. Listeners are installed on it IMMEDIATELY — construct before setting `src`. */
    readonly frame: HTMLIFrameElement;
    readonly panelId: string;
    /**
     * The window whose `message` events carry handshakes. Injected only so a T1 case can drive a
     * bridge without racing the shared page; production passes nothing.
     */
    readonly hostWindow?: Window;
    /** The verb table. Absent / empty ⇒ every verb is refused, which is e13b-1's whole surface. */
    readonly verbs?: ReadonlyMap<string, PanelVerbHandler>;
    readonly handshakeTimeoutMs?: number;
    readonly requestTimeoutMs?: number;
}

/** Counters the T1 tier asserts on — every one of them a REFUSAL this class made, or a load it saw. */
export interface PanelPortStats {
    /** `load` events seen on the frame element. The 2nd is what revokes. */
    readonly loads: number;
    /**
     * HANDSHAKE ATTEMPTS from our frame that were refused — a message claiming the handshake tag, or
     * transferring a port under any name. Ordinary panel chatter is ignored rather than counted, so
     * this stays a count of grants attempted rather than of messages sent.
     */
    readonly refusedHandshakes: number;
    /** Port envelopes refused (bad shape, bad version, ungranted verb). */
    readonly refusedRequests: number;
    /** Replies actually posted back down the port. */
    readonly repliesSent: number;
    /** Host-issued requests still awaiting a reply. */
    readonly pending: number;
}

interface PendingRequest {
    readonly resolve: (reply: PanelBridgeReply) => void;
    readonly timer: ReturnType<typeof setTimeout>;
}

/**
 * ONE panel frame's port: its grant, its lifetime, and the envelope over it.
 *
 * NEVER THROWS out of a listener and never rejects a promise — the same total, fail-closed contract
 * every parser in `panels.ts` follows, and for a sharper reason here: the peer is untrusted, so a
 * throw escaping a `message` handler would be an unhandled rejection an attacker can trigger at will.
 */
export class PanelPortBridge {
    readonly #frame: HTMLIFrameElement;
    readonly #panelId: string;
    readonly #verbs: ReadonlyMap<string, PanelVerbHandler>;
    readonly #requestTimeoutMs: number;
    readonly #onMessage: (event: MessageEvent) => void;
    readonly #onLoad: () => void;
    readonly #pending = new Map<string, PendingRequest>();
    readonly #waiters: ((granted: boolean) => void)[] = [];
    #hostWindow: Window | undefined;
    #port: MessagePort | undefined;
    /** ONE-WAY LATCH, exactly like `IframePanelRenderer.#frame`: set on the grant and NEVER cleared. */
    #granted = false;
    #revoked = false;
    #loads = 0;
    #refusedHandshakes = 0;
    #refusedRequests = 0;
    #repliesSent = 0;
    #nextRequestId = 0;
    #handshakeTimer: ReturnType<typeof setTimeout> | undefined;

    constructor(options: PanelPortBridgeOptions) {
        this.#frame = options.frame;
        this.#panelId = options.panelId;
        this.#verbs = options.verbs ?? new Map<string, PanelVerbHandler>();
        this.#requestTimeoutMs = options.requestTimeoutMs ?? PANEL_BRIDGE_REQUEST_TIMEOUT_MS;
        this.#hostWindow = options.hostWindow ?? window;
        this.#onMessage = (event: MessageEvent): void => {
            this.#handleWindowMessage(event);
        };
        this.#onLoad = (): void => {
            this.#handleLoad();
        };
        // INSTALLED IN THE CONSTRUCTOR, which is why the constructor must run before `src` is set.
        // The bootstrap posts its handshake from a HEAD script, i.e. potentially before the caller's
        // next statement completes; a listener attached afterwards would miss the one grant a frame
        // ever offers and the panel would be silently portless.
        this.#hostWindow.addEventListener("message", this.#onMessage);
        this.#frame.addEventListener("load", this.#onLoad);
        this.#reflect("pending");
        this.#handshakeTimer = setTimeout(() => {
            // The grant window closed with nothing offered. Revoking (rather than waiting forever)
            // is what bounds it — see PANEL_PORT_HANDSHAKE_TIMEOUT_MS.
            this.#revoke();
        }, options.handshakeTimeoutMs ?? PANEL_PORT_HANDSHAKE_TIMEOUT_MS);
    }

    get panelId(): string {
        return this.#panelId;
    }

    /** Has a port been granted? Stays TRUE after revocation — it is a latch, not a liveness flag. */
    get granted(): boolean {
        return this.#granted;
    }

    get revoked(): boolean {
        return this.#revoked;
    }

    /** The live port, or `undefined` before a grant and after a revocation. */
    get port(): MessagePort | undefined {
        return this.#port;
    }

    get state(): PanelPortState {
        if (this.#revoked) {
            return "revoked";
        }
        return this.#granted ? "granted" : "pending";
    }

    get stats(): PanelPortStats {
        return {
            loads: this.#loads,
            refusedHandshakes: this.#refusedHandshakes,
            refusedRequests: this.#refusedRequests,
            repliesSent: this.#repliesSent,
            pending: this.#pending.size,
        };
    }

    /**
     * Resolve when the port is granted (`true`) or the grant window closes without one (`false`).
     *
     * The seam a caller reports a portless panel from. Resolves IMMEDIATELY when the outcome is
     * already known, so it is safe to await at any point in the lifecycle.
     */
    waitForPort(): Promise<boolean> {
        if (this.#port !== undefined) {
            return Promise.resolve(true);
        }
        if (this.#revoked) {
            return Promise.resolve(false);
        }
        return new Promise<boolean>((resolve) => {
            this.#waiters.push(resolve);
        });
    }

    /**
     * Issue a HOST-side request and await the panel's reply.
     *
     * Nothing in e13b-1 calls this — stated plainly rather than dressed up, exactly as
     * `PanelHost.refreshAll`'s own doc does. It is here because the DoD requires request/reply
     * CORRELATION and a TIMEOUT to be settled now, and a correlation layer with no requester on
     * either side would be untestable; the panel-to-host direction exercises correlation, and this
     * direction is the only one that can exercise a timeout at all (the host answers synchronously, so
     * a panel's request never waits). e13b-2 is its first production consumer.
     *
     * NEVER REJECTS: a timeout, a revoked port and a missing port are all REPLIES, because a caller in
     * a renderer whose only diagnostic channel is a DOM attribute cannot handle a rejection usefully.
     */
    request(verb: string, params?: unknown): Promise<PanelBridgeReply> {
        const port = this.#port;
        if (port === undefined) {
            return Promise.resolve(
                refusal(
                    this.#revoked
                        ? PANEL_BRIDGE_REFUSALS.portRevoked
                        : PANEL_BRIDGE_REFUSALS.portUnavailable,
                    verb,
                    "no live panel port",
                ),
            );
        }
        this.#nextRequestId += 1;
        const id = `h${String(this.#nextRequestId)}`;
        return new Promise<PanelBridgeReply>((resolve) => {
            const timer = setTimeout(() => {
                this.#pending.delete(id);
                resolve(refusal(PANEL_BRIDGE_REFUSALS.timeout, verb, "the panel did not reply"));
            }, this.#requestTimeoutMs);
            this.#pending.set(id, { resolve, timer });
            try {
                port.postMessage({
                    ctx: PANEL_BRIDGE_TAG,
                    v: PANEL_BRIDGE_VERSION,
                    kind: "request",
                    id,
                    verb,
                    params,
                });
            } catch {
                // A detached / closed port throws on post. Settle it here rather than waiting out the
                // budget for a reply that provably cannot come.
                clearTimeout(timer);
                this.#pending.delete(id);
                resolve(refusal(PANEL_BRIDGE_REFUSALS.portRevoked, verb, "the panel port is closed"));
            }
        });
    }

    /**
     * Tear the bridge down. Idempotent, and the ONLY caller is the renderer's `dispose`.
     *
     * Revokes first (which closes the port and settles everything pending), then detaches the
     * listeners. Order matters: detaching first would leave a closing port able to deliver one more
     * message into a handler that believes it is still live.
     */
    dispose(): void {
        this.#revoke();
        this.#hostWindow?.removeEventListener("message", this.#onMessage);
        this.#frame.removeEventListener("load", this.#onLoad);
        this.#hostWindow = undefined;
    }

    // ------------------------------------------------------------------------------------ internals

    /**
     * A `load` event on the frame ELEMENT — the revocation signal.
     *
     * WHY COUNTING, AND WHY THE SECOND. `load` fires on the element for a cross-origin document too
     * (that is the classic cross-origin load-completion signal), but it carries no identity, so the
     * only usable fact is HOW MANY have happened. Load #1 is the entry document's own completion: the
     * bootstrap is a head script, so its handshake was posted during parsing, strictly before this
     * event. A load #2 therefore cannot be the entry document's — another document has taken the slot,
     * which is precisely the `ext_scheme.h` hazard, so the port dies here.
     *
     * ⚠ THIS RULE ASSUMES NO `about:blank` LOAD IS COUNTED. A frame created with `src` set BEFORE
     * insertion does not fire one — MEASURED in the T1 tier against a real browser rather than assumed
     * from the spec (`panelport.test.ts` § the load-count rule), because an extra initial load would
     * silently revoke every healthy panel on its own first navigation.
     */
    #handleLoad(): void {
        this.#loads += 1;
        if (this.#loads >= 2) {
            this.#revoke();
        }
    }

    #handleWindowMessage(event: MessageEvent): void {
        if (this.#hostWindow === undefined) {
            return;
        }
        // NOT OUR FRAME ⇒ SILENT, and not counted. The editor's window carries messages from every
        // panel, from `IframeThemeChannel`'s own traffic and from anything else on the page; treating
        // those as refusals would make every bridge's counters a function of every other panel's
        // behaviour, and one panel could then mask another's refusals by generating noise.
        if (event.source === null || event.source !== this.#frame.contentWindow) {
            return;
        }
        const data = event.data;
        // IS THIS EVEN A HANDSHAKE ATTEMPT? Two independent tells, and either is enough: it claims our
        // tag, or it TRANSFERS a port. A message that does neither is ordinary panel-to-editor chatter
        // and is IGNORED — not counted, not answered.
        //
        // The port tell is the load-bearing half. Keying only on the tag would let a message that
        // transfers a port under some OTHER name pass unremarked, and transferring a port to the editor
        // is unambiguously an attempt to establish a channel whatever it calls itself — so it must be
        // refused (and its ports closed) like any other. Keying on NEITHER — counting every message
        // from the frame, which is what this did first — makes `refusedHandshakes` a function of how
        // chatty the panel is rather than of how many grants it tried to obtain, and the counter's
        // whole job is to make the one-shot observable.
        const attemptsHandshake =
            (isPlainObject(data) && data["ctx"] === EXT_PORT_HANDSHAKE_TAG) || event.ports.length > 0;
        if (!attemptsHandshake) {
            return;
        }
        // THE FOUR GATES, all of which must hold. Each is independent, and the ORDER is chosen so a
        // refused handshake never has a side effect: the latch checks come before the shape checks,
        // so a replayed handshake cannot be distinguished from a malformed one by whether its ports
        // were closed.
        const conforming =
            !this.#revoked &&
            !this.#granted &&
            isPlainObject(data) &&
            data["ctx"] === EXT_PORT_HANDSHAKE_TAG &&
            event.ports.length === 1;
        if (!conforming) {
            this.#refusedHandshakes += 1;
            // CLOSE whatever was transferred. A refused port left open is an entangled channel the
            // sender believes it holds, and closing it makes the refusal observable at the peer
            // instead of looking like a message that vanished.
            for (const port of event.ports) {
                port.close();
            }
            return;
        }
        // THE ONE-SHOT. Latched BEFORE anything else can fail, so no path leads to a second grant.
        this.#granted = true;
        const port = event.ports[0] as MessagePort;
        this.#port = port;
        port.onmessage = (portEvent: MessageEvent): void => {
            this.#handlePortMessage(portEvent);
        };
        // Explicit `start()` even though assigning `onmessage` implies it: the implicit form is a
        // property of the assignment, so a later refactor to `addEventListener` would silently stop
        // the port and queue every message forever.
        port.start();
        this.#clearHandshakeTimer();
        this.#reflect("granted");
        this.#settleWaiters(true);
    }

    /**
     * One envelope off the port. The peer is untrusted third-party code, so EVERY field is validated
     * and nothing is echoed back that was not first bounded.
     */
    #handlePortMessage(event: MessageEvent): void {
        const data = event.data;
        if (!isPlainObject(data) || data["ctx"] !== PANEL_BRIDGE_TAG) {
            // Unaddressed noise. Counted but never answered: replying would turn the port into an
            // echo for anything a panel cares to send.
            this.#refusedRequests += 1;
            return;
        }
        const kind = data["kind"];
        if (kind === "reply") {
            this.#resolveReply(data);
            return;
        }
        if (kind !== "request") {
            this.#refusedRequests += 1;
            return;
        }
        const id = data["id"];
        if (!isToken(id)) {
            // UNCORRELATABLE, so there is nowhere to send a refusal — a reply with no id is noise the
            // panel could not match either. Counted and dropped.
            this.#refusedRequests += 1;
            return;
        }
        if (data["v"] !== PANEL_BRIDGE_VERSION) {
            this.#refuse(id, PANEL_BRIDGE_REFUSALS.unsupportedVersion, undefined,
                `unsupported envelope version; this build speaks v${String(PANEL_BRIDGE_VERSION)}`);
            return;
        }
        const verb = data["verb"];
        if (!isToken(verb)) {
            this.#refuse(id, PANEL_BRIDGE_REFUSALS.malformedRequest, undefined,
                "a request must carry a `verb` string");
            return;
        }
        const handler = this.#verbs.get(verb);
        if (handler === undefined) {
            // THE e13b-1 ANSWER, and the only one a well-formed request can get: the table is EMPTY in
            // production, so this is a lookup MISS rather than a hardcoded refusal. A T1 case installs
            // a handler to prove the difference — a hardcoded refusal would pass every negative test.
            this.#refuse(id, PANEL_BRIDGE_REFUSALS.verbNotGranted, verb,
                "this verb is not granted to package panels in this build");
            return;
        }
        void this.#invoke(id, verb, handler, data["params"]);
    }

    async #invoke(id: string, verb: string, handler: PanelVerbHandler,
                  params: unknown): Promise<void> {
        try {
            const result = await handler(params);
            this.#reply(id, { ok: true, result });
        } catch {
            // A handler that threw is a HOST fault, and its message is host state — refused with the
            // generic code and no detail rather than leaking a stack into untrusted code.
            this.#reply(id, refusal(PANEL_BRIDGE_REFUSALS.malformedRequest, verb,
                "the verb handler failed"));
        }
    }

    #resolveReply(data: Record<string, unknown>): void {
        const id = data["id"];
        if (!isToken(id)) {
            this.#refusedRequests += 1;
            return;
        }
        const entry = this.#pending.get(id);
        if (entry === undefined) {
            // A reply to nothing — a stale reply after a timeout, or a panel inventing ids. Neither is
            // actionable.
            this.#refusedRequests += 1;
            return;
        }
        this.#pending.delete(id);
        clearTimeout(entry.timer);
        entry.resolve(
            data["ok"] === true
                ? { ok: true, result: data["result"] }
                : refusal(PANEL_BRIDGE_REFUSALS.malformedRequest, undefined,
                    "the panel refused or answered unreadably"),
        );
    }

    #refuse(id: string, code: string, verb: string | undefined, message: string): void {
        this.#refusedRequests += 1;
        this.#reply(id, refusal(code, verb, message));
    }

    #reply(id: string, reply: PanelBridgeReply): void {
        const port = this.#port;
        if (port === undefined) {
            return;
        }
        try {
            port.postMessage({
                ctx: PANEL_BRIDGE_TAG,
                v: PANEL_BRIDGE_VERSION,
                kind: "reply",
                id,
                ...reply,
            });
            this.#repliesSent += 1;
        } catch {
            // A closed port throws; there is no one left to tell.
        }
    }

    /**
     * Close the grant for good. Idempotent, and `#granted` is deliberately LEFT SET so a handshake
     * arriving afterwards is refused by BOTH latches rather than by whichever one happens to run.
     */
    #revoke(): void {
        if (this.#revoked) {
            return;
        }
        this.#revoked = true;
        this.#clearHandshakeTimer();
        const port = this.#port;
        this.#port = undefined;
        try {
            port?.close();
        } catch {
            // Already dead; nothing to do.
        }
        for (const [, entry] of this.#pending) {
            clearTimeout(entry.timer);
            entry.resolve(
                refusal(PANEL_BRIDGE_REFUSALS.portRevoked, undefined, "the panel port was revoked"),
            );
        }
        this.#pending.clear();
        this.#reflect("revoked");
        this.#settleWaiters(false);
    }

    #clearHandshakeTimer(): void {
        if (this.#handshakeTimer !== undefined) {
            clearTimeout(this.#handshakeTimer);
            this.#handshakeTimer = undefined;
        }
    }

    #settleWaiters(granted: boolean): void {
        const waiters = this.#waiters.splice(0, this.#waiters.length);
        for (const waiter of waiters) {
            waiter(granted);
        }
    }

    #reflect(state: PanelPortState): void {
        this.#frame.setAttribute(PANEL_PORT_STATE_ATTRIBUTE, state);
    }
}

// ------------------------------------------------------------------------------------- internals

function refusal(code: string, verb: string | undefined, message: string): PanelBridgeReply {
    return { ok: false, error: verb === undefined ? { code, message } : { code, verb, message } };
}

function isPlainObject(value: unknown): value is Record<string, unknown> {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}

/**
 * A bounded, non-empty string — the shape both `id` and `verb` must have.
 *
 * BOUNDED because an untrusted peer chooses both and each is ECHOED in a refusal: an unbounded verb
 * would be an unbounded attacker-chosen string travelling back out of the host, which is the same
 * amplification `clamp_logged_url` refuses on the native side. Refusing rather than truncating keeps
 * the echoed value byte-identical to the one that was sent.
 */
function isToken(value: unknown): value is string {
    return (
        typeof value === "string" &&
        value.length > 0 &&
        value.length <= PANEL_BRIDGE_TOKEN_MAX_LENGTH
    );
}
