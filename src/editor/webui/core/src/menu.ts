// The APPLICATION MENU (editor-window-chrome d3, menu structure 03 / target design 02 §4): ONE
// declarative menu model in editor-core, rendered two ways — a web menubar inside the titlebar
// strip on Windows/Linux (`system` mode's strip IS this menubar; `custom` hosts it beside the
// caption), and the native global NSMenu bar on macOS, fed from the SAME model over `menu.publish`
// (window.ts). Four facts are load-bearing:
//
//   1. NO SECOND DISPATCH SYSTEM EXISTS (03's core rule). Every item names a command id in the ONE
//      e07b registry: a web-menubar activation calls the injected `executeCommand` (the late-bound
//      registry, the titlebar palette button's exact pattern), and a native NSMenu activation comes
//      back as an `editor.ui.menu` fact over the EXISTING mirror relay (uibus.ts `UI_TOPIC_MENU`,
//      C++ menu_facts.h), which `subscribeMenuFacts` below routes into the SAME registry. The menu
//      never invents a handler; the commands it names either exist here (`menuCommands`) or exist
//      already (undo/palette/theme/panel/play — the 03 table's "existing" column).
//
//   2. THE TREE IS EXACTLY 03's TABLE. App (macOS only) / File / Edit / View / Selection / Panel /
//      Window / Help, with the per-item backing as written. Items whose backing is future work
//      render DISABLED with the reason in their tooltip — the ⏳ rows: Cut/Copy/Paste outside
//      `textInputFocus` (app-level clipboard is future; inside a text field they delegate to the
//      browser's native editing). Play/pause/stop deliberately have NO menu (03: they live in the
//      d1 strip and the palette). Enablement rides the existing when-context (when.ts) — an item
//      is DISABLED, never hidden, so the menu's shape cannot flicker with focus (03's rule).
//
//   3. THE ACCELERATOR COLUMN DISPLAYS `DEFAULT_KEYBINDINGS` (01 §7). It shows the chord where a
//      binding exists and does NOT imply the web keymap is globally wired (it is not — the e07c
//      resolver seam stays untouched); the ONE place a displayed chord is also live is macOS,
//      where the NSMenu key equivalents the Shell builds from these same strings ride the same
//      activation return path as a click.
//
//   4. DROPDOWNS ARE APP-CHROME OVERLAYS IN THE PALETTE'S PATTERN (03: palette_view.ts precedent,
//      the app.css z-order stack): CSP-clean `createElement` DOM, classes declared here and styled
//      in app.css from existing tokens, ARIA `menubar`/`menu`/`menuitem` with full keyboard nav —
//      arrows/Enter/Escape/Home/End. Alt-mnemonics are DEFERRED, recorded here: nothing binds Alt,
//      and underlined access keys wait for the e07c keymap wiring they depend on. No new kit
//      family (the twelve stay closed); the About dialog's one control is a kit button.
//
// DOM ONLY, no `innerHTML`, exactly like banners.ts / chrome.ts: every node is built with
// `createElement` + `textContent`, so a recent project's on-disk name can never inject markup into
// the trusted zone.

import { createButton } from "../../kit/src/index.js";
import type { Command, CommandOutcome } from "./commands.js";
import { DEFAULT_KEYBINDINGS } from "./keymap.js";
import { PALETTE_TOGGLE_COMMAND_ID } from "./palette.js";
import { SETTINGS_PANEL_ID } from "./settings.js";
import type { SessionSelectSender } from "./session.js";
import type { UpdateState } from "./banners.js";
import { evaluateWhen, type WhenContext } from "./when.js";
import { isRecord } from "./bridge.js";
import { UI_TOPIC_MENU, type EditorUiBus, type EditorUiSubscription } from "./uibus.js";
import type { ChromeWindowControls } from "./chrome.js";
import {
    CHROME_MODE_HYBRID,
    type ChromeMode,
    type WindowControlResult,
} from "./window.js";
import type {
    NewProjectResult,
    OpenResult,
    PickFolderResult,
    RecentProject,
} from "./welcome.js";

// --------------------------------------------------------------------------- the new command ids
// The d3-built commands (03's table, "new" column). Grep-stable; every one is registered through
// `menuCommands` below and dispatched through the ONE registry like any palette command.

export const PROJECT_NEW_COMMAND_ID = "project.new";
export const PROJECT_OPEN_COMMAND_ID = "project.open";
/** One command per boot-time recent project: `project.openRecent.<index>`. */
export const PROJECT_OPEN_RECENT_PREFIX = "project.openRecent.";
export const EDIT_CUT_COMMAND_ID = "edit.cut";
export const EDIT_COPY_COMMAND_ID = "edit.copy";
export const EDIT_PASTE_COMMAND_ID = "edit.paste";
export const SELECTION_CLEAR_COMMAND_ID = "selection.clear";
export const VIEW_OPEN_SETTINGS_COMMAND_ID = "view.panel.open.settings";
export const VIEW_WINDOW_CLOSE_COMMAND_ID = "view.window.close";
export const WINDOW_QUIT_COMMAND_ID = "window.quit";
/** Deliberately spelled like their bridge verbs — 03's table names them so (02 §5's surfaces). */
export const WINDOW_MINIMIZE_COMMAND_ID = "window.minimize";
export const WINDOW_TOGGLE_MAXIMIZE_COMMAND_ID = "window.toggle-maximize";
/** One command per listed window: `window.focus.<windowId>`. */
export const WINDOW_FOCUS_COMMAND_PREFIX = "window.focus.";
export const HELP_DOCS_COMMAND_ID = "help.docs";
export const HELP_ABOUT_COMMAND_ID = "help.about";

/** The ⏳ tooltip (03's honest-degrade rule) for Cut/Copy/Paste outside a text field. */
export const CLIPBOARD_DISABLED_REASON =
    "App-level clipboard is future work — cut, copy and paste work inside a focused text field";
/** The generic disabled tooltip for an item whose guard is simply not met right now. */
export const MENU_ITEM_DISABLED_REASON = "Not available right now";
/** The tooltip on the placeholder entry an empty recents list renders. */
export const NO_RECENTS_REASON = "No recent projects yet";

// ------------------------------------------------------------------------------- the model types
// The AUTHORED model — built once at boot by `buildMenuModel`, rendered by `mountMenubar`, and
// serialized for `menu.publish` by `menuModelJson`. Every entry kind mirrors the C++ parse
// (menu_model.h): command / separator / submenu, nothing else.

/** The wire `type` tokens (menu_model.h kMenuItemType*). */
export const MENU_ITEM_TYPE_COMMAND = "command";
export const MENU_ITEM_TYPE_SEPARATOR = "separator";
export const MENU_ITEM_TYPE_SUBMENU = "submenu";

