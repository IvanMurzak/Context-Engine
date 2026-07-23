// The two NOTIFICATION BANNERS, JS side (M9 e14d, design 07 §4 / 08 threat row).
//
//   1. THE UPDATE BANNER — notify-only (O3, owner-confirmed 2026-07-22). It reports what the SHELL
//      already found out and offers ONE action: open the downloads page. There is no in-app updater.
//   2. THE DAEMON-LOST BANNER — the read-only reconnect notice over e14a's reconnect state.
//
// ⚠ THIS MODULE MAKES NO NETWORK CALL, AND MUST NOT. The version check is a Shell responsibility for
// a PRIVACY reason, not a layering one: a renderer `fetch()` carries Chromium's own `User-Agent`
// (OS, CPU, browser build), `Accept-Language` (the user's locale) and client hints beneath the
// JavaScript — identifiers the 08 threat row forbids, that no assertion written here could see, and
// that no code written here could remove. So editor-core ASKS the Shell for the RESULT
// (`update.state`) and renders it. `tools/check_release_request.py` (ctest
// `editor-shell-release-request`) refuses any network primitive in this workspace, so that stays
// true after this file is forgotten.
//
// Two facts are load-bearing, exactly as in welcome.ts / panels.ts:
//   1. THE VOCABULARY IS CROSS-LANGUAGE AND GATED. The method + ownership constants below are
//      byte-compared against banners.h's C++ constants by `tools/check_webui_assets.py
//      --welcome-contract` (ctest `webui-welcome-contract`), read out of the BUILT bundle.
//   2. EVERY PARSER IS TOTAL. `parse*` returns `null` on anything it cannot read, and the caller
//      degrades honestly — an absent banner surface is an ordinary state, not an error.
//
// DOM ONLY, no `innerHTML`: every node is built with `createElement` + `textContent`, so a version
// string or a daemon error message off the wire can never inject markup into the trusted zone.

import type { ShellBridge } from "./bridge.js";
import { BridgeError, isRecord } from "./bridge.js";
import { createBadge, createButton } from "../../kit/src/index.js";

// --------------------------------------------------------------------------- the wire vocabulary
// MUST match banners.h's kUpdate*Method / kDaemonLinkStateMethod / kDaemonOwnership*. See note 1.

export const UPDATE_STATE_METHOD = "update.state";
export const UPDATE_DISMISS_METHOD = "update.dismiss";
export const UPDATE_OPEN_DOWNLOADS_METHOD = "update.openDownloads";
export const DAEMON_LINK_STATE_METHOD = "daemon.linkState";

/** How this process relates to the daemon it is talking to. Mirrors `kDaemonOwnership*`. */
export const DAEMON_OWNERSHIP_NONE = "none";
export const DAEMON_OWNERSHIP_OWNED = "owned";

// -------------------------------------------------------------------------------- the value types

/** What `update.state` reports (`shell::ReleaseNotice::state_json`). */
export interface UpdateState {
    /** True only when a check actually completed and produced a readable version. */
    readonly checked: boolean;
    /** The running version. Known LOCALLY — it is never sent anywhere (banners.h property (b)). */
    readonly current: string;
    /** The latest published release, or `""` when unknown. */
    readonly latest: string;
    /** True when a newer release exists AND the banner has not been dismissed this session. */
    readonly updateAvailable: boolean;
    readonly dismissed: boolean;
    readonly downloadsUrl: string;
    /** Why the check did not produce an answer, or `""`. */
    readonly error: string;
}

