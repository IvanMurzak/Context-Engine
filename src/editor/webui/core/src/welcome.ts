// The `welcome.*` bridge surface + the welcome-screen renderer, JS side (M9 e14c, design 07 §4 / 10 /
// D13). editor-core's front door: on a BARE launch the Shell reports `mode: "welcome"` and this module
// renders the mini-welcome screen — recent projects, "Open project…" (a native folder picker via the
// Shell), and "New from template" (a thin wrapper over `context new`). Every action goes back over the
// SAME privileged bridge; nothing here has a daemon socket or knows a project path it was not handed.
//
// Two facts are load-bearing, exactly as in panels.ts:
//   1. THE VOCABULARY IS CROSS-LANGUAGE AND GATED. The method + mode constants below are byte-compared
//      against welcome.h's C++ constants by `tools/check_webui_assets.py --welcome-contract` (ctest
//      `webui-welcome-contract`), read out of the BUILT bundle — a rename on either side reds a ctest
//      instead of silently unbinding the welcome surface.
//   2. EVERY PARSER IS TOTAL. A response is validated structurally; `parse*` returns `null` on anything
//      it cannot read, and the caller degrades honestly.
//
// 🚫 NO SIGNATURE FLOURISH on the welcome CTA (O1, resolved 2026-07-19): the "Pulse of Work" flourish is
// state-linked and meaningful only on the Play/active-action button, which the welcome CTA is not — so it
// uses ORDINARY primary-button styling (`.welcome-cta`), never the aurora/flourish.

import type { ShellBridge } from "./bridge.js";
import { BridgeError, isRecord } from "./bridge.js";

// --------------------------------------------------------------------------- the wire vocabulary
// MUST match welcome.h's kWelcome*Method / kWelcomeMode*. See note 1 above.

export const WELCOME_STATE_METHOD = "welcome.state";
export const WELCOME_PICK_FOLDER_METHOD = "welcome.pickFolder";
export const WELCOME_OPEN_METHOD = "welcome.open";
export const WELCOME_NEW_PROJECT_METHOD = "welcome.newProject";

/** The two launch modes `welcome.state` reports. Mirrors `kWelcomeMode*` (welcome.h). */
export const WELCOME_MODE_WELCOME = "welcome";
export const WELCOME_MODE_PROJECT = "project";

// -------------------------------------------------------------------------------- the value types

/** One recent project (`shell::RecentProject`). */
export interface RecentProject {
    readonly path: string;
    readonly name: string;
    readonly lastOpenedMs: number;
}

/** One "New from template" option (`shell::WelcomeTemplate`). */
export interface WelcomeTemplate {
    readonly name: string;
    readonly label: string;
    readonly description: string;
}

/** What `welcome.state` reports — everything the welcome screen renders. */
export interface WelcomeState {
    /** `"welcome"` (bare launch, show the front door) or `"project"` (open the editor). */
    readonly mode: string;
    readonly projectName: string;
    readonly recents: readonly RecentProject[];
    readonly templates: readonly WelcomeTemplate[];
}

/** The outcome of `welcome.pickFolder`. `picked:false` (cancel/unsupported) is an ordinary result. */
export interface PickFolderResult {
    readonly picked: boolean;
    readonly path: string;
}

/** The outcome of `welcome.open`. */
export interface OpenResult {
    readonly opened: boolean;
    /** The e14a launch-flow verdict (`spawn` / `focused` / …); empty when unavailable. */
    readonly action: string;
    readonly path: string;
}