export interface MenuCommandEntry {
    readonly kind: "command";
    readonly commandId: string;
    readonly label: string;
    /** The DISPLAY chord from DEFAULT_KEYBINDINGS (`""` = none) — fact 3 above. */
    readonly accelerator: string;
    /** The applicability guard (when.ts), mirroring the backing command's own. `""` = always. */
    readonly when: string;
    /** The tooltip shown while DISABLED — the ⏳ reason. `""` renders the generic reason. */
    readonly disabledReason: string;
}

export interface MenuSeparatorEntry {
    readonly kind: "separator";
}

export interface MenuSubmenuEntry {
    readonly kind: "submenu";
    readonly label: string;
    readonly items: readonly MenuEntry[];
}

export type MenuEntry = MenuCommandEntry | MenuSeparatorEntry | MenuSubmenuEntry;

export interface MenuDefinition {
    /** Grep-stable (`"file"`, `"edit"`, …) — the DOM `data-menu-id` and the wire `id`. */
    readonly id: string;
    readonly label: string;
    readonly items: readonly MenuEntry[];
}

export interface MenuModel {
    readonly menus: readonly MenuDefinition[];
}

// ------------------------------------------------------------------------------- the accelerator

/**
 * The DISPLAY accelerator for a command id: the FIRST default binding that targets it (`""` when
 * none). Read from DEFAULT_KEYBINDINGS so the column can never drift from the shipped map — the
 * one rule 01 §7 pins: display only, no keymap-wiring implication.
 */
export function menuAcceleratorFor(commandId: string): string {
    const binding = DEFAULT_KEYBINDINGS.find((entry) => entry.command === commandId);
    return binding === undefined ? "" : binding.key;
}

// --------------------------------------------------------------------------------- the model

/** What `buildMenuModel` needs beyond the platform: the boot-time data the dynamic entries name. */
export interface MenuModelOptions {
    /** The chrome mode — `hybrid` (macOS) gets the App menu shape; everything else the Exit shape. */
    readonly mode: ChromeMode;
    /** Boot-time recents (`welcome.state`), in the Shell's order. */
    readonly recents: readonly RecentProject[];
    /** The live window ids (`window.list`), self included. */
    readonly windows: readonly number[];
    readonly selfWindowId: number;
}

function command(
    commandId: string,
    label: string,
    when = "",
    disabledReason = "",
): MenuCommandEntry {
    return {
        kind: "command",
        commandId,
        label,
        accelerator: menuAcceleratorFor(commandId),
        when,
        disabledReason,
    };
}

const SEPARATOR: MenuSeparatorEntry = { kind: "separator" };

/** The Open Recent submenu — one entry per recent, or the honest disabled placeholder. */
function openRecentSubmenu(recents: readonly RecentProject[]): MenuSubmenuEntry {
    if (recents.length === 0) {
        return {
            kind: "submenu",
            label: "Open Recent",
            items: [
                // Never registered, so it renders disabled (fact 2's honesty) with its reason.
                command("project.openRecent.none", "No Recent Projects", "", NO_RECENTS_REASON),
            ],
        };
    }
    return {
        kind: "submenu",
        label: "Open Recent",
        items: recents.map((recent, index) =>
            command(`${PROJECT_OPEN_RECENT_PREFIX}${String(index)}`, recent.name),
        ),
    };
}

/** The Window menu's window list — self first, peers after, each naming its focus command. */
function windowListEntries(
    windows: readonly number[],
    selfWindowId: number,
): readonly MenuCommandEntry[] {
    const ids = [...new Set<number>([selfWindowId, ...windows])].sort((a, b) => a - b);
    return ids.map((id) =>
        command(
            `${WINDOW_FOCUS_COMMAND_PREFIX}${String(id)}`,
            `Window ${String(id + 1)}${id === selfWindowId ? " (this window)" : ""}`,
        ),
    );
}

/**
 * Build the ONE menu model — exactly 03's table (fact 2), shaped by platform: `hybrid` (macOS)
 * carries the App menu (About / Settings… / Quit) and no File>Exit / Help>About; every other mode
 * carries Exit in File and About in Help. Every entry's `when` mirrors its backing command's, so
 * the menu and the palette can never disagree about applicability.
 */
export function buildMenuModel(options: MenuModelOptions): MenuModel {
    const mac = options.mode === CHROME_MODE_HYBRID;
    const menus: MenuDefinition[] = [];

    if (mac) {
        menus.push({
            id: "app",
            label: "Context Editor",
            items: [
                command(HELP_ABOUT_COMMAND_ID, "About Context Editor"),
                command(VIEW_OPEN_SETTINGS_COMMAND_ID, "Settings…"),
                SEPARATOR,
                command(WINDOW_QUIT_COMMAND_ID, "Quit Context Editor"),
            ],
        });
    }

    const fileItems: MenuEntry[] = [
        command(PROJECT_NEW_COMMAND_ID, "New Project…"),
        command(PROJECT_OPEN_COMMAND_ID, "Open Project…"),
        openRecentSubmenu(options.recents),
        SEPARATOR,
        command(VIEW_WINDOW_CLOSE_COMMAND_ID, "Close Window"),
    ];
    if (!mac) {
        fileItems.push(command(WINDOW_QUIT_COMMAND_ID, "Exit"));
    }
    menus.push({ id: "file", label: "File", items: fileItems });

    menus.push({
        id: "edit",
        label: "Edit",
        items: [
            command("session.undo", "Undo", "!textInputFocus"),
            command("session.redo", "Redo", "!textInputFocus"),
            SEPARATOR,
            // The ⏳ rows (03): enabled ONLY under textInputFocus, where they delegate to the
            // browser's native editing; disabled elsewhere with the reason in the tooltip.
            command(EDIT_CUT_COMMAND_ID, "Cut", "textInputFocus", CLIPBOARD_DISABLED_REASON),
            command(EDIT_COPY_COMMAND_ID, "Copy", "textInputFocus", CLIPBOARD_DISABLED_REASON),
            command(EDIT_PASTE_COMMAND_ID, "Paste", "textInputFocus", CLIPBOARD_DISABLED_REASON),
        ],
    });

    menus.push({
        id: "view",
        label: "View",
        items: [
            command(PALETTE_TOGGLE_COMMAND_ID, "Command Palette"),
            command("view.theme.toggle", "Toggle Theme"),
            SEPARATOR,
            // Honest-refusal implementations today (boot.ts) — the menu inherits that honesty (03).
            command("view.panel.focusNext", "Focus Next Panel", "!textInputFocus"),
            command("view.panel.focusPrevious", "Focus Previous Panel", "!textInputFocus"),
            command("view.panel.close", "Close Panel", "panelFocus && !textInputFocus"),
        ],
    });

    menus.push({
        id: "selection",
        label: "Selection",
        // The menu grows with e11 picking (03, recorded); today exactly the clear.
        items: [command(SELECTION_CLEAR_COMMAND_ID, "Clear Selection")],
    });

    menus.push({
        id: "panel",
        label: "Panel",
        items: [
            command("view.window.tearOut", "Tear Out Panel", "panelFocus && !textInputFocus"),
            command(
                "view.window.moveToPrimary",
                "Move Panel to Primary",
                "panelFocus && !textInputFocus",
            ),
            SEPARATOR,
            command("view.panel.move.left", "Move Panel Left", "panelFocus && !textInputFocus"),
            command("view.panel.move.right", "Move Panel Right", "panelFocus && !textInputFocus"),
            command("view.panel.move.up", "Move Panel Up", "panelFocus && !textInputFocus"),
            command("view.panel.move.down", "Move Panel Down", "panelFocus && !textInputFocus"),
        ],
    });

    menus.push({
        id: "window",
        label: "Window",
        items: [
            command(WINDOW_MINIMIZE_COMMAND_ID, "Minimize"),
            // The label flips to "Restore" on the maximized fact at RENDER/publish time — see
            // `toggleMaximizeLabel`; the model carries the resting label.
            command(WINDOW_TOGGLE_MAXIMIZE_COMMAND_ID, "Maximize"),
            SEPARATOR,
            ...windowListEntries(options.windows, options.selfWindowId),
        ],
    });

    const helpItems: MenuEntry[] = [command(HELP_DOCS_COMMAND_ID, "Documentation")];
    if (!mac) {
        helpItems.push(command(HELP_ABOUT_COMMAND_ID, "About Context Editor"));
    }
    menus.push({ id: "help", label: "Help", items: helpItems });

    return { menus };
}

