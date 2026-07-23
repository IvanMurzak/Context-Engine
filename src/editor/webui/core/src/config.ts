// The per-user CONFIG surface, JS side (M9 e06d, design 06 §4 / C-F14 / C-F22).
//
// This is the TS mirror of `src/editor/shell/include/context/editor/shell/user_config.h`, and the ONE
// module in editor-core that names the `config.*` wire methods — `tools/check_config_writers.py`
// (ctest `editor-shell-config-writers`) enforces exactly that, so the request shape and its handling
// have a single definition rather than a copy per caller.
//
// ⚠ EDITOR-CORE DOES NOT PERSIST. It READS the document over `config.get` and REQUESTS changes over
// `config.set`; the Shell validates and writes (C-F14). There is no localStorage here, no sessionStorage,
// no file API — and that absence is gated, not merely intended: the renderer having its own store is
// not a shortcut, it is a SECOND source of truth for the same user choice, and the first time the two
// disagree there is no principled way to say which is right. The same rule editorstate.ts states for
// `.editor/editor-state.json` (C-F3), applied to the per-user document.
//
// EVERY PARSER IS TOTAL, like panels.ts: the Shell is trusted, but a malformed or partial envelope
// still degrades to a documented default rather than surfacing later as `undefined` in the DOM.
//
// WHAT "FIRST RUN" MEANS (06 §4 / C-F22), because it is easy to state backwards: a persisted choice
// WINS when it is present and resolvable; otherwise the editor follows `prefers-color-scheme`, and
// Dark when even that is undetectable. `startupThemeId` below is the single expression of that rule —
// it is pure, so both branches are provable in the T1 tier without an ambient host preference deciding
// the answer (which is exactly the false-positive mode that cost e06b two CI rounds).

import { BridgeError, isRecord, type ShellBridge } from "./bridge.js";
import { defaultThemeId, parsePinnedThemeId, type MediaQueryProbe } from "./theme.js";

// --------------------------------------------------------------------------- the wire vocabulary
// MUST match user_config.h's kConfig*Method / kConfigThemeKey. Cross-checked byte-for-byte out of the
// BUILT bundle by `tools/check_webui_assets.py --panel-contract` (ctest `webui-panel-contract`) — the
// same drift gate the panel / editor-state / keybindings / themes surfaces ride. A rename on one side
// would leave the theme switching for the session and never persisting, with nothing reporting it.

export const CONFIG_GET_METHOD = "config.get";
export const CONFIG_SET_METHOD = "config.set";

/** The one settable key (06 §4). The Shell refuses anything else with `config.unknown_key`. */
export const CONFIG_THEME_KEY = "theme";

/** The Shell's refusal codes, mirrored so a caller can branch on them rather than on message text. */
export const CONFIG_ERR_UNKNOWN_KEY = "config.unknown_key";
export const CONFIG_ERR_BAD_VALUE = "config.bad_value";
export const CONFIG_ERR_WRITE_FAILED = "config.write_failed";

// -------------------------------------------------------------------------------- the snapshot

/** The `config.get` result: the document plus the facts only the Shell can answer. */
export interface UserConfigSnapshot {
    /** Moves whenever the Shell observes a change to the document (its own write, or an external one). */
    readonly generation: number;
    /** False when no home directory resolved — the editor runs, but no choice can be remembered. */
    readonly writable: boolean;
    /** Absolute path of the config document, or `""` when there is none. Shown, never opened. */
    readonly path: string;
    /** Absolute path of `~/.context/keybindings.json` (e07c) — the file the Settings panel points at. */
    readonly keybindingsPath: string;
    /** The document itself. Read through the accessors below rather than indexed by callers. */
    readonly config: Readonly<Record<string, unknown>>;
}

/** The empty snapshot: what a Shell that does not serve `config.get` (an older build) yields. */
export const EMPTY_CONFIG_SNAPSHOT: UserConfigSnapshot = {
    generation: 0,
    writable: false,
    path: "",
    keybindingsPath: "",
    config: {},
};

/** Parse a `config.get` result. TOTAL: anything unreadable degrades to `EMPTY_CONFIG_SNAPSHOT`. */
export function parseConfigSnapshot(value: unknown): UserConfigSnapshot {
    if (!isRecord(value)) {
        return EMPTY_CONFIG_SNAPSHOT;
    }
    const generation = typeof value["generation"] === "number" ? value["generation"] : 0;
    const config = isRecord(value["config"]) ? value["config"] : {};
    return {
        generation,
        writable: value["writable"] === true,
        path: typeof value["path"] === "string" ? value["path"] : "",
        keybindingsPath:
            typeof value["keybindingsPath"] === "string" ? value["keybindingsPath"] : "",
        config,
    };
}