/** What `daemon.linkState` reports (`shell::DaemonLinkStatus`). */
export interface DaemonLinkState {
    /** True when the editor is NOT live read-write: no daemon yet, or a lost one mid-reconnect. */
    readonly readOnly: boolean;
    readonly reconnectAttempts: number;
    /** `none` / `external` / `owned`. */
    readonly ownership: string;
    readonly lastError: string;
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

/** Parse an `update.state` result. `null` when the envelope is not a readable object. */
export function parseUpdateState(value: unknown): UpdateState | null {
    if (!isRecord(value)) {
        return null;
    }
    return {
        checked: readBoolean(value, "checked"),
        current: readString(value, "current"),
        latest: readString(value, "latest"),
        updateAvailable: readBoolean(value, "updateAvailable"),
        dismissed: readBoolean(value, "dismissed"),
        downloadsUrl: readString(value, "downloadsUrl"),
        error: readString(value, "error"),
    };
}

/** Parse a `daemon.linkState` result. `null` when the envelope is not a readable object. */
export function parseDaemonLinkState(value: unknown): DaemonLinkState | null {
    if (!isRecord(value)) {
        return null;
    }
    return {
        readOnly: readBoolean(value, "readOnly"),
        reconnectAttempts: readNumber(value, "reconnectAttempts"),
        ownership: readString(value, "ownership", DAEMON_OWNERSHIP_NONE),
        lastError: readString(value, "lastError"),
    };
}

// ------------------------------------------------------------------------------- the client

/**
 * The typed client over the banner bridge methods.
 *
 * TOTAL, like `WelcomeClient`: a refusal (an absent banner surface answers `unknown_method`) comes
 * back as `null`, NOT as a throw — a build with no update channel and a smoke with no daemon must
 * both render an editor, not an error.
 */
export class BannerClient {
    readonly #bridge: ShellBridge;

    constructor(bridge: ShellBridge) {
        this.#bridge = bridge;
    }

    async updateState(): Promise<UpdateState | null> {
        return this.#call(UPDATE_STATE_METHOD, parseUpdateState);
    }

    async daemonLinkState(): Promise<DaemonLinkState | null> {
        return this.#call(DAEMON_LINK_STATE_METHOD, parseDaemonLinkState);
    }

    /** Hide the update banner for this session. Resolves false when the surface is absent. */
    async dismissUpdate(): Promise<boolean> {
        const result = await this.#call(UPDATE_DISMISS_METHOD, (value) =>
            isRecord(value) ? readBoolean(value, "dismissed") : null,
        );
        return result === true;
    }

    /** Open the downloads page. Resolves false when the Shell refused or the surface is absent. */
    async openDownloads(): Promise<boolean> {
        const result = await this.#call(UPDATE_OPEN_DOWNLOADS_METHOD, (value) =>
            isRecord(value) ? readBoolean(value, "opened") : null,
        );
        return result === true;
    }

    async #call<T>(method: string, parse: (value: unknown) => T | null): Promise<T | null> {
        try {
            return parse(await this.#bridge.call(method));
        } catch (error) {
            if (error instanceof BridgeError) {
                return null;
            }
            throw error;
        }
    }
}

// ------------------------------------------------------------------------------- the renderers

/** The class the banner strip carries — `app.css` styles it; the CONTROLS are the kit's. */
export const BANNER_STRIP_CLASS = "ctx-banners";
export const UPDATE_BANNER_CLASS = "ctx-banner ctx-banner--update";
export const DAEMON_BANNER_CLASS = "ctx-banner ctx-banner--daemon";

export interface UpdateBannerHandlers {
    /** Open the downloads page (the ONLY action — notify-only, O3: no in-app updater). */
    readonly onOpenDownloads: () => void;
    /** Hide the banner for this session. */
    readonly onDismiss: () => void;
}

function el(tag: string, className: string, text = ""): HTMLElement {
    const node = document.createElement(tag);
    if (className !== "") {
        node.className = className;
    }
    if (text !== "") {
        node.textContent = text;
    }
    return node;
}

/**
 * Render the update banner, or `null` when there is nothing to say.
 *
 * WHEN IT APPEARS: only when the Shell actually completed a check AND found a newer release AND the
 * user has not dismissed it. A failed check renders NOTHING here — a banner that said "we could not
 * check" on every offline launch would train users to ignore the one that matters. The failure is
 * still visible, in Settings § Updates, where a user who went looking is asking the question.
 */