/** The Maximize item's live label (02 §5: it flips on the `maximized` fact, never on a poll). */
export function toggleMaximizeLabel(maximized: boolean): string {
    return maximized ? "Restore" : "Maximize";
}

// ------------------------------------------------------------------------------- the enablement

/** How the renderer/serializer asks whether a command id currently resolves (the late-bound registry). */
export type CommandAvailability = (commandId: string) => boolean;

/**
 * One entry's enablement (fact 2): the backing command must EXIST in the registry (a window-list
 * entry for a closed window, the recents placeholder, a command layer still coming up — all read
 * as "not available") AND its `when` guard must hold in the CURRENT context — the same
 * `evaluateWhen` the palette filters with, so the two can never disagree.
 */
export function menuEntryEnabled(
    entry: MenuCommandEntry,
    context: WhenContext,
    available: CommandAvailability,
): boolean {
    return available(entry.commandId) && evaluateWhen(entry.when, context);
}

/** The tooltip a DISABLED entry carries — its declared reason, or the honest generic one. */
export function menuEntryDisabledTooltip(entry: MenuCommandEntry): string {
    return entry.disabledReason !== "" ? entry.disabledReason : MENU_ITEM_DISABLED_REASON;
}

// ------------------------------------------------------------------------------- the wire shape

/** What `menuModelJson` evaluates enablement/labels against — the publish-time snapshot. */
export interface MenuSerializeOptions {
    readonly context: WhenContext;
    readonly available: CommandAvailability;
    readonly maximized: boolean;
}

function entryJson(entry: MenuEntry, options: MenuSerializeOptions): Record<string, unknown> {
    if (entry.kind === "separator") {
        return { type: MENU_ITEM_TYPE_SEPARATOR };
    }
    if (entry.kind === "submenu") {
        return {
            type: MENU_ITEM_TYPE_SUBMENU,
            label: entry.label,
            items: entry.items.map((item) => entryJson(item, options)),
        };
    }
    const enabled = menuEntryEnabled(entry, options.context, options.available);
    const label =
        entry.commandId === WINDOW_TOGGLE_MAXIMIZE_COMMAND_ID
            ? toggleMaximizeLabel(options.maximized)
            : entry.label;
    const out: Record<string, unknown> = {
        type: MENU_ITEM_TYPE_COMMAND,
        id: entry.commandId,
        label,
        enabled,
    };
    if (entry.accelerator !== "") {
        out["accelerator"] = entry.accelerator;
    }
    if (!enabled) {
        out["tooltip"] = menuEntryDisabledTooltip(entry);
    }
    return out;
}

/**
 * Serialize the model for `menu.publish` (menu_model.h's wire shape). Enablement is the
 * PUBLISH-TIME snapshot — the honest scope of the native bar today: the NSMenu's enabled states
 * are as of the last publish, while the web menubar re-evaluates at every dropdown open. A
 * when-context-driven RE-PUBLISH is recorded future work, not pretended here.
 */
export function menuModelJson(
    model: MenuModel,
    options: MenuSerializeOptions,
): Record<string, unknown> {
    return {
        menus: model.menus.map((menu) => ({
            id: menu.id,
            label: menu.label,
            items: menu.items.map((item) => entryJson(item, options)),
        })),
    };
}

// ------------------------------------------------------------------------------- the actions

/** The `welcome.*` slice the project commands drive (WelcomeClient satisfies it structurally). */
export interface MenuProjectGateway {
    pickFolder(): Promise<PickFolderResult | null>;
    open(path: string): Promise<OpenResult | null>;
    newProject(directory: string, template: string): Promise<NewProjectResult | null>;
}

/** The window slice the window commands drive (WindowClient satisfies it structurally). */
export interface MenuWindowControls extends ChromeWindowControls {
    focus(windowId?: number): Promise<WindowControlResult>;
}

/** The actions the d3 commands dispatch to — an interface so the T1 tier drives them with spies. */
export interface MenuCommandActions {
    newProject(): Promise<CommandOutcome> | CommandOutcome;
    openProject(): Promise<CommandOutcome> | CommandOutcome;
    openRecent(path: string): Promise<CommandOutcome> | CommandOutcome;
    /** The browser-native editing delegation (the ⏳ rows' enabled half). */
    editClipboard(verb: "cut" | "copy" | "paste"): Promise<CommandOutcome> | CommandOutcome;
    clearSelection(): Promise<CommandOutcome> | CommandOutcome;
    openSettings(): Promise<CommandOutcome> | CommandOutcome;
    closeWindow(): Promise<CommandOutcome> | CommandOutcome;
    quit(): Promise<CommandOutcome> | CommandOutcome;
    minimizeWindow(): Promise<CommandOutcome> | CommandOutcome;
    toggleMaximizeWindow(): Promise<CommandOutcome> | CommandOutcome;
    focusWindow(windowId: number): Promise<CommandOutcome> | CommandOutcome;
    openDocs(): Promise<CommandOutcome> | CommandOutcome;
    openAbout(): Promise<CommandOutcome> | CommandOutcome;
}

