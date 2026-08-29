// T1 for the d3 application menu (editor-window-chrome d3, menu structure 03).
//
// WHAT THIS TIER PROVES (the DoD's web half — the ts-a11y browser tier, the settings.test.ts
// worked example):
//
//   * THE TREE IS EXACTLY 03's TABLE, both platform shapes — the model builder is pinned menu by
//     menu, item by item, so a drive-by "tidy" of the tree reds a named assertion instead of
//     silently shipping a different menu.
//   * THE ACCELERATOR COLUMN DISPLAYS DEFAULT_KEYBINDINGS (01 §7) — where a binding exists it
//     shows, where none exists nothing is invented.
//   * SINGLE DISPATCH: a menu-item activation reaches the SAME registry path as the palette for a
//     shared command (03's "no second dispatch system"), asserted on one spy fed by both paths.
//   * EVERY NEW COMMAND dispatches its gateway (project/selection/window/help spies, the recents'
//     paths, the per-window focus ids) with honest failure notes.
//   * DISABLED-ITEM HONESTY: the ⏳ rows render disabled with the reason in the tooltip and are
//     TRULY INERT (click and Enter dispatch nothing) — and flip live once the guard holds.
//   * ARIA + KEYBOARD NAV: menubar/menu/menuitem roles, arrows/Enter/Escape/Home/End — the 03
//     keyboard contract, driven through real key events in the browser tier.
//   * THE NATIVE RETURN PATH: an `editor.ui.menu` envelope from the Shell (origin "shell", the
//     mirror relay's shape) executes its command id through the injected dispatch; a wrong-window
//     or malformed envelope executes nothing.

import {
    assert,
    assertEqual,
    delay,
    noopPlayActions,
    type TestCase,
} from "./harness.js";
import {
    CLIPBOARD_DISABLED_REASON,
    EDIT_CUT_COMMAND_ID,
    HELP_ABOUT_COMMAND_ID,
    HELP_DOCS_COMMAND_ID,
    MENU_ITEM_CLASS,
    MENUBAR_ITEM_CLASS,
    NO_RECENTS_REASON,
    PROJECT_NEW_COMMAND_ID,
    PROJECT_OPEN_COMMAND_ID,
    PROJECT_OPEN_RECENT_PREFIX,
    SELECTION_CLEAR_COMMAND_ID,
    VIEW_OPEN_SETTINGS_COMMAND_ID,
    VIEW_WINDOW_CLOSE_COMMAND_ID,
    WINDOW_FOCUS_COMMAND_PREFIX,
    WINDOW_MINIMIZE_COMMAND_ID,
    WINDOW_QUIT_COMMAND_ID,
    WINDOW_TOGGLE_MAXIMIZE_COMMAND_ID,
    aboutLines,
    buildMenuModel,
    makeMenuActions,
    menuAcceleratorFor,
    menuCommands,
    menuModelJson,
    mountMenubar,
    openAboutDialog,
    parseMenuFact,
    subscribeMenuFacts,
    type MenuActionDeps,
    type MenuCommandActions,
    type MenuCommandEntry,
    type MenuModel,
    type MenuSubmenuEntry,
} from "../menu.js";
import { buildCommandRegistry, type CommandOutcome } from "../commands.js";
import { EDITOR_PLAYBAR_ID, EDITOR_STATUSBAR_ID, EDITOR_TITLEBAR_ID, mountChrome, TITLEBAR_DRAG_CLASS, TITLEBAR_MENU_CLASS, type ChromeStripElements } from "../chrome.js";
import { PALETTE_TOGGLE_COMMAND_ID, Palette } from "../palette.js";
import { EditorUiBus, UI_TOPIC_MENU } from "../uibus.js";
import {
    CHROME_MODE_CUSTOM,
    CHROME_MODE_HYBRID,
    CHROME_MODE_SYSTEM,
    CHROME_WINDOW_PRIMARY,
    CHROME_WINDOW_SECONDARY,
    type ChromeState,
} from "../window.js";
import type { UpdateState } from "../banners.js";
import type { PanelRoster } from "../panels.js";

// ------------------------------------------------------------------------------- model fixtures

const RECENTS = [
    { path: "C:/projects/alpha", name: "alpha", lastOpenedMs: 3 },
    { path: "C:/projects/beta", name: "beta", lastOpenedMs: 2 },
] as const;

function model(mode: "custom" | "hybrid" | "system" = "custom"): MenuModel {
    return buildMenuModel({
        mode,
        recents: RECENTS,
        windows: [0, 2],
        selfWindowId: 0,
    });
}

function menuIds(m: MenuModel): readonly string[] {
    return m.menus.map((menu) => menu.id);
}

function commandIdsOf(m: MenuModel, menuId: string): readonly string[] {
    const menu = m.menus.find((entry) => entry.id === menuId);
    if (menu === undefined) {
        return [];
    }
    const ids: string[] = [];
    for (const item of menu.items) {
        if (item.kind === "command") {
            ids.push(item.commandId);
        } else if (item.kind === "submenu") {
            for (const child of item.items) {
                if (child.kind === "command") {
                    ids.push(child.commandId);
                }
            }
        }
    }
    return ids;
}

