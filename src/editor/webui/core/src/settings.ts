// The `builtin.settings` panel (M9 e06d, design 06 §4 / C-F14 / 10 "switch theme <= 3 steps").
//
// WHAT IT IS. The one place a user changes how the editor behaves for THEM rather than for a project:
// pick a theme (applied live, no restart), find the keybindings file, and see update state. Three
// sections, one tab set, no settings tree — this panel is small on purpose, because everything the
// editor can be told to do already lives in the command palette (design 10: the palette surface IS the
// scriptable surface), and a second, parallel place to change behaviour would be a second contract.
//
// WHY EDITOR-CORE RENDERS IT (ContentType::local — the ONE panel that does; extension.h). Every other
// built-in is a C++ uitree model hydrated over the bridge (04 §4 / D17), and that is right for a panel
// whose subject is PROJECT state: one headless implementation serves CI and the live editor alike. This
// panel's subject is the RENDERER'S OWN state — the active theme IS the set of CSS custom properties on
// the editor-core document (06 §1) — so a C++ model of it could only be a lagging copy of something it
// cannot observe. Its a11y is gated in the `webui-ts-*` browser tier (settings.test.ts) over the REAL
// DOM, which `gui-a11y-coverage` requires by name for a local panel.
//
// BUILT FROM THE e06c KIT, and it is the kit's FIRST REAL CONSUMER (e06c2 shipped the twelve families
// of design 06 §3). Nothing here hand-rolls a control: tabs, fields, buttons, badges, chips, toasts and
// empty states all come from `@context-engine/editor-kit`, so this panel inherits the token discipline
// (`webui-kit-source-tokens`), the keyboard contracts, and the ARIA wiring the kit already proves — and
// a theme change re-tokens it for free, since kit components reference only custom properties.
//
// IT NEVER PERSISTS ANYTHING ITSELF (C-F14). Selecting a theme APPLIES it locally and REQUESTS the
// write; the Shell validates and writes `~/.context/config.json`. The two halves are deliberately
// separate — the visible switch must be instant, while the write may honestly fail (a read-only home,
// no home at all) — and the outcome is reported back through `reportSave` so the panel can say which
// happened rather than implying success. See config.ts for the request side and the gate that keeps
// this the only door.

import {
    createBadge,
    createButton,
    createChip,
    createEmptyState,
    createSelectField,
    createTabs,
    createTextField,
    createToastRegion,
    type FieldOption,
    type KitBadge,
    type KitToastRegion,
} from "../../kit/src/index.js";

/** The roster id this panel is registered under (gui/contract/src/builtin_roster.cpp). */
export const SETTINGS_PANEL_ID = "builtin.settings";

/** The panel root's class — `app.css` styles the layout; every CONTROL is the kit's. */
export const SETTINGS_ROOT_CLASS = "ctx-settings";

/** The tab ids, stable because the smoke and the tests select by them. */
export const SETTINGS_TAB_APPEARANCE = "appearance";
export const SETTINGS_TAB_KEYMAP = "keymap";
export const SETTINGS_TAB_UPDATES = "updates";

/** One theme the picker can offer. A projection of `ThemeEntry`, so this module needs no registry. */
export interface ThemeChoice {
    readonly id: string;
    readonly name: string;
    /** `builtin` / `user` / `package` — shown as a chip so a user knows where a theme came from. */
    readonly source: string;
    readonly highContrast: boolean;
}

/**
 * What the panel is currently able to say about persistence.
 *
 * `session-only` is NOT an error state: a host with no resolvable home directory genuinely cannot
 * remember a choice, and saying so plainly is better than a failed save the user cannot act on.
 */
export type SettingsSaveState = "idle" | "pending" | "saved" | "failed" | "session-only";

/** The result of a persist attempt, as `ConfigClient.setTheme` reports it (config.ts). */
export interface SettingsSaveReport {
    readonly stored: boolean;
    readonly diagnostic: string;
}

export interface SettingsPanelOptions {
    /** Every theme the registry holds, in listing order. */
    readonly themes: readonly ThemeChoice[];
    readonly activeThemeId: string;
    /** `~/.context/keybindings.json` as the Shell resolved it, or `""` when it could not (e07c). */
    readonly keybindingsPath: string;
    /** False when the Shell reported no writable config path — the panel then says "this session only". */
    readonly writable: boolean;
    /**
     * Apply + request persistence for a theme id.
     *
     * ONE callback for both halves, injected: this module must not know about the ThemeEngine (it would
     * then be untestable without a document) NOR about the bridge (it would then be a second write
     * path, which config.ts's gate forbids). boot.ts owns the wiring.
     */
    readonly onSelectTheme: (themeId: string) => void;
    /**
     * The theme id the HOST's `prefers-color-scheme` currently implies — what "Match system appearance"
     * resolves to at the moment it is pressed. Injected for the same reason: a probe is environment,
     * and this module asks for answers, not for the environment.
     */
    readonly systemThemeId: () => string;
}