/** What `makeMenuActions` composes over — every dependency injectable, the makePlayActions rule. */
export interface MenuActionDeps {
    readonly project: MenuProjectGateway;
    readonly windowControls: MenuWindowControls;
    readonly select: SessionSelectSender;
    /** Open a panel by id (late-bound over the PanelHost; false = no host / unknown id). */
    readonly openPanel: (panelId: string) => boolean;
    /** The Shell's native docs opener (BannerClient.openDocs; false = refused / absent). */
    readonly openDocs: () => Promise<boolean>;
    /** Show the About dialog (boot composes `openAboutDialog` over the live update state). */
    readonly showAbout: () => void;
    /** The default template `project.new` scaffolds from (the welcome CTA's own rule). */
    readonly defaultTemplate: string;
    /** The browser-native editing hook; defaults to `document.execCommand`. Injectable for tests. */
    readonly execEditCommand?: (verb: "cut" | "copy" | "paste") => boolean;
}

/** The live `document.execCommand` delegation — the ⏳ rows' enabled half (fact 2). */
function defaultExecEditCommand(verb: "cut" | "copy" | "paste"): boolean {
    if (typeof document === "undefined") {
        return false;
    }
    try {
        // Deprecated but UNIVERSALLY the in-page editing dispatch for cut/copy/paste — exactly the
        // "delegate to the browser's native editing" 03 names. A refusal (paste without permission,
        // nothing editable) reads false and the command reports it honestly.
        return document.execCommand(verb);
    } catch {
        return false;
    }
}

/**
 * The real `MenuCommandActions` — each command's effect over the injected gateways, extracted (and
 * exported) so the T1 tier drives THIS function against scripted deps rather than a copy of it
 * (the makeSessionActions rule). Every outcome is honest: a cancelled picker, a refused write and
 * an absent surface all read `ok:false` with a note naming why.
 */
export function makeMenuActions(deps: MenuActionDeps): MenuCommandActions {
    const exec = deps.execEditCommand ?? defaultExecEditCommand;
    const pickFolder = async (): Promise<string | null> => {
        const picked = await deps.project.pickFolder();
        return picked !== null && picked.picked && picked.path !== "" ? picked.path : null;
    };
    return {
        newProject: async (): Promise<CommandOutcome> => {
            const path = await pickFolder();
            if (path === null) {
                return { ok: false, note: "no folder picked" };
            }
            const result = await deps.project.newProject(path, deps.defaultTemplate);
            return result !== null && result.created
                ? { ok: true, note: `created ${result.directory} from ${result.template}` }
                : { ok: false, note: "project creation refused (no welcome surface, or the CLI failed)" };
        },
        openProject: async (): Promise<CommandOutcome> => {
            const path = await pickFolder();
            if (path === null) {
                return { ok: false, note: "no folder picked" };
            }
            const result = await deps.project.open(path);
            return result !== null && result.opened
                ? { ok: true, note: `opened ${result.path} (${result.action})` }
                : { ok: false, note: "open refused (no welcome surface behind this Shell)" };
        },
        openRecent: async (path: string): Promise<CommandOutcome> => {
            const result = await deps.project.open(path);
            return result !== null && result.opened
                ? { ok: true, note: `opened ${result.path} (${result.action})` }
                : { ok: false, note: "open refused (no welcome surface behind this Shell)" };
        },
        editClipboard: (verb): CommandOutcome =>
            exec(verb)
                ? { ok: true, note: `${verb} delegated to the browser's native editing` }
                : { ok: false, note: `the browser refused ${verb} (no editable focus?)` },
        clearSelection: async (): Promise<CommandOutcome> => {
            const report = await deps.select.select([]);
            if (!report.served) {
                return { ok: false, note: `session.select unavailable: ${report.diagnostic}` };
            }
            return report.applied
                ? { ok: true, note: "selection cleared" }
                : { ok: false, note: "clear refused (no live daemon session?)" };
        },
        openSettings: (): CommandOutcome =>
            deps.openPanel(SETTINGS_PANEL_ID)
                ? { ok: true, note: "Settings opened" }
                : { ok: false, note: "no panel host to open Settings in (welcome screen?)" },
        closeWindow: async (): Promise<CommandOutcome> => {
            // `window.close` carries the primary-vs-secondary policy Shell-side (window_bridge) —
            // the ASK is the outcome; a primary close tears the app down under us, so nothing
            // meaningful can be read out of the reply.
            await deps.windowControls.close();
            return { ok: true, note: "window close requested" };
        },
        quit: async (): Promise<CommandOutcome> => {
            // The primary-window close path IS the quit (03's table: window_bridge close policy).
            await deps.windowControls.close();
            return { ok: true, note: "quit requested (primary-window close policy)" };
        },
        minimizeWindow: async (): Promise<CommandOutcome> => {
            // `ChromeWindowControls.minimize` is typed `Promise<unknown>` (the a2 interface keeps
            // the loose shape its spies use), so the reply is read structurally here.
            const result = await deps.windowControls.minimize();
            return isRecord(result) && result["accepted"] === true
                ? { ok: true, note: "minimized" }
                : { ok: false, note: "minimize refused (no OS window behind this Shell)" };
        },
        toggleMaximizeWindow: async (): Promise<CommandOutcome> => {
            const result = await deps.windowControls.toggleMaximize();
            return result.accepted
                ? { ok: true, note: result.maximized ? "maximized" : "restored" }
                : { ok: false, note: "maximize toggle refused (no OS window behind this Shell)" };
        },
        focusWindow: async (windowId: number): Promise<CommandOutcome> => {
            const result = await deps.windowControls.focus(windowId);
            return result.accepted
                ? { ok: true, note: `focused window ${String(windowId)}` }
                : { ok: false, note: `window ${String(windowId)} is not a live window` };
        },
        openDocs: async (): Promise<CommandOutcome> =>
            (await deps.openDocs())
                ? { ok: true, note: "documentation opened in the browser" }
                : { ok: false, note: "the Shell has no URL opener on this platform yet" },
        openAbout: (): CommandOutcome => {
            deps.showAbout();
            return { ok: true, note: "about dialog opened" };
        },
    };
}

/** What `menuCommands` needs beyond the actions: the boot data the per-entry commands close over. */
export interface MenuCommandData {
    readonly recents: readonly RecentProject[];
    readonly windows: readonly number[];
    readonly selfWindowId: number;
}