/** The outcome of `welcome.newProject`. */
export interface NewProjectResult {
    readonly created: boolean;
    readonly runnable: boolean;
    readonly directory: string;
    readonly template: string;
    readonly opened: boolean;
    readonly action: string;
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

/** Parse one recent-project entry. `null` when it carries no usable path. */
export function parseRecentProject(value: unknown): RecentProject | null {
    if (!isRecord(value)) {
        return null;
    }
    const path = readString(value, "path");
    if (path === "") {
        return null;
    }
    return {
        path,
        name: readString(value, "name", path),
        lastOpenedMs: readNumber(value, "lastOpenedMs"),
    };
}

/** Parse one template entry. `null` when it carries no name. */
export function parseWelcomeTemplate(value: unknown): WelcomeTemplate | null {
    if (!isRecord(value)) {
        return null;
    }
    const name = readString(value, "name");
    if (name === "") {
        return null;
    }
    return {
        name,
        label: readString(value, "label", name),
        description: readString(value, "description"),
    };
}

/** Parse a `welcome.state` result. `null` when the envelope carries no readable mode. */
export function parseWelcomeState(value: unknown): WelcomeState | null {
    if (!isRecord(value)) {
        return null;
    }
    const mode = readString(value, "mode");
    if (mode === "") {
        return null;
    }
    const recents: RecentProject[] = [];
    const rawRecents = value["recents"];
    if (Array.isArray(rawRecents)) {
        for (const entry of rawRecents) {
            const parsed = parseRecentProject(entry);
            if (parsed !== null) {
                recents.push(parsed);
            }
        }
    }
    const templates: WelcomeTemplate[] = [];
    const rawTemplates = value["templates"];
    if (Array.isArray(rawTemplates)) {
        for (const entry of rawTemplates) {
            const parsed = parseWelcomeTemplate(entry);
            if (parsed !== null) {
                templates.push(parsed);
            }
        }
    }
    return { mode, projectName: readString(value, "projectName"), recents, templates };
}

export function parsePickFolderResult(value: unknown): PickFolderResult | null {
    if (!isRecord(value)) {
        return null;
    }
    return { picked: readBoolean(value, "picked"), path: readString(value, "path") };
}

export function parseOpenResult(value: unknown): OpenResult | null {
    if (!isRecord(value)) {
        return null;
    }
    return {
        opened: readBoolean(value, "opened"),
        action: readString(value, "action"),
        path: readString(value, "path"),
    };
}

export function parseNewProjectResult(value: unknown): NewProjectResult | null {
    if (!isRecord(value)) {
        return null;
    }
    return {
        created: readBoolean(value, "created"),
        runnable: readBoolean(value, "runnable"),
        directory: readString(value, "directory"),
        template: readString(value, "template"),
        opened: readBoolean(value, "opened"),
        action: readString(value, "action"),
    };
}

// ------------------------------------------------------------------------------- the client

/**
 * The typed client over the `welcome.*` bridge methods.
 *
 * Thin, and TOTAL: a refusal (an absent welcome surface answers `unknown_method`) is returned as `null`,
 * NOT thrown — that is exactly how `boot.ts` distinguishes "no welcome surface, open the editor" from a
 * transport failure. A transport failure still rejects.
 */
export class WelcomeClient {
    readonly #bridge: ShellBridge;

    constructor(bridge: ShellBridge) {
        this.#bridge = bridge;
    }