export interface SettingsPanelMount {
    readonly element: HTMLElement;
    /** How many themes the picker offered — the falsifiable handle a test (and the smoke) reads. */
    readonly themeCount: number;
    /** The tab currently shown. */
    readonly activeTabId: string;
    /** The theme the picker shows as selected. */
    readonly selectedThemeId: string;
    readonly saveState: SettingsSaveState;
    /** Select a theme programmatically — the same path the `<select>` change handler takes. */
    selectTheme(themeId: string): void;
    /** Show a tab programmatically. */
    selectTab(tabId: string): void;
    /** Report the outcome of the persist request this panel triggered (boot.ts calls it). */
    reportSave(report: SettingsSaveReport): void;
}

/** Human label for a theme in the picker: its name, with high contrast called out (it matters). */
function themeLabel(choice: ThemeChoice): string {
    return choice.highContrast ? `${choice.name} (high contrast)` : choice.name;
}

/**
 * Build and mount the Settings panel into `container`, replacing its contents.
 *
 * Synchronous and DOM-only: everything asynchronous (applying a theme, requesting a write) is a
 * callback the caller owns. That is what makes the whole panel provable in the browser T1 tier without
 * a bridge, a Shell, or a timer — and it is why `reportSave` exists rather than the panel awaiting a
 * promise it would have to invent a loading state for.
 */