/**
 * The d3 command set (03's table, "new" column) — registered through `buildCommandRegistry`'s
 * `menuCommands` source, BEFORE the panel source, so incumbent-wins protects the ids. Every `when`
 * mirrors the matching menu entry's, so the palette and the menu agree about applicability.
 */
export function menuCommands(
    actions: MenuCommandActions,
    data: MenuCommandData,
): readonly Command[] {
    const editor = (
        id: string,
        title: string,
        when: string,
        summary: string,
        detail: string,
        handler: () => CommandOutcome | Promise<CommandOutcome>,
    ): Command => ({
        id,
        title,
        category: "editor",
        when,
        docs: { summary, detail },
        handler,
    });
    const commands: Command[] = [
        editor(
            PROJECT_NEW_COMMAND_ID,
            "New Project…",
            "",
            "Pick a folder and scaffold a new project from the default template",
            "project action (d3); the e14c welcome flow: welcome.pickFolder + welcome.newProject " +
                "(the Shell spawns `context new` via the located CLI)",
            () => actions.newProject(),
        ),
        editor(
            PROJECT_OPEN_COMMAND_ID,
            "Open Project…",
            "",
            "Pick a folder and open it as a project",
            "project action (d3); welcome.pickFolder + welcome.open (the e14a launch flow decides " +
                "spawn-vs-focus)",
            () => actions.openProject(),
        ),
        editor(
            EDIT_CUT_COMMAND_ID,
            "Cut",
            "textInputFocus",
            "Cut the selection in the focused text field",
            "edit action (d3, the 03 ⏳ row): delegates to the browser's native editing inside a " +
                "focused text field; app-level clipboard is future work",
            () => actions.editClipboard("cut"),
        ),
        editor(
            EDIT_COPY_COMMAND_ID,
            "Copy",
            "textInputFocus",
            "Copy the selection in the focused text field",
            "edit action (d3, the 03 ⏳ row): delegates to the browser's native editing inside a " +
                "focused text field; app-level clipboard is future work",
            () => actions.editClipboard("copy"),
        ),
        editor(
            EDIT_PASTE_COMMAND_ID,
            "Paste",
            "textInputFocus",
            "Paste into the focused text field",
            "edit action (d3, the 03 ⏳ row): delegates to the browser's native editing inside a " +
                "focused text field; app-level clipboard is future work",
            () => actions.editClipboard("paste"),
        ),
        editor(
            SELECTION_CLEAR_COMMAND_ID,
            "Clear Selection",
            "",
            "Clear the session selection",
            "selection action (d3): `editor.selection-set []` over the session.select relay — the " +
                "same proven editor.select chain a scene-tree clear writes through",
            () => actions.clearSelection(),
        ),
        editor(
            VIEW_OPEN_SETTINGS_COMMAND_ID,
            "Open Settings",
            "",
            "Open the Settings panel",
            "view action (d3): PanelHost.open(\"builtin.settings\")",
            () => actions.openSettings(),
        ),
        editor(
            VIEW_WINDOW_CLOSE_COMMAND_ID,
            "Close Window",
            "",
            "Close this window (its panels rehome to the primary)",
            "window action (d3): dispatches `window.close`, which carries the primary-vs-secondary " +
                "policy Shell-side",
            () => actions.closeWindow(),
        ),
        editor(
            WINDOW_QUIT_COMMAND_ID,
            "Quit",
            "",
            "Quit the editor (the primary window's close path)",
            "window action (d3): the primary-window close policy — `window.close` on the primary " +
                "shuts the app down",
            () => actions.quit(),
        ),
        editor(
            WINDOW_MINIMIZE_COMMAND_ID,
            "Minimize Window",
            "",
            "Minimize this window",
            "window action (d3, 02 §5): dispatches the `window.minimize` control verb",
            () => actions.minimizeWindow(),
        ),
        editor(
            WINDOW_TOGGLE_MAXIMIZE_COMMAND_ID,
            "Maximize or Restore Window",
            "",
            "Toggle this window's maximized state",
            "window action (d3, 02 §5): dispatches the `window.toggle-maximize` control verb; the " +
                "menu label flips on the maximized fact",
            () => actions.toggleMaximizeWindow(),
        ),
        editor(
            HELP_DOCS_COMMAND_ID,
            "Documentation",
            "",
            "Open the documentation in the system browser",
            "help action (d3): the Shell's native URL opener (the ReleaseNotice opener seam) " +
                "pointed at the docs page — editor-core itself makes no network call",
            () => actions.openDocs(),
        ),
        editor(
            HELP_ABOUT_COMMAND_ID,
            "About Context Editor",
            "",
            "Show the version and update state",
            "help action (d3): a chrome dialog rendering the running version plus the existing " +
                "`update.state` read — nothing is fetched that the Shell has not already fetched",
            () => actions.openAbout(),
        ),
    ];
    data.recents.forEach((recent, index) => {
        commands.push(
            editor(
                `${PROJECT_OPEN_RECENT_PREFIX}${String(index)}`,
                `Open Recent: ${recent.name}`,
                "",
                `Open the recent project ${recent.name}`,
                `project action (d3); opens ${recent.path} via welcome.open`,
                () => actions.openRecent(recent.path),
            ),
        );
    });
    for (const id of [...new Set<number>([data.selfWindowId, ...data.windows])]) {
        commands.push(
            editor(
                `${WINDOW_FOCUS_COMMAND_PREFIX}${String(id)}`,
                `Focus Window ${String(id + 1)}`,
                "",
                `Raise and focus window ${String(id + 1)}`,
                "window action (d3): dispatches `window.focus {windowId}` — the Shell routes " +
                    "request_activation at the named window",
                () => actions.focusWindow(id),
            ),
        );
    }
    return commands;
}

// ------------------------------------------------------------------------------- the fact return
// The macOS activation return path (fact 1): the Shell publishes `{windowId, commandId}` on
// `editor.ui.menu` (menu_facts.h), the mirror poll drains it onto this window's bus, and this
// subscriber executes the command through the SAME registry a web-menubar click uses.

/** The `editor.ui.menu` payload (menu_facts.cpp): which window, and the activated command id. */
export interface MenuFact {
    readonly windowId: number;
    readonly commandId: string;
}

/** Parse an `editor.ui.menu` payload, TOTAL like every wire parser here (`null` = unreadable). */
export function parseMenuFact(payload: unknown): MenuFact | null {
    if (!isRecord(payload)) {
        return null;
    }
    const windowId = payload["windowId"];
    const commandId = payload["commandId"];
    if (typeof windowId !== "number" || !Number.isFinite(windowId)) {
        return null;
    }
    if (typeof commandId !== "string" || commandId === "") {
        return null;
    }
    return { windowId, commandId };
}