    /** The welcome state. `null` when the surface is absent (an `unknown_method` refusal) or unreadable. */
    async state(): Promise<WelcomeState | null> {
        try {
            return parseWelcomeState(await this.#bridge.call(WELCOME_STATE_METHOD));
        } catch (error) {
            if (error instanceof BridgeError) {
                return null;
            }
            throw error;
        }
    }

    /** Invoke the native folder picker. `null` on a refusal. */
    async pickFolder(): Promise<PickFolderResult | null> {
        try {
            return parsePickFolderResult(await this.#bridge.call(WELCOME_PICK_FOLDER_METHOD));
        } catch (error) {
            if (error instanceof BridgeError) {
                return null;
            }
            throw error;
        }
    }

    /** Open `path` as a project (feeds the e14a launch flow). `null` on a refusal (e.g. an empty path). */
    async open(path: string): Promise<OpenResult | null> {
        try {
            return parseOpenResult(await this.#bridge.call(WELCOME_OPEN_METHOD, { path }));
        } catch (error) {
            if (error instanceof BridgeError) {
                return null;
            }
            throw error;
        }
    }

    /** Scaffold `directory` from `template`, then open it. `null` on a refusal. */
    async newProject(directory: string, template: string): Promise<NewProjectResult | null> {
        try {
            return parseNewProjectResult(
                await this.#bridge.call(WELCOME_NEW_PROJECT_METHOD, { directory, template }),
            );
        } catch (error) {
            if (error instanceof BridgeError) {
                return null;
            }
            throw error;
        }
    }
}

// ------------------------------------------------------------------------------- the renderer

/** The element id the welcome screen mounts into (the same `#editor-root` PanelHost uses). */
export const WELCOME_ROOT_CLASS = "welcome-screen";

/** What `mountWelcome` did — returned so a caller (and a test) can assert on it. */
export interface WelcomeMount {
    /** The number of recent-project rows rendered. */
    readonly recentCount: number;
    /** The number of template chips rendered. */
    readonly templateCount: number;
    /** The root element the welcome screen was rendered into. */
    readonly root: HTMLElement;
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
 * Render the welcome screen into `container`, wiring its three actions to `client`.
 *
 * DOM ONLY, no `innerHTML`: every node is built with `createElement` + `textContent`, so a recent
 * project's on-disk name can never inject markup into the trusted editor-core zone. The CTA carries
 * ordinary primary styling (`.welcome-cta`) — NO flourish (O1). Returns a report for the tests.
 */
export function mountWelcome(
    bridge: ShellBridge,
    container: HTMLElement,
    state: WelcomeState,
): WelcomeMount {
    const client = new WelcomeClient(bridge);
    container.replaceChildren();

    // Run the native folder picker and return the chosen absolute path, or null on cancel / empty path.
    const pickFolderPath = async (): Promise<string | null> => {
        const picked = await client.pickFolder();
        return picked !== null && picked.picked && picked.path !== "" ? picked.path : null;
    };

    const root = el("div", WELCOME_ROOT_CLASS);
    root.setAttribute("role", "region");
    root.setAttribute("aria-label", "Welcome");

    // --- left: recent projects (persona A "open recent" = 2 steps) -------------------------------
    const recentPane = el("div", "welcome-recent");
    const recentHeader = el("div", "welcome-recent-header");
    recentHeader.appendChild(el("div", "welcome-eyebrow", "Recent"));
    recentHeader.appendChild(el("h1", "welcome-recent-title", "Jump back in"));
    recentPane.appendChild(recentHeader);

    const recentList = el("ul", "welcome-recent-list");
    recentList.setAttribute("role", "list");
    for (const recent of state.recents) {
        const row = document.createElement("li");
        row.className = "welcome-recent-row";
        row.setAttribute("role", "button");
        row.setAttribute("tabindex", "0");
        row.setAttribute("data-recent-path", recent.path);
        row.appendChild(el("div", "welcome-recent-name", recent.name));
        row.appendChild(el("div", "welcome-recent-path", recent.path));
        const activate = (): void => {
            void client.open(recent.path);
        };
        row.addEventListener("click", activate);
        // key-handler-ok: Enter/Space ARIA activation of this `role="button"` recent-project row —
        // fires the SAME `activate()` (open the project) as the click above, never a global chord
        // bypassing the keymap/command path (05 §6). Window-0 front-door chrome, not an editor panel.
        row.addEventListener("keydown", (event: KeyboardEvent) => {
            if (event.key === "Enter" || event.key === " ") {
                event.preventDefault();
                activate();
            }
        });
        recentList.appendChild(row);
    }
    if (state.recents.length === 0) {
        recentList.appendChild(el("li", "welcome-recent-empty", "No recent projects yet."));
    }
    recentPane.appendChild(recentList);

    // "Open project…" — the native folder picker, then open the picked directory.
    const openFolder = document.createElement("button");
    openFolder.type = "button";
    openFolder.className = "welcome-open-folder";
    openFolder.textContent = "Open project…";
    openFolder.addEventListener("click", () => {
        void (async () => {
            const path = await pickFolderPath();
            if (path !== null) {
                await client.open(path);
            }
        })();
    });
    const recentFooter = el("div", "welcome-recent-footer");
    recentFooter.appendChild(openFolder);
    recentPane.appendChild(recentFooter);

    // --- right: hero + new-from-template ---------------------------------------------------------
    const hero = el("div", "welcome-hero");
    hero.appendChild(el("div", "welcome-mark"));
    hero.appendChild(el("h2", "welcome-title", "Context Editor"));
    hero.appendChild(
        el(
            "p",
            "welcome-tagline",
            "An ordinary client of your project's daemon — dockable panels, live viewports, and a package-extensible workspace. No project loaded yet.",
        ),
    );

    const startFromTemplate = (template: string): void => {
        void (async () => {
            const path = await pickFolderPath();
            if (path !== null) {
                await client.newProject(path, template);
            }
        })();
    };

    // The primary CTA — ORDINARY primary styling, NO flourish (O1). Uses the first template as the
    // default "New from template" action; the chips below pick a specific one.
    const defaultTemplate = state.templates[0]?.name ?? "default";
    const cta = document.createElement("button");
    cta.type = "button";
    cta.className = "welcome-cta";
    cta.textContent = "New from Template";
    cta.addEventListener("click", () => startFromTemplate(defaultTemplate));
    hero.appendChild(cta);

    const chipRow = el("div", "welcome-template-row");
    for (const template of state.templates) {
        const chip = document.createElement("button");
        chip.type = "button";
        chip.className = "welcome-template-chip";
        chip.setAttribute("data-template", template.name);
        chip.appendChild(el("span", "welcome-template-label", template.label));
        chip.title = template.description;
        chip.addEventListener("click", () => startFromTemplate(template.name));
        chipRow.appendChild(chip);
    }
    hero.appendChild(chipRow);

    root.appendChild(recentPane);
    root.appendChild(hero);
    container.appendChild(root);

    return { recentCount: state.recents.length, templateCount: state.templates.length, root };
}