/**
 * The theme id the user has chosen, or `""` when none is recorded.
 *
 * A non-string member reads as "none recorded" rather than being coerced: a hand-edited config with
 * `"theme": 3` has expressed no theme, and stringifying it would invent a preference nobody stated.
 */
export function configuredThemeId(snapshot: UserConfigSnapshot): string {
    const value = snapshot.config[CONFIG_THEME_KEY];
    return typeof value === "string" ? value : "";
}

/**
 * THE startup theme rule (06 §4 / C-F22), in one place and pure.
 *
 * Order: an explicit `?ctx-smoke-theme` pin (the CEF smokes' determinism seam, theme.ts) > the
 * PERSISTED choice > `prefers-color-scheme` > Dark. The persisted id is honoured only when the registry
 * actually holds it — a config naming a user theme whose file has since been deleted must fall back to
 * the first-run rule, not leave the window unstyled (`ThemeEngine.apply` refuses an unknown id, and at
 * boot there is no previous theme for that refusal to fall back onto).
 */
export function startupThemeId(
    persistedThemeId: string,
    search: string,
    probe: MediaQueryProbe,
    isKnown: (id: string) => boolean,
): string {
    // The pin outranks the config ON PURPOSE: it exists so a live pixel test can CHOOSE the appearance
    // it asserts on, and a config left behind on a CI host must not be able to override that choice.
    // An unknown pin falls through rather than being applied, exactly as `bootThemeId` documents.
    const pinned = parsePinnedThemeId(search);
    if (pinned !== "" && isKnown(pinned)) {
        return pinned;
    }
    if (persistedThemeId !== "" && isKnown(persistedThemeId)) {
        return persistedThemeId;
    }
    return defaultThemeId(probe);
}

// --------------------------------------------------------------------------------- the client

/** What a `config.set` request did. A refusal is RETURNED, not thrown — it is an ordinary outcome. */
export interface ConfigWriteResult {
    readonly stored: boolean;
    /** `""` on success; the Shell's refusal code otherwise. */
    readonly code: string;
    /** `""` on success; the Shell's human/AI-readable reason otherwise. */
    readonly diagnostic: string;
    /** The document generation after the write (0 when it was refused). */
    readonly generation: number;
}

/**
 * The thin typed client over `config.get` / `config.set`.
 *
 * Holds NO state: the document lives in the Shell (C-F14), so a cache here would be a second source of
 * truth free to disagree with it — the same reason `ThemesClient` caches nothing. A bridge refusal
 * degrades to the empty snapshot / a reported failure rather than throwing: a Shell that does not serve
 * these methods (an older build, or a smoke's minimal router) must leave the editor booting on its
 * first-run defaults, never break boot.
 */
export class ConfigClient {
    readonly #bridge: ShellBridge;

    constructor(bridge: ShellBridge) {
        this.#bridge = bridge;
    }

    async get(): Promise<UserConfigSnapshot> {
        try {
            return parseConfigSnapshot(await this.#bridge.call(CONFIG_GET_METHOD));
        } catch (error) {
            if (error instanceof BridgeError) {
                return EMPTY_CONFIG_SNAPSHOT;
            }
            throw error;
        }
    }

    /**
     * Ask the Shell to persist a theme choice.
     *
     * The theme is applied LOCALLY first by the caller and persisted here — deliberately in that order.
     * The switch is what the user asked for and must be instant; persistence is a durability concern
     * that may honestly fail (a read-only home, no home at all), and making the visible switch wait on
     * the filesystem would turn a working editor into a stuttering one for no benefit.
     */
    async setTheme(themeId: string): Promise<ConfigWriteResult> {
        return this.#set(CONFIG_THEME_KEY, themeId);
    }

    async #set(key: string, value: unknown): Promise<ConfigWriteResult> {
        try {
            const result = await this.#bridge.call(CONFIG_SET_METHOD, { key, value });
            if (!isRecord(result)) {
                return {
                    stored: false,
                    code: "",
                    diagnostic: "the Shell returned an unreadable config.set result",
                    generation: 0,
                };
            }
            return {
                stored: result["stored"] === true,
                code: "",
                diagnostic: "",
                generation: typeof result["generation"] === "number" ? result["generation"] : 0,
            };
        } catch (error) {
            if (error instanceof BridgeError) {
                return {
                    stored: false,
                    code: error.reason,
                    diagnostic: error.message,
                    generation: 0,
                };
            }
            throw error;
        }
    }
}