/**
 * Route native menu activations into the ONE registry (fact 1). The windowId filter is
 * belt-and-braces on top of the Shell's unicast, exactly as `subscribeChromeFacts` documents; a
 * malformed payload executes nothing. Extracted (and exported) so the DOM tier drives THIS
 * function against a real bus — the subscribeChromeFacts rule.
 */
export function subscribeMenuFacts(
    bus: EditorUiBus,
    windowId: number,
    execute: (commandId: string) => void,
): EditorUiSubscription {
    return bus.subscribe(UI_TOPIC_MENU, (event): void => {
        const fact = parseMenuFact(event.payload);
        if (fact !== null && fact.windowId === windowId) {
            execute(fact.commandId);
        }
    });
}

// ------------------------------------------------------------------------------- the DOM classes

export const MENUBAR_CLASS = "ctx-menubar";
export const MENUBAR_ENTRY_CLASS = "ctx-menubar__entry";
export const MENUBAR_ITEM_CLASS = "ctx-menubar__item";
export const MENU_CLASS = "ctx-menu";
export const MENU_SUBMENU_CLASS = "ctx-menu--submenu";
export const MENU_ITEM_CLASS = "ctx-menu__item";
export const MENU_ITEM_SUBMENU_CLASS = "ctx-menu__item--submenu";
export const MENU_ITEM_LABEL_CLASS = "ctx-menu__label";
export const MENU_ITEM_ACCEL_CLASS = "ctx-menu__accel";
export const MENU_SEPARATOR_CLASS = "ctx-menu__separator";
export const MENU_SUBMENU_ENTRY_CLASS = "ctx-menu__entry";

/** The `<html>` report of what the menubar rendered — boot diagnosability, like every `data-editor-*`. */
export const MENUBAR_ATTRIBUTE = "data-editor-menubar";

// ------------------------------------------------------------------------------- the menubar

export interface MountMenubarOptions {
    readonly model: MenuModel;
    /** The resolved when-context, read fresh on every dropdown open — the palette-view rule. */
    readonly contextProvider: () => WhenContext;
    /** Dispatch through the late-bound registry (boot closes over the holder — the a2 pattern). */
    readonly executeCommand: (commandId: string) => void;
    /** Does a command id currently resolve? (`liveRegistry.current?.has` — late-bound too.) */
    readonly commandAvailable: CommandAvailability;
    /** The maximized fact the Maximize/Restore label flips on (chrome.ts mount's own state). */
    readonly isMaximized: () => boolean;
}

/** What `mountMenubar` produced — the handle boot keeps and the T1 tier asserts on. */
export interface MenubarMount {
    readonly element: HTMLElement;
    /** The open top-level menu's id, or `null` while every dropdown is closed. */
    openMenuId(): string | null;
    /** Open one top-level menu by id (re-rendering its items against the live context). */
    openMenu(menuId: string): void;
    /** Close every dropdown. Idempotent. */
    closeMenus(): void;
    dispose(): void;
}

/**
 * Render the web menubar into `slot` (the titlebar's menu host, chrome.ts) — fact 4's overlay
 * pattern throughout. Dropdown CONTENT is rebuilt every time a menu opens, which is what makes
 * enablement live (the fresh when-context + registry probe) without a single poll.
 */