export function renderUpdateBanner(
    state: UpdateState,
    handlers: UpdateBannerHandlers,
): HTMLElement | null {
    if (!state.checked || !state.updateAvailable || state.dismissed) {
        return null;
    }

    const root = el("div", UPDATE_BANNER_CLASS);
    // `status`, not `alert`: an available update is informational and must not interrupt a screen
    // reader mid-sentence (an `alert` is assertive by definition).
    root.setAttribute("role", "status");
    root.setAttribute("aria-live", "polite");
    root.setAttribute("data-update-available", "true");

    // `neutral`, deliberately: an available update is news, not a problem. Reserving the alarming
    // tones for states a user must act on is what keeps them meaningful.
    root.append(createBadge({ label: "Update", tone: "neutral" }).element);
    const message =
        state.latest !== ""
            ? `Context Editor ${state.latest} is available. You are running ${state.current}.`
            : "A newer Context Editor is available.";
    root.append(el("span", "ctx-banner__message", message));

    const actions = el("div", "ctx-banner__actions");
    actions.append(
        createButton({
            label: "Get the update",
            tone: "primary",
            onActivate: handlers.onOpenDownloads,
        }).element,
    );
    actions.append(
        createButton({ label: "Dismiss", tone: "default", onActivate: handlers.onDismiss }).element,
    );
    root.append(actions);
    return root;
}

/**
 * Render the daemon-lost banner, or `null` when the link is live.
 *
 * WHAT IT SAYS AND WHY. The user-visible consequence is that editing is disabled, so the banner
 * leads with THAT, not with "the daemon disconnected" — the daemon is an implementation detail and
 * "read-only" is the fact that changes what a user can do. The reconnect count is shown once the
 * ladder has actually retried, so a momentary blip does not flash a number nobody can act on.
 */
export function renderDaemonBanner(state: DaemonLinkState): HTMLElement | null {
    if (!state.readOnly) {
        return null;
    }

    const root = el("div", DAEMON_BANNER_CLASS);
    // `alert` here, unlike the update banner: losing write access changes what the next keystroke
    // will do, and a user must not discover that by having an edit silently not happen.
    root.setAttribute("role", "alert");
    root.setAttribute("aria-live", "assertive");
    root.setAttribute("data-read-only", "true");

    root.append(createBadge({ label: "Read-only", tone: "warn" }).element);
    // The two ownership cases are genuinely different situations for the user. An OWNED daemon is
    // this editor's own child: it will be respawned, and waiting is the right thing to do. An
    // EXTERNAL one belongs to somebody else (another editor, a CLI session), so this process can only
    // wait for it to come back — saying "reconnecting" there would imply an agency we do not have.
    const subject =
        state.ownership === DAEMON_OWNERSHIP_OWNED
            ? "Lost the project daemon — reconnecting"
            : "Lost the shared project daemon — waiting for it";
    const attempt = state.reconnectAttempts > 0 ? ` (attempt ${state.reconnectAttempts})` : "";
    root.append(
        el(
            "span",
            "ctx-banner__message",
            `${subject}${attempt}. Editing is disabled until it is back.`,
        ),
    );
    if (state.lastError !== "") {
        root.append(el("span", "ctx-banner__detail", state.lastError));
    }
    return root;
}

/** What `mountBanners` rendered — returned so a caller (and a test) can assert on it. */
export interface BannerMount {
    readonly element: HTMLElement;
    readonly updateShown: boolean;
    readonly daemonShown: boolean;
}

export interface BannerData {
    /** `null` when the Shell has no update surface (an `unknown_method` refusal). */
    readonly update: UpdateState | null;
    /** `null` when the Shell has no daemon-link surface. */
    readonly link: DaemonLinkState | null;
    readonly handlers: UpdateBannerHandlers;
}

/**
 * Render both banners into a strip and mount it in `container`, replacing its contents.
 *
 * The strip element is ALWAYS created (empty when neither banner applies) so a caller can place it
 * once and let it fill in later without a layout shift, and so a test has one stable handle.
 */
export function mountBanners(container: HTMLElement, data: BannerData): BannerMount {
    container.replaceChildren();
    const strip = el("div", BANNER_STRIP_CLASS);

    const daemon = data.link === null ? null : renderDaemonBanner(data.link);
    if (daemon !== null) {
        strip.append(daemon);
    }
    const update = data.update === null ? null : renderUpdateBanner(data.update, data.handlers);
    if (update !== null) {
        strip.append(update);
    }

    container.append(strip);
    return { element: strip, updateShown: update !== null, daemonShown: daemon !== null };
}