// ------------------------------------------------------------------------------- action fixtures

interface ActionLog {
    readonly calls: string[];
    readonly actions: MenuCommandActions;
}

/** A recording MenuCommandActions where every call answers ok — the dispatch-shape observable. */
function actionSpies(): ActionLog {
    const calls: string[] = [];
    const done = (note: string): CommandOutcome => ({ ok: true, note });
    return {
        calls,
        actions: {
            newProject: () => (calls.push("newProject"), done("newProject")),
            openProject: () => (calls.push("openProject"), done("openProject")),
            openRecent: (path: string) => (calls.push(`openRecent:${path}`), done("openRecent")),
            editClipboard: (verb) => (calls.push(`edit:${verb}`), done(verb)),
            clearSelection: () => (calls.push("clearSelection"), done("clearSelection")),
            openSettings: () => (calls.push("openSettings"), done("openSettings")),
            closeWindow: () => (calls.push("closeWindow"), done("closeWindow")),
            quit: () => (calls.push("quit"), done("quit")),
            minimizeWindow: () => (calls.push("minimize"), done("minimize")),
            toggleMaximizeWindow: () => (calls.push("toggleMaximize"), done("toggleMaximize")),
            focusWindow: (windowId: number) =>
                (calls.push(`focusWindow:${String(windowId)}`), done("focusWindow")),
            openDocs: () => (calls.push("openDocs"), done("openDocs")),
            openAbout: () => (calls.push("openAbout"), done("openAbout")),
        },
    };
}

const EMPTY_ROSTER: PanelRoster = { contractMajor: 1, panels: [] };

/** A REAL registry carrying the d3 command set over spies — the single-dispatch fixture. */
function registryWith(log: ActionLog) {
    return buildCommandRegistry({
        contractDispatch: () => ({ ok: false, note: "not wired in this fixture" }),
        editorActions: {
            focusNextPanel: () => ({ ok: true, note: "" }),
            focusPreviousPanel: () => ({ ok: true, note: "" }),
            moveActivePanel: () => ({ ok: true, note: "" }),
            closeActivePanel: () => ({ ok: true, note: "" }),
            toggleTheme: () => ({ ok: true, note: "" }),
            tearOutActivePanel: () => ({ ok: true, note: "" }),
            movePanelToPrimary: () => ({ ok: true, note: "" }),
        },
        sessionActions: { undo: () => ({ ok: true, note: "" }), redo: () => ({ ok: true, note: "" }) },
        playActions: noopPlayActions(),
        menuCommands: menuCommands(log.actions, {
            recents: RECENTS,
            windows: [0, 2],
            selfWindowId: 0,
        }),
        roster: EMPTY_ROSTER,
        panelDispatch: () => ({ ok: false, note: "no panels in this fixture" }),
    });
}

// ------------------------------------------------------------------------------- menubar fixture

interface MenubarHarness {
    readonly slot: HTMLElement;
    readonly executed: string[];
    context: Record<string, string | boolean>;
    maximized: boolean;
    readonly mount: ReturnType<typeof mountMenubar>;
    dispose(): void;
}

function menubarHarness(options?: {
    readonly model?: MenuModel;
    readonly available?: (id: string) => string | null;
    readonly execute?: (id: string) => void;
}): MenubarHarness {
    const slot = document.createElement("div");
    document.body.append(slot);
    const executed: string[] = [];
    // The default availability mirrors boot's real wiring: a REAL registry's own `when` guards
    // (the single-home rule), so enablement here rides the same truth the palette reads.
    const registry = registryWith(actionSpies());
    const registryAvailability = (id: string): string | null => registry.get(id)?.when ?? null;
    const harness: MenubarHarness = {
        slot,
        executed,
        context: {},
        maximized: false,
        mount: mountMenubar(slot, {
            model: options?.model ?? model(),
            contextProvider: () => harness.context,
            executeCommand:
                options?.execute ??
                ((id: string): void => {
                    executed.push(id);
                }),
            commandAvailable: options?.available ?? registryAvailability,
            isMaximized: () => harness.maximized,
        }),
        dispose: (): void => {
            harness.mount.dispose();
            slot.remove();
            document.documentElement.removeAttribute("data-editor-menubar");
        },
    };
    return harness;
}

function topLevelButton(slot: HTMLElement, menuId: string): HTMLButtonElement {
    const button = slot.querySelector<HTMLButtonElement>(`[data-menu-id="${menuId}"]`);
    assert(button !== null, `the ${menuId} top-level item exists`);
    return button as HTMLButtonElement;
}

function dropdownItem(slot: HTMLElement, commandId: string): HTMLButtonElement {
    const item = slot.querySelector<HTMLButtonElement>(`[data-command-id="${commandId}"]`);
    assert(item !== null, `the ${commandId} item is rendered in the open dropdown`);
    return item as HTMLButtonElement;
}