export function mountMenubar(slot: HTMLElement, options: MountMenubarOptions): MenubarMount {
    const doc = slot.ownerDocument;
    slot.replaceChildren();
    slot.classList.add(MENUBAR_CLASS);
    slot.setAttribute("role", "menubar");
    slot.setAttribute("aria-label", "Application menu");

    interface TopLevel {
        readonly menu: MenuDefinition;
        readonly button: HTMLButtonElement;
        readonly dropdown: HTMLElement;
    }
    const topLevel: TopLevel[] = [];
    let openId: string | null = null;
    let disposed = false;

    // The outside-click closer, armed only while a dropdown is open (the transient-listener rule:
    // an idle menubar leaves nothing on the document).
    const onDocumentPointerDown = (event: Event): void => {
        const target = event.target;
        if (!(target instanceof Node) || !slot.contains(target)) {
            closeAll();
        }
    };
    let outsideCloserArmed = false;
    const armOutsideCloser = (): void => {
        if (!outsideCloserArmed) {
            doc.addEventListener("mousedown", onDocumentPointerDown);
            outsideCloserArmed = true;
        }
    };
    const disarmOutsideCloser = (): void => {
        if (outsideCloserArmed) {
            doc.removeEventListener("mousedown", onDocumentPointerDown);
            outsideCloserArmed = false;
        }
    };

    const report = (): void => {
        doc.documentElement.setAttribute(
            MENUBAR_ATTRIBUTE,
            `menus=${String(topLevel.length)} open=${openId ?? "none"}`,
        );
    };

    const closeAll = (): void => {
        for (const entry of topLevel) {
            entry.button.setAttribute("aria-expanded", "false");
            entry.dropdown.hidden = true;
            entry.dropdown.replaceChildren();
        }
        openId = null;
        disarmOutsideCloser();
        report();
    };

    /** The roving tabindex (the ARIA menubar pattern): exactly one top-level item is tabbable. */
    const setTabStop = (index: number): void => {
        topLevel.forEach((entry, i) => {
            entry.button.tabIndex = i === index ? 0 : -1;
        });
    };

    const menuItems = (dropdown: HTMLElement): HTMLButtonElement[] =>
        [...dropdown.querySelectorAll<HTMLButtonElement>(`.${MENU_ITEM_CLASS}`)];

    const focusItem = (items: readonly HTMLButtonElement[], index: number): void => {
        const item = items[index];
        if (item !== undefined) {
            item.focus();
        }
    };

    /** Build one dropdown's items against the LIVE context (called at every open). */
    const renderEntries = (
        dropdown: HTMLElement,
        entries: readonly MenuEntry[],
        depth: number,
    ): void => {
        dropdown.replaceChildren();
        const context = options.contextProvider();
        for (const entry of entries) {
            if (entry.kind === "separator") {
                const separator = doc.createElement("div");
                separator.className = MENU_SEPARATOR_CLASS;
                separator.setAttribute("role", "separator");
                dropdown.append(separator);
                continue;
            }
            if (entry.kind === "submenu") {
                // ONE nesting level ships (Open Recent — 03's tree needs exactly it); the model
                // type admits deeper trees for the C++ parse's sake, but the web renderer keeps
                // the palette pattern's flatness and renders a deeper submenu's header disabled.
                const wrapper = doc.createElement("div");
                wrapper.className = MENU_SUBMENU_ENTRY_CLASS;
                const header = doc.createElement("button");
                header.type = "button";
                header.className = `${MENU_ITEM_CLASS} ${MENU_ITEM_SUBMENU_CLASS}`;
                header.setAttribute("role", "menuitem");
                header.setAttribute("aria-haspopup", "menu");
                header.setAttribute("aria-expanded", "false");
                header.tabIndex = -1;
                const headerLabel = doc.createElement("span");
                headerLabel.className = MENU_ITEM_LABEL_CLASS;
                headerLabel.textContent = entry.label;
                header.append(headerLabel);
                const submenu = doc.createElement("div");
                submenu.className = `${MENU_CLASS} ${MENU_SUBMENU_CLASS}`;
                submenu.setAttribute("role", "menu");
                submenu.setAttribute("aria-label", entry.label);
                submenu.hidden = true;
                const openSubmenu = (): void => {
                    if (depth >= 1) {
                        return; // deeper than the shipped nesting — the header stays inert
                    }
                    renderEntries(submenu, entry.items, depth + 1);
                    submenu.hidden = false;
                    header.setAttribute("aria-expanded", "true");
                };
                const closeSubmenu = (): void => {
                    submenu.hidden = true;
                    submenu.replaceChildren();
                    header.setAttribute("aria-expanded", "false");
                };
                header.addEventListener("click", (): void => {
                    if (submenu.hidden) {
                        openSubmenu();
                        focusItem(menuItems(submenu), 0);
                    } else {
                        closeSubmenu();
                    }
                });
                header.addEventListener("mouseenter", openSubmenu);
                // key-handler-ok: ARIA menu submenu navigation (ArrowRight opens / ArrowLeft
                // closes) on an OPEN dropdown — dispatch still goes through the ONE registry via
                // each item's activation; never a global chord (the palette-view precedent).
                header.addEventListener("keydown", (event: KeyboardEvent): void => {
                    if (event.key === "ArrowRight" || event.key === "Enter" || event.key === " ") {
                        event.preventDefault();
                        event.stopPropagation();
                        openSubmenu();
                        focusItem(menuItems(submenu), 0);
                    }
                });
                // key-handler-ok: ArrowLeft backs out of the OPEN submenu to its header —
                // submenu-local navigation, the same palette-view rationale as above.
                submenu.addEventListener("keydown", (event: KeyboardEvent): void => {
                    if (event.key === "ArrowLeft") {
                        event.preventDefault();
                        event.stopPropagation();
                        closeSubmenu();
                        header.focus();
                    }
                });
                wrapper.append(header, submenu);
                dropdown.append(wrapper);
                continue;
            }
            const enabled = menuEntryEnabled(entry, context, options.commandAvailable);
            const item = doc.createElement("button");
            item.type = "button";
            item.className = MENU_ITEM_CLASS;
            item.setAttribute("role", "menuitem");
            item.setAttribute("data-command-id", entry.commandId);
            item.tabIndex = -1;
            // `aria-disabled`, NOT the native disabled attribute: the ARIA menu pattern keeps a
            // disabled item focusable so arrow navigation can reach (and a screen reader can
            // announce) it — the activation guard below is what makes it truly inert.
            item.setAttribute("aria-disabled", enabled ? "false" : "true");
            if (!enabled) {
                item.title = menuEntryDisabledTooltip(entry);
            }
            const label = doc.createElement("span");
            label.className = MENU_ITEM_LABEL_CLASS;
            label.textContent =
                entry.commandId === WINDOW_TOGGLE_MAXIMIZE_COMMAND_ID
                    ? toggleMaximizeLabel(options.isMaximized())
                    : entry.label;
            item.append(label);
            if (entry.accelerator !== "") {
                item.setAttribute("aria-keyshortcuts", entry.accelerator);
                const accel = doc.createElement("span");
                accel.className = MENU_ITEM_ACCEL_CLASS;
                accel.setAttribute("aria-hidden", "true"); // aria-keyshortcuts carries it instead
                accel.textContent = entry.accelerator;
                item.append(accel);
            }
            item.addEventListener("click", (): void => {
                if (item.getAttribute("aria-disabled") === "true") {
                    return; // disabled-item honesty: truly inert, not merely greyed
                }
                const commandId = entry.commandId;
                closeAll();
                options.executeCommand(commandId);
            });
            dropdown.append(item);
        }
    };

    const openMenuAt = (index: number, focusFirst: boolean): void => {
        const entry = topLevel[index];
        if (entry === undefined) {
            return;
        }
        closeAll();
        renderEntries(entry.dropdown, entry.menu.items, 0);
        entry.dropdown.hidden = false;
        entry.button.setAttribute("aria-expanded", "true");
        openId = entry.menu.id;
        setTabStop(index);
        armOutsideCloser();
        report();
        if (focusFirst) {
            focusItem(menuItems(entry.dropdown), 0);
        }
    };

    options.model.menus.forEach((menu, index) => {
        const wrapper = doc.createElement("div");
        wrapper.className = MENUBAR_ENTRY_CLASS;
        const button = doc.createElement("button");
        button.type = "button";
        button.className = MENUBAR_ITEM_CLASS;
        button.setAttribute("role", "menuitem");
        button.setAttribute("aria-haspopup", "menu");
        button.setAttribute("aria-expanded", "false");
        button.setAttribute("data-menu-id", menu.id);
        button.textContent = menu.label;
        button.tabIndex = index === 0 ? 0 : -1;
        const dropdown = doc.createElement("div");
        dropdown.className = MENU_CLASS;
        dropdown.setAttribute("role", "menu");
        dropdown.setAttribute("aria-label", menu.label);
        dropdown.hidden = true;
        button.addEventListener("click", (): void => {
            if (openId === menu.id) {
                closeAll();
                setTabStop(index);
            } else {
                openMenuAt(index, false);
            }
        });
        // Hover FOLLOWS an open menu (the platform menubar convention): moving along the bar with
        // a dropdown open switches menus without a second click; an idle bar ignores hover.
        button.addEventListener("mouseenter", (): void => {
            if (openId !== null && openId !== menu.id) {
                openMenuAt(index, false);
            }
        });
        wrapper.append(button, dropdown);
        slot.append(wrapper);
        topLevel.push({ menu, button, dropdown });
    });

    // ARIA menubar keyboard navigation (arrows/Enter/Escape/Home/End) — menu-local list navigation
    // and activation in the palette-view's exact pattern: activation dispatches the chosen command
    // through the ONE registry, and nothing here is a global chord (the menubar only sees keys
    // while focus is inside it). key-handler-ok: the 03 keyboard contract, on the command path.
    slot.addEventListener("keydown", (event: KeyboardEvent): void => {
        const active = doc.activeElement;
        const topIndex = topLevel.findIndex((entry) => entry.button === active);
        const inTopLevel = topIndex !== -1;
        const openIndex = topLevel.findIndex((entry) => entry.menu.id === openId);
        switch (event.key) {
            case "ArrowRight":
            case "ArrowLeft": {
                // Submenu headers handled their own arrows (stopPropagation); here the arrows walk
                // the TOP-LEVEL menus — from the bar, or across an open dropdown.
                const delta = event.key === "ArrowRight" ? 1 : -1;
                const count = topLevel.length;
                if (count === 0) {
                    return;
                }
                if (inTopLevel) {
                    event.preventDefault();
                    const next = (topIndex + delta + count) % count;
                    if (openId !== null) {
                        openMenuAt(next, false);
                    }
                    setTabStop(next);
                    topLevel[next]?.button.focus();
                    return;
                }
                if (openIndex !== -1) {
                    event.preventDefault();
                    const next = (openIndex + delta + count) % count;
                    openMenuAt(next, true);
                    topLevel[next]?.button.focus();
                    return;
                }
                return;
            }
            case "ArrowDown":
            case "ArrowUp": {
                event.preventDefault();
                if (inTopLevel) {
                    openMenuAt(topIndex, true);
                    return;
                }
                if (openIndex === -1) {
                    return;
                }
                const dropdown = topLevel[openIndex]?.dropdown;
                if (dropdown === undefined) {
                    return;
                }
                // Navigate within the INNERMOST open menu the focus is in (submenu-aware).
                const scope =
                    active instanceof Element ? active.closest(`.${MENU_CLASS}`) : null;
                const items = menuItems(
                    scope instanceof HTMLElement && !scope.hidden ? scope : dropdown,
                );
                if (items.length === 0) {
                    return;
                }
                const current = items.findIndex((item) => item === active);
                const delta = event.key === "ArrowDown" ? 1 : -1;
                const next =
                    current === -1
                        ? delta === 1
                          ? 0
                          : items.length - 1
                        : (current + delta + items.length) % items.length;
                focusItem(items, next);
                return;
            }
            case "Home":
            case "End": {
                if (openIndex === -1) {
                    return;
                }
                event.preventDefault();
                const dropdown = topLevel[openIndex]?.dropdown;
                if (dropdown === undefined) {
                    return;
                }
                const scope =
                    active instanceof Element ? active.closest(`.${MENU_CLASS}`) : null;
                const items = menuItems(
                    scope instanceof HTMLElement && !scope.hidden ? scope : dropdown,
                );
                focusItem(items, event.key === "Home" ? 0 : items.length - 1);
                return;
            }
            case "Enter":
            case " ": {
                // A focused menu ITEM is a real <button>, so the browser synthesises `click` from
                // Enter/Space itself — the kit-button rule. Only the TOP-LEVEL open needs a hand
                // here (it must also focus the first item, which a synthesized click does not).
                if (inTopLevel) {
                    event.preventDefault();
                    openMenuAt(topIndex, true);
                }
                return;
            }
            case "Escape": {
                if (openIndex !== -1) {
                    event.preventDefault();
                    const button = topLevel[openIndex]?.button;
                    closeAll();
                    button?.focus();
                }
                return;
            }
            default:
                return;
        }
    });

    report();

    return {
        element: slot,
        openMenuId: (): string | null => openId,
        openMenu: (menuId: string): void => {
            const index = topLevel.findIndex((entry) => entry.menu.id === menuId);
            if (index !== -1) {
                openMenuAt(index, false);
            }
        },
        closeMenus: (): void => {
            closeAll();
        },
        dispose: (): void => {
            if (disposed) {
                return;
            }
            disposed = true;
            closeAll();
            slot.replaceChildren();
        },
    };
}