export function mountSettings(
    container: HTMLElement,
    options: SettingsPanelOptions,
): SettingsPanelMount {
    container.replaceChildren();

    const root = document.createElement("div");
    root.className = SETTINGS_ROOT_CLASS;
    // A labelled landmark, like every other panel body: a screen-reader user tabbing between docked
    // groups is told which panel focus moved into.
    root.setAttribute("role", "region");
    root.setAttribute("aria-label", "Settings");

    const themes = options.themes;
    let selectedThemeId = options.activeThemeId;
    let saveState: SettingsSaveState = options.writable ? "idle" : "session-only";

    // --- the save badge (kit: badges) ------------------------------------------------------------
    // LIVE, because its change is exactly the kind worth announcing: the user just made a choice and
    // the only evidence it was remembered is this word changing. That is the narrow case the kit's
    // `live` flag documents, not the default.
    const saveBadge: KitBadge = createBadge({
        label: options.writable ? "Not saved yet" : "This session only",
        tone: options.writable ? "idle" : "warn",
        live: true,
        accessibleLabel: "Theme save state",
    });

    // --- toasts (kit: toasts) --------------------------------------------------------------------
    // A failed WRITE is the one thing here a user must not miss: the theme changed on screen, so
    // without this the editor would look like it had saved. No auto-dismiss (the kit refuses to bake
    // one in), so the message survives being looked away from.
    const toasts: KitToastRegion = createToastRegion();

    const setSaveState = (next: SettingsSaveState, message: string, tone: "good" | "warn" | "bad" | "idle"): void => {
        saveState = next;
        saveBadge.setLabel(message);
        saveBadge.setTone(tone);
    };

    // --- appearance -------------------------------------------------------------------------------
    const appearance = document.createElement("div");
    appearance.className = "ctx-settings__section";

    const themeOptions: FieldOption[] = themes.map((choice) => ({
        value: choice.id,
        label: themeLabel(choice),
    }));

    // The source chip (kit: chips): where the SELECTED theme came from. A user with a hand-written
    // theme in `~/.context/themes/` needs to be able to tell it apart from a built-in with the same
    // name — the picker's label alone cannot say that.
    const sourceChip = createChip({
        label: sourceLabel(themes, selectedThemeId),
        tone: "idle",
    });

    const applySelection = (themeId: string): void => {
        if (themeId === "" || themeId === selectedThemeId) {
            return; // selecting what is already selected is not a change; do not re-apply or re-write
        }
        selectedThemeId = themeId;
        sourceChip.setLabel(sourceLabel(themes, themeId));
        if (options.writable) {
            setSaveState("pending", "Saving...", "idle");
        }
        options.onSelectTheme(themeId);
    };

    const picker = createSelectField({
        label: "Theme",
        description: "Applied immediately. Saved to your user config, so it is remembered next launch.",
        options: themeOptions,
        value: selectedThemeId,
        onChange: applySelection,
    });
    appearance.append(picker.element);

    const appearanceStatus = document.createElement("div");
    appearanceStatus.className = "ctx-settings__status";
    appearanceStatus.append(sourceChip.element, saveBadge.element);
    appearance.append(appearanceStatus);

    // The system-appearance button (kit: buttons). RESOLVES the host preference at the moment it is
    // pressed and selects that theme — it does not install a "follow the system forever" mode, which
    // would be a second, invisible source of truth for the active theme fighting the persisted one.
    const systemButton = createButton({
        label: "Match system appearance",
        tone: "default",
        onActivate: (): void => {
            const systemId = options.systemThemeId();
            if (systemId !== "") {
                picker.control.value = systemId;
                applySelection(systemId);
            }
        },
    });
    appearance.append(systemButton.element);

    // --- keymap -----------------------------------------------------------------------------------
    // A READ-ONLY path field (kit: fields) rather than a label: the user's next action is to open that
    // file in their own editor, and a real text control is what lets them select and copy the path.
    // editor-core cannot open it — it has no filesystem and no shell-execute — so claiming a button
    // that "opens" it would be a lie the panel cannot keep.
    const keymap = document.createElement("div");
    keymap.className = "ctx-settings__section";
    if (options.keybindingsPath === "") {
        keymap.append(
            createEmptyState({
                title: "No keybindings file location",
                description:
                    "This host has no home directory (no HOME / USERPROFILE), so per-user keybindings " +
                    "cannot be stored. The default keymap is in effect.",
            }).element,
        );
    } else {
        const pathField = createTextField({
            label: "Keybindings file",
            description:
                "Create or edit this file to override the default keymap. It is watched, so changes " +
                "apply without a restart.",
            value: options.keybindingsPath,
        });
        pathField.control.readOnly = true;
        keymap.append(pathField.element);
    }

    // --- updates ----------------------------------------------------------------------------------
    // The e14d update banner is not landed. An EMPTY STATE (kit: empty-states) says so in the panel's
    // own voice; inventing a "Check for updates" button that cannot check would be worse than the gap.
    const updates = document.createElement("div");
    updates.className = "ctx-settings__section";
    updates.append(
        createEmptyState({
            title: "No update channel configured",
            description:
                "This build does not check for updates. Update state will appear here once the " +
                "release channel is wired.",
        }).element,
    );

    // --- the tab set (kit: tabs) ------------------------------------------------------------------
    const tabs = createTabs({
        label: "Settings sections",
        activeId: SETTINGS_TAB_APPEARANCE,
        tabs: [
            { id: SETTINGS_TAB_APPEARANCE, label: "Appearance", content: appearance },
            { id: SETTINGS_TAB_KEYMAP, label: "Keymap", content: keymap },
            { id: SETTINGS_TAB_UPDATES, label: "Updates", content: updates },
        ],
    });
    root.append(tabs.element, toasts.element);
    container.append(root);

    return {
        element: root,
        themeCount: themes.length,
        get activeTabId(): string {
            return tabs.activeId;
        },
        get selectedThemeId(): string {
            return selectedThemeId;
        },
        get saveState(): SettingsSaveState {
            return saveState;
        },
        selectTheme: (themeId: string): void => {
            // Keep the control and the model in step when driven programmatically (the T2 smoke and
            // the T1 tier both take this path), exactly as a user's change event would leave them.
            picker.control.value = themeId;
            applySelection(themeId);
        },
        selectTab: (tabId: string): void => {
            tabs.select(tabId);
        },
        reportSave: (report: SettingsSaveReport): void => {
            if (!options.writable) {
                return; // nothing was requested; a report here would contradict the badge
            }
            if (report.stored) {
                setSaveState("saved", "Saved", "good");
                return;
            }
            setSaveState("failed", "Not saved", "bad");
            toasts.show({
                message:
                    report.diagnostic === ""
                        ? "The theme could not be saved to your user config."
                        : `The theme could not be saved: ${report.diagnostic}`,
                tone: "bad",
            });
        },
    };
}

/** The chip's text for `themeId`: its source, or a plain note when the id is not in the list. */
function sourceLabel(themes: readonly ThemeChoice[], themeId: string): string {
    const match = themes.find((choice) => choice.id === themeId);
    return match === undefined ? "unknown theme" : `${match.source} theme`;
}