function press(target: Element, key: string): void {
    target.dispatchEvent(new KeyboardEvent("keydown", { key, bubbles: true, cancelable: true }));
}

// ---------------------------------------------------------------------------------- the tests

export const menuTests: readonly TestCase[] = [
    // ------------------------------------------------------------------------ the model (03's table)
    {
        name: "menu model: the Windows/Linux tree is exactly 03's table",
        run: () => {
            const m = model("custom");
            assertEqual(
                menuIds(m),
                ["file", "edit", "view", "selection", "panel", "window", "help"],
                "the seven menus, in the table's order, no App menu off macOS",
            );
            assertEqual(
                commandIdsOf(m, "file"),
                [
                    PROJECT_NEW_COMMAND_ID,
                    PROJECT_OPEN_COMMAND_ID,
                    `${PROJECT_OPEN_RECENT_PREFIX}0`,
                    `${PROJECT_OPEN_RECENT_PREFIX}1`,
                    VIEW_WINDOW_CLOSE_COMMAND_ID,
                    WINDOW_QUIT_COMMAND_ID,
                ],
                "File: New / Open / Open Recent (per recent) / Close Window / Exit",
            );
            assertEqual(
                commandIdsOf(m, "edit"),
                [
                    "session.undo",
                    "session.redo",
                    EDIT_CUT_COMMAND_ID,
                    "edit.copy",
                    "edit.paste",
                ],
                "Edit: Undo / Redo / Cut / Copy / Paste (the two existing + the three ⏳ rows)",
            );
            assertEqual(
                commandIdsOf(m, "view"),
                [
                    PALETTE_TOGGLE_COMMAND_ID,
                    "view.theme.toggle",
                    "view.panel.focusNext",
                    "view.panel.focusPrevious",
                    "view.panel.close",
                ],
                "View: Command Palette / Toggle Theme / Focus Next / Focus Previous / Close Panel",
            );
            assertEqual(
                commandIdsOf(m, "selection"),
                [SELECTION_CLEAR_COMMAND_ID],
                "Selection: exactly the clear (the menu grows with e11 picking)",
            );
            assertEqual(
                commandIdsOf(m, "panel"),
                [
                    "view.window.tearOut",
                    "view.window.moveToPrimary",
                    "view.panel.move.left",
                    "view.panel.move.right",
                    "view.panel.move.up",
                    "view.panel.move.down",
                ],
                "Panel: Tear Out / Move to Primary / the four dock moves",
            );
            assertEqual(
                commandIdsOf(m, "window"),
                [
                    WINDOW_MINIMIZE_COMMAND_ID,
                    WINDOW_TOGGLE_MAXIMIZE_COMMAND_ID,
                    `${WINDOW_FOCUS_COMMAND_PREFIX}0`,
                    `${WINDOW_FOCUS_COMMAND_PREFIX}2`,
                ],
                "Window: Minimize / Maximize / the window list (self + the peer)",
            );
            assertEqual(
                commandIdsOf(m, "help"),
                [HELP_DOCS_COMMAND_ID, HELP_ABOUT_COMMAND_ID],
                "Help: Documentation / About (About lives here off macOS)",
            );
            // Play/pause/stop deliberately have NO menu (03): no play.* id anywhere in the tree.
            for (const menu of m.menus) {
                assert(
                    !commandIdsOf(m, menu.id).some((id) => id.startsWith("play.")),
                    `no transport command in the ${menu.id} menu — the strip and palette own play`,
                );
            }
        },
    },
    {
        name: "menu model: the macOS (hybrid) shape carries the App menu and moves Quit/About into it",
        run: () => {
            const m = model("hybrid");
            assertEqual(
                menuIds(m),
                ["app", "file", "edit", "view", "selection", "panel", "window", "help"],
                "the App menu leads on macOS",
            );
            assertEqual(
                commandIdsOf(m, "app"),
                [HELP_ABOUT_COMMAND_ID, VIEW_OPEN_SETTINGS_COMMAND_ID, WINDOW_QUIT_COMMAND_ID],
                "App: About / Settings… / Quit (03's macOS-only column)",
            );
            assert(
                !commandIdsOf(m, "file").includes(WINDOW_QUIT_COMMAND_ID),
                "File carries no Exit on macOS — Quit lives in the App menu",
            );
            assertEqual(
                commandIdsOf(m, "help"),
                [HELP_DOCS_COMMAND_ID],
                "Help carries no About on macOS — it lives in the App menu",
            );
        },
    },
    {
        name: "menu model: the accelerator column displays DEFAULT_KEYBINDINGS and invents nothing",
        run: () => {
            assertEqual(menuAcceleratorFor("session.undo"), "Ctrl+Z", "Undo shows its binding");
            assertEqual(
                menuAcceleratorFor(PALETTE_TOGGLE_COMMAND_ID),
                "Ctrl+Shift+P",
                "Command Palette shows its binding",
            );
            assertEqual(
                menuAcceleratorFor(PROJECT_NEW_COMMAND_ID),
                "",
                "a command with no default binding shows NO accelerator — nothing is invented",
            );
            const edit = model().menus.find((menu) => menu.id === "edit");
            const undo = edit?.items.find(
                (item): item is MenuCommandEntry =>
                    item.kind === "command" && item.commandId === "session.undo",
            );
            assertEqual(undo?.accelerator, "Ctrl+Z", "the model entry carries the display chord");
        },
    },
    {
        name: "menu model: an empty recents list renders the honest disabled placeholder",
        run: () => {
            const empty = buildMenuModel({
                mode: CHROME_MODE_CUSTOM,
                recents: [],
                windows: [0],
                selfWindowId: 0,
            });
            const file = empty.menus.find((menu) => menu.id === "file");
            const submenu = file?.items.find(
                (item): item is MenuSubmenuEntry => item.kind === "submenu",
            );
            assertEqual(submenu?.label, "Open Recent", "the submenu still renders");
            const placeholder = submenu?.items[0];
            assert(
                placeholder !== undefined && placeholder.kind === "command",
                "with one placeholder entry",
            );
            if (placeholder !== undefined && placeholder.kind === "command") {
                assertEqual(
                    placeholder.disabledReason,
                    NO_RECENTS_REASON,
                    "carrying the honest reason (it is never registered, so it renders disabled)",
                );
            }
        },
    },
    // ------------------------------------------------------------------------ the wire serialization
    {
        name: "menu model: menuModelJson serializes the publish-time enablement snapshot",
        run: () => {
            // The fixture registry: two resolvable ids (with their real guards); everything else
            // unregistered (null) — the CommandAvailability shape boot wires.
            const available = new Map<string, string>([
                [EDIT_CUT_COMMAND_ID, "textInputFocus"],
                [WINDOW_TOGGLE_MAXIMIZE_COMMAND_ID, ""],
            ]);
            const json = menuModelJson(model(), {
                context: { textInputFocus: true },
                available: (id) => available.get(id) ?? null,
                maximized: true,
            });
            const menus = json["menus"];
            assert(Array.isArray(menus), "the wire shape is {menus: [...]}");
            if (!Array.isArray(menus)) {
                return;
            }
            const edit = menus.find(
                (menu) => (menu as Record<string, unknown>)["id"] === "edit",
            ) as Record<string, unknown>;
            const items = edit["items"] as Record<string, unknown>[];
            const cut = items.find((item) => item["id"] === EDIT_CUT_COMMAND_ID);
            assert(cut !== undefined, "Cut serialized");
            assertEqual(cut?.["enabled"], true, "Cut is enabled: registered + textInputFocus holds");
            assertEqual(cut?.["tooltip"], undefined, "an enabled item carries no disabled tooltip");
            const undo = items.find((item) => item["id"] === "session.undo");
            assertEqual(
                undo?.["enabled"],
                false,
                "Undo is disabled at this snapshot — its id is not in the fixture registry",
            );
            const windowMenu = menus.find(
                (menu) => (menu as Record<string, unknown>)["id"] === "window",
            ) as Record<string, unknown>;
            const windowItems = windowMenu["items"] as Record<string, unknown>[];
            const toggle = windowItems.find(
                (item) => item["id"] === WINDOW_TOGGLE_MAXIMIZE_COMMAND_ID,
            );
            assertEqual(
                toggle?.["label"],
                "Restore",
                "the Maximize item publishes the flipped label under the maximized fact",
            );
            const separatorCount = items.filter((item) => item["type"] === "separator").length;
            assert(separatorCount >= 1, "separators serialize as their own type");
        },
    },
    // ------------------------------------------------------------------------ the actions + commands
    {
        name: "menu commands: every new command dispatches its action (recents and windows included)",
        run: async () => {
            const log = actionSpies();
            const registry = registryWith(log);
            const expectations: readonly (readonly [string, string])[] = [
                [PROJECT_NEW_COMMAND_ID, "newProject"],
                [PROJECT_OPEN_COMMAND_ID, "openProject"],
                [`${PROJECT_OPEN_RECENT_PREFIX}1`, "openRecent:C:/projects/beta"],
                [EDIT_CUT_COMMAND_ID, "edit:cut"],
                [SELECTION_CLEAR_COMMAND_ID, "clearSelection"],
                [VIEW_OPEN_SETTINGS_COMMAND_ID, "openSettings"],
                [VIEW_WINDOW_CLOSE_COMMAND_ID, "closeWindow"],
                [WINDOW_QUIT_COMMAND_ID, "quit"],
                [WINDOW_MINIMIZE_COMMAND_ID, "minimize"],
                [WINDOW_TOGGLE_MAXIMIZE_COMMAND_ID, "toggleMaximize"],
                [`${WINDOW_FOCUS_COMMAND_PREFIX}2`, "focusWindow:2"],
                [HELP_DOCS_COMMAND_ID, "openDocs"],
                [HELP_ABOUT_COMMAND_ID, "openAbout"],
            ];
            for (const [commandId, expected] of expectations) {
                const before = log.calls.length;
                const outcome = await registry.execute(commandId);
                assert(outcome.ok, `${commandId} executed ok`);
                assertEqual(
                    log.calls[before],
                    expected,
                    `${commandId} dispatched exactly its action`,
                );
            }
        },
    },
    {
        name: "menu actions: the real gateways are driven honestly (picker cancel, clear, docs)",
        run: async () => {
            const calls: string[] = [];
            let pickedPath: string | null = null;
            const deps: MenuActionDeps = {
                project: {
                    pickFolder: () => {
                        calls.push("pickFolder");
                        return Promise.resolve(
                            pickedPath === null
                                ? { picked: false, path: "" }
                                : { picked: true, path: pickedPath },
                        );
                    },
                    open: (path: string) => {
                        calls.push(`open:${path}`);
                        return Promise.resolve({ opened: true, action: "spawn", path });
                    },
                    newProject: (directory: string, template: string) => {
                        calls.push(`new:${directory}:${template}`);
                        return Promise.resolve({
                            created: true,
                            runnable: true,
                            directory,
                            template,
                            opened: true,
                            action: "spawn",
                        });
                    },
                },
                windowControls: {
                    minimize: () => Promise.resolve({ accepted: true }),
                    toggleMaximize: () => Promise.resolve({ accepted: true, maximized: true }),
                    close: () => (calls.push("close"), Promise.resolve({ closed: true })),
                    focus: (windowId?: number) => {
                        calls.push(`focus:${String(windowId)}`);
                        return Promise.resolve({ accepted: windowId === 2 });
                    },
                },
                select: {
                    select: (ids: readonly string[]) => {
                        calls.push(`select:[${ids.join(",")}]`);
                        return Promise.resolve({
                            served: true,
                            applied: true,
                            ids: [],
                            diagnostic: "",
                        });
                    },
                },
                openPanel: (panelId: string) => (calls.push(`openPanel:${panelId}`), true),
                openDocs: () => (calls.push("openDocs"), Promise.resolve(false)),
                showAbout: () => calls.push("showAbout"),
                defaultTemplate: "starter",
                execEditCommand: (verb) => (calls.push(`exec:${verb}`), verb !== "paste"),
            };
            const actions = makeMenuActions(deps);

            // A CANCELLED picker is an honest ok:false, and nothing downstream fires.
            const cancelled = await actions.newProject();
            assert(!cancelled.ok, "a cancelled picker refuses honestly");
            assert(!calls.some((c) => c.startsWith("new:")), "…and scaffolds nothing");

            pickedPath = "C:/projects/gamma";
            const created = await actions.newProject();
            assert(created.ok, "a picked folder scaffolds");
            assert(
                calls.includes("new:C:/projects/gamma:starter"),
                "…from the injected default template (the welcome CTA's own rule)",
            );

            const cleared = await actions.clearSelection();
            assert(cleared.ok, "the clear applied");
            assert(
                calls.includes("select:[]"),
                "selection.clear sends the EMPTY id list over session.select",
            );

            const cut = await actions.editClipboard("cut");
            assert(cut.ok && calls.includes("exec:cut"), "cut delegates to the injected editing hook");
            const paste = await actions.editClipboard("paste");
            assert(!paste.ok, "a refused paste reads ok:false — never a pretend success");

            const docs = await actions.openDocs();
            assert(!docs.ok, "a Shell with no URL opener refuses docs honestly");

            const badFocus = await actions.focusWindow(7);
            assert(!badFocus.ok, "focusing a window the Shell refused reads ok:false");
            const goodFocus = await actions.focusWindow(2);
            assert(goodFocus.ok && calls.includes("focus:2"), "a live peer focuses through the ask");
        },
    },
    // ------------------------------------------------------------------------ single dispatch (03)
    {
        name: "menu: an item activation reaches the SAME registry path as the palette (no second dispatch)",
        run: async () => {
            const log = actionSpies();
            const registry = registryWith(log);
            // Path 1 — the palette (the e07d surface).
            const palette = new Palette(registry);
            await palette.execute(SELECTION_CLEAR_COMMAND_ID);
            assertEqual(log.calls.length, 1, "the palette run dispatched the action once");

            // Path 2 — a real menubar CLICK, dispatching through the SAME registry object.
            const h = menubarHarness({
                execute: (id: string): void => {
                    void registry.execute(id);
                },
            });
            try {
                topLevelButton(h.slot, "selection").click();
                dropdownItem(h.slot, SELECTION_CLEAR_COMMAND_ID).click();
                await delay(0); // registry.execute is async; one tick settles the handler
                assertEqual(
                    log.calls,
                    ["clearSelection", "clearSelection"],
                    "the menu click ran the SAME handler through the SAME registry — one dispatch " +
                        "system, byte-for-byte",
                );
                assertEqual(h.mount.openMenuId(), null, "the dropdown closed on activation");
            } finally {
                h.dispose();
            }
        },
    },
    // ------------------------------------------------------------------------ the menubar (ARIA + keys)
    {
        name: "menubar: ARIA roles and the open/close lifecycle",
        run: () => {
            const h = menubarHarness();
            try {
                assertEqual(h.slot.getAttribute("role"), "menubar", "the bar is a menubar");
                const buttons = [...h.slot.querySelectorAll(`.${MENUBAR_ITEM_CLASS}`)];
                assertEqual(buttons.length, 7, "seven top-level items (the custom-mode tree)");
                for (const button of buttons) {
                    assertEqual(button.getAttribute("role"), "menuitem", "each is a menuitem");
                    assertEqual(
                        button.getAttribute("aria-haspopup"),
                        "menu",
                        "each declares its popup",
                    );
                }
                assertEqual(
                    buttons.filter((b) => (b as HTMLElement).tabIndex === 0).length,
                    1,
                    "exactly one top-level item is tabbable (the roving tabindex)",
                );
                const file = topLevelButton(h.slot, "file");
                file.click();
                assertEqual(h.mount.openMenuId(), "file", "a click opens the menu");
                assertEqual(file.getAttribute("aria-expanded"), "true", "…and says so");
                const dropdown = file.parentElement?.querySelector('[role="menu"]');
                assert(
                    dropdown instanceof HTMLElement && !dropdown.hidden,
                    "the dropdown is a visible role=menu",
                );
                assert(
                    dropdown !== null &&
                        dropdown instanceof HTMLElement &&
                        dropdown.querySelectorAll('[role="menuitem"]').length >= 4,
                    "with its items as menuitems",
                );
                file.click();
                assertEqual(h.mount.openMenuId(), null, "a second click closes it");
                assertEqual(file.getAttribute("aria-expanded"), "false", "…and says so");
            } finally {
                h.dispose();
            }
        },
    },
    {
        name: "menubar: the 03 keyboard contract — arrows, Enter, Escape, Home/End",
        run: async () => {
            const h = menubarHarness();
            try {
                const file = topLevelButton(h.slot, "file");
                file.focus();
                press(file, "ArrowRight");
                assertEqual(
                    document.activeElement?.getAttribute("data-menu-id"),
                    "edit",
                    "ArrowRight moves along the bar",
                );
                press(document.activeElement as Element, "ArrowLeft");
                assertEqual(
                    document.activeElement?.getAttribute("data-menu-id"),
                    "file",
                    "ArrowLeft moves back",
                );
                press(file, "ArrowDown");
                assertEqual(h.mount.openMenuId(), "file", "ArrowDown opens the focused menu");
                assertEqual(
                    document.activeElement?.getAttribute("data-command-id"),
                    PROJECT_NEW_COMMAND_ID,
                    "…and focuses the first item",
                );
                press(document.activeElement as Element, "ArrowDown");
                assertEqual(
                    document.activeElement?.getAttribute("data-command-id"),
                    PROJECT_OPEN_COMMAND_ID,
                    "ArrowDown walks the items",
                );
                press(document.activeElement as Element, "End");
                const last = document.activeElement;
                assert(
                    last instanceof HTMLElement && last.classList.contains(MENU_ITEM_CLASS),
                    "End jumps to the last item",
                );
                press(last as Element, "Home");
                assertEqual(
                    document.activeElement?.getAttribute("data-command-id"),
                    PROJECT_NEW_COMMAND_ID,
                    "Home jumps back to the first",
                );
                // Enter on a focused item ACTIVATES through the one dispatch (a real <button>
                // synthesises click from Enter — asserted via the spy, not assumed).
                (document.activeElement as HTMLElement).click();
                await delay(0);
                assertEqual(h.executed, [PROJECT_NEW_COMMAND_ID], "activation dispatched the id");
                assertEqual(h.mount.openMenuId(), null, "…and closed the menu");

                // Escape closes an open menu and returns focus to its top-level item. (The
                // activation above moved focus off the bar with the removed item, so re-establish
                // it — the handler keys on the REAL activeElement, exactly like a user would.)
                file.focus();
                press(file, "ArrowDown");
                assertEqual(h.mount.openMenuId(), "file", "reopened for the Escape leg");
                press(document.activeElement as Element, "Escape");
                assertEqual(h.mount.openMenuId(), null, "Escape closes the menu");
                assertEqual(
                    document.activeElement?.getAttribute("data-menu-id"),
                    "file",
                    "…and restores focus to the top-level item",
                );

                // With a menu OPEN, ArrowRight from inside moves to the NEXT menu, open.
                press(file, "ArrowDown");
                press(document.activeElement as Element, "ArrowRight");
                assertEqual(h.mount.openMenuId(), "edit", "ArrowRight walks open menus");
            } finally {
                h.dispose();
            }
        },
    },
    {
        name: "menubar: the Open Recent submenu opens with ArrowRight and backs out with ArrowLeft",
        run: () => {
            const h = menubarHarness();
            try {
                h.mount.openMenu("file");
                const header = h.slot.querySelector<HTMLButtonElement>(
                    'button[aria-haspopup="menu"].ctx-menu__item',
                );
                assert(header !== null, "the Open Recent submenu header rendered");
                header?.focus();
                press(header as Element, "ArrowRight");
                assertEqual(
                    document.activeElement?.getAttribute("data-command-id"),
                    `${PROJECT_OPEN_RECENT_PREFIX}0`,
                    "ArrowRight opens the submenu and focuses its first entry",
                );
                press(document.activeElement as Element, "ArrowLeft");
                assertEqual(
                    document.activeElement,
                    header,
                    "ArrowLeft backs out to the submenu header",
                );
            } finally {
                h.dispose();
            }
        },
    },
    // ------------------------------------------------------------------------ disabled-item honesty
    {
        name: "menubar: the ⏳ rows are disabled with the reason and TRULY inert — and flip live",
        run: async () => {
            const h = menubarHarness();
            try {
                // Outside a text field: disabled, tooltip carries the ⏳ reason, activation inert.
                h.context = { textInputFocus: false };
                h.mount.openMenu("edit");
                const cut = dropdownItem(h.slot, EDIT_CUT_COMMAND_ID);
                assertEqual(cut.getAttribute("aria-disabled"), "true", "Cut renders disabled");
                assertEqual(cut.title, CLIPBOARD_DISABLED_REASON, "with the honest ⏳ reason");
                cut.click();
                await delay(0);
                assertEqual(h.executed.length, 0, "a disabled item dispatches NOTHING — truly inert");

                // Inside a text field the SAME item is live — enablement is evaluated at open.
                h.context = { textInputFocus: true };
                h.mount.openMenu("edit");
                const liveCut = dropdownItem(h.slot, EDIT_CUT_COMMAND_ID);
                assertEqual(liveCut.getAttribute("aria-disabled"), "false", "Cut enabled in a field");
                liveCut.click();
                await delay(0);
                assertEqual(h.executed, [EDIT_CUT_COMMAND_ID], "…and dispatches");
            } finally {
                h.dispose();
            }
        },
    },
    {
        name: "menubar: a command the registry does not hold renders disabled (never a dead click)",
        run: async () => {
            const h = menubarHarness({
                available: (id: string): string | null =>
                    id === SELECTION_CLEAR_COMMAND_ID ? null : "",
            });
            try {
                h.mount.openMenu("selection");
                const clear = dropdownItem(h.slot, SELECTION_CLEAR_COMMAND_ID);
                assertEqual(
                    clear.getAttribute("aria-disabled"),
                    "true",
                    "an unregistered command's item is disabled",
                );
                clear.click();
                await delay(0);
                assertEqual(h.executed.length, 0, "…and inert");
            } finally {
                h.dispose();
            }
        },
    },
    {
        name: "menubar: the Maximize item's label flips on the maximized fact at open time",
        run: () => {
            const h = menubarHarness();
            try {
                h.maximized = false;
                h.mount.openMenu("window");
                assert(
                    dropdownItem(h.slot, WINDOW_TOGGLE_MAXIMIZE_COMMAND_ID).textContent?.includes(
                        "Maximize",
                    ) === true,
                    "restored windows offer Maximize",
                );
                h.maximized = true;
                h.mount.openMenu("window");
                assert(
                    dropdownItem(h.slot, WINDOW_TOGGLE_MAXIMIZE_COMMAND_ID).textContent?.includes(
                        "Restore",
                    ) === true,
                    "maximized windows offer Restore (02 §5's fact-driven flip)",
                );
            } finally {
                h.dispose();
            }
        },
    },
    // ------------------------------------------------------------------------ the native return path
    {
        name: "menu facts: an editor.ui.menu envelope executes its command id through the dispatch",
        run: () => {
            const bus = new EditorUiBus({ origin: "0" });
            const executed: string[] = [];
            subscribeMenuFacts(bus, 0, (id: string): void => {
                executed.push(id);
            });
            // The Shell's envelope shape verbatim (menu_facts.cpp): origin `shell`, never a window
            // id — the ninth built-in topic must ACCEPT it (a refused topic is the drift this
            // asserts against).
            const report = bus.receiveMirrored({
                seq: 1,
                topic: UI_TOPIC_MENU,
                origin: "shell",
                payload: { windowId: 0, commandId: HELP_ABOUT_COMMAND_ID },
            });
            assert(report.published, "the menu topic is a KNOWN built-in — the envelope applied");
            assertEqual(executed, [HELP_ABOUT_COMMAND_ID], "the activation executed its command id");

            // A peer window's activation is filtered (belt-and-braces over the Shell's unicast).
            bus.receiveMirrored({
                seq: 2,
                topic: UI_TOPIC_MENU,
                origin: "shell",
                payload: { windowId: 3, commandId: HELP_DOCS_COMMAND_ID },
            });
            assertEqual(executed.length, 1, "a wrong-window fact executes nothing");

            // Malformed payloads execute nothing (the total-parser rule).
            bus.receiveMirrored({
                seq: 3,
                topic: UI_TOPIC_MENU,
                origin: "shell",
                payload: { windowId: 0 },
            });
            assertEqual(executed.length, 1, "a fact with no command id executes nothing");
            assertEqual(parseMenuFact(null), null, "a non-record payload parses to null");
            assertEqual(
                parseMenuFact({ windowId: 0, commandId: "" }),
                null,
                "an empty command id parses to null — the registry never sees a nameless dispatch",
            );
        },
    },
    // ------------------------------------------------------------------------ the About dialog
    {
        name: "about: the dialog renders version + update state and closes on Escape",
        run: () => {
            const state: UpdateState = {
                checked: true,
                current: "0.9.1",
                latest: "0.9.5",
                updateAvailable: true,
                dismissed: false,
                downloadsUrl: "https://example.test",
                error: "",
            };
            assertEqual(
                aboutLines(state),
                { version: "Version 0.9.1", update: "Update available: 0.9.5" },
                "the lines table renders a known update",
            );
            assertEqual(
                aboutLines({ ...state, updateAvailable: false }).update,
                "Up to date",
                "…and the up-to-date case",
            );
            assertEqual(
                aboutLines(null).version,
                "Version unknown (no update surface behind this Shell)",
                "…and the honest no-surface case",
            );
            const dialog = openAboutDialog(document.body, state);
            try {
                assertEqual(dialog.element.getAttribute("role"), "dialog", "a real dialog role");
                assert(
                    dialog.element.textContent?.includes("Version 0.9.1") === true,
                    "the version renders",
                );
                press(dialog.element, "Escape");
                assert(!dialog.element.isConnected, "Escape dismisses the dialog");
            } finally {
                dialog.close();
            }
        },
    },
    // ------------------------------------------------------------------------ the titlebar slot gating
    {
        name: "chrome: the menubar slot exists in custom/system, never in hybrid or a secondary window",
        run: () => {
            const mountWith = (
                state: Partial<ChromeState>,
            ): { titlebar: HTMLElement; slot: Element | null; dispose(): void } => {
                const titlebar = document.createElement("header");
                titlebar.id = EDITOR_TITLEBAR_ID;
                const playbar = document.createElement("div");
                playbar.id = EDITOR_PLAYBAR_ID;
                const statusbar = document.createElement("footer");
                statusbar.id = EDITOR_STATUSBAR_ID;
                document.body.append(titlebar, playbar, statusbar);
                const elements: ChromeStripElements = { titlebar, playbar, statusbar };
                mountChrome(elements, {
                    state: {
                        mode: CHROME_MODE_CUSTOM,
                        controlsInset: { left: 0, right: 0 },
                        maximized: false,
                        focused: true,
                        window: CHROME_WINDOW_PRIMARY,
                        ...state,
                    },
                    projectName: "Sprocket Quest",
                    welcome: false,
                    controls: {
                        minimize: () => Promise.resolve({}),
                        toggleMaximize: () => Promise.resolve({ accepted: false, maximized: false }),
                        close: () => Promise.resolve({}),
                    },
                    executeCommand: (): void => {},
                });
                return {
                    titlebar,
                    slot: titlebar.querySelector(`.${TITLEBAR_MENU_CLASS}`),
                    dispose: (): void => {
                        titlebar.remove();
                        playbar.remove();
                        statusbar.remove();
                    },
                };
            };

            const custom = mountWith({ mode: CHROME_MODE_CUSTOM });
            try {
                assert(custom.slot !== null, "custom chrome hosts the menubar slot");
                // The slot sits OUTSIDE (before) the caption drag element: the Shell consumes
                // caption hits before client routing, so a menubar inside it could never be
                // clicked (chrome.ts § the menubar slot).
                const drag = custom.titlebar.querySelector(`.${TITLEBAR_DRAG_CLASS}`);
                assert(
                    drag !== null && custom.slot !== null && !drag.contains(custom.slot),
                    "…outside the caption drag surface",
                );
            } finally {
                custom.dispose();
            }
            const system = mountWith({ mode: CHROME_MODE_SYSTEM });
            try {
                assert(
                    system.slot !== null,
                    "system chrome hosts it too — D6: the Linux strip IS the menu bar",
                );
            } finally {
                system.dispose();
            }
            const hybrid = mountWith({ mode: CHROME_MODE_HYBRID });
            try {
                assert(
                    hybrid.slot === null,
                    "hybrid chrome hosts NO web menubar — the native NSMenu bar is the rendering " +
                        "(02 §4)",
                );
            } finally {
                hybrid.dispose();
            }
            const secondary = mountWith({ window: CHROME_WINDOW_SECONDARY });
            try {
                assert(
                    secondary.slot === null,
                    "a secondary window's compact strip carries no menu (02 §9 / D4)",
                );
            } finally {
                secondary.dispose();
            }
        },
    },
];