// ------------------------------------------------------------------------------- the About dialog

export const ABOUT_CLASS = "ctx-about";
export const ABOUT_TITLE_CLASS = "ctx-about__title";
export const ABOUT_VERSION_CLASS = "ctx-about__version";
export const ABOUT_UPDATE_CLASS = "ctx-about__update";

/** The dialog's rendered lines for one `update.state` answer — one table, testable without a DOM. */
export function aboutLines(state: UpdateState | null): {
    readonly version: string;
    readonly update: string;
} {
    if (state === null || state.current === "") {
        return {
            version: "Version unknown (no update surface behind this Shell)",
            update: "",
        };
    }
    const version = `Version ${state.current}`;
    if (!state.checked) {
        return { version, update: "Update check unavailable" };
    }
    return {
        version,
        update: state.updateAvailable
            ? `Update available: ${state.latest}`
            : "Up to date",
    };
}

/** What `openAboutDialog` produced — the handle the T1 tier asserts on. */
export interface AboutDialog {
    readonly element: HTMLElement;
    close(): void;
}

/**
 * Show the About chrome dialog (03's table: version + the existing `update.state` read). An
 * app-chrome overlay in the palette's pattern — CSP-clean DOM, app.css classes, one kit button.
 * `state` is whatever the caller already fetched (boot reads it through the SAME BannerClient the
 * banners use); `null` renders the honest "unknown" lines rather than fetching anything here.
 */
export function openAboutDialog(host: HTMLElement, state: UpdateState | null): AboutDialog {
    const doc = host.ownerDocument;
    const root = doc.createElement("div");
    root.className = ABOUT_CLASS;
    root.setAttribute("role", "dialog");
    root.setAttribute("aria-modal", "true");
    root.setAttribute("aria-label", "About Context Editor");

    const title = doc.createElement("div");
    title.className = ABOUT_TITLE_CLASS;
    title.textContent = "Context Editor";
    root.append(title);

    const lines = aboutLines(state);
    const version = doc.createElement("div");
    version.className = ABOUT_VERSION_CLASS;
    version.textContent = lines.version;
    root.append(version);
    if (lines.update !== "") {
        const update = doc.createElement("div");
        update.className = ABOUT_UPDATE_CLASS;
        update.textContent = lines.update;
        root.append(update);
    }

    let closed = false;
    const close = (): void => {
        if (!closed) {
            closed = true;
            root.remove();
        }
    };
    const closeButton = createButton({
        label: "Close",
        onActivate: close,
    });
    root.append(closeButton.element);
    // key-handler-ok: Escape dismisses this open dialog — dialog-local, the palette-view rationale;
    // no command is dispatched and no global chord is claimed.
    root.addEventListener("keydown", (event: KeyboardEvent): void => {
        if (event.key === "Escape") {
            event.preventDefault();
            close();
        }
    });
    host.append(root);
    closeButton.element.focus();
    return { element: root, close };
}
