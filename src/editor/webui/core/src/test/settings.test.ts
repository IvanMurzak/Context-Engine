// T1 unit + a11y tests for the `builtin.settings` panel (settings.ts, M9 e06d).
//
// ⚠ THIS FILE IS THE PANEL'S ACCESSIBILITY GATE. `builtin.settings` is the one `ContentType::local`
// panel — editor-core renders it, so there is no C++ model for `context_gui_a11y_scan` to instantiate
// and it cannot appear in that harness's report. `gui-a11y-coverage` therefore REQUIRES its
// coverage-manifest line to declare `ts-a11y`, and `tools/a11y_scan.py` exempts it from the scanned-set
// cross-check on exactly that marker. The assertions below are what that marker BUYS: a labelled
// landmark, a real tablist with roving arrow-key focus, every control associated with a visible label,
// and a live region for the one state change worth announcing. They run in a REAL browser against the
// REAL DOM the user meets, which is strictly closer to the truth than a scan of a model.
//
// Everything asynchronous is injected, so every case here is synchronous and deterministic: the panel
// takes an `onSelectTheme` callback (boot.ts applies + requests the write) and is TOLD the outcome via
// `reportSave`. That is why there is no bridge, no ThemeEngine and no timer anywhere in this file.

import { assert, assertEqual, type TestCase } from "./harness.js";
import {
    SETTINGS_ROOT_CLASS,
    SETTINGS_TAB_APPEARANCE,
    SETTINGS_TAB_KEYMAP,
    SETTINGS_TAB_UPDATES,
    mountSettings,
    type SettingsPanelMount,
    type SettingsPanelOptions,
    type ThemeChoice,
} from "../settings.js";

const THEMES: readonly ThemeChoice[] = [
    { id: "builtin.dark", name: "Dark", source: "builtin", highContrast: false },
    { id: "builtin.light", name: "Light", source: "builtin", highContrast: false },
    {
        id: "builtin.high-contrast-dark",
        name: "High Contrast Dark",
        source: "builtin",
        highContrast: true,
    },
    { id: "user.mine", name: "Mine", source: "user", highContrast: false },
];

interface Harness {
    readonly mount: SettingsPanelMount;
    readonly container: HTMLElement;
    readonly selected: string[];
    readonly systemCalls: number[];
}

function mount(overrides: Partial<SettingsPanelOptions> = {}): Harness {
    const container = document.createElement("div");
    document.body.appendChild(container);
    const selected: string[] = [];
    const systemCalls: number[] = [];
    const panel = mountSettings(container, {
        themes: THEMES,
        activeThemeId: "builtin.dark",
        keybindingsPath: "C:/Users/dev/.context/keybindings.json",
        writable: true,
        onSelectTheme: (id: string): void => {
            selected.push(id);
        },
        systemThemeId: (): string => {
            systemCalls.push(1);
            return "builtin.light";
        },
        ...overrides,
    });
    return { mount: panel, container, selected, systemCalls };
}

function query<T extends Element>(root: ParentNode, selector: string): T {
    const found = root.querySelector<T>(selector);
    if (found === null) {
        throw new Error(`settings.test: no element matched ${selector}`);
    }
    return found;
}

export const settingsTests: readonly TestCase[] = [
    {
        name: "settings: the picker offers every registered theme, selecting the active one",
        run: (): void => {
            const h = mount();
            assertEqual(h.mount.themeCount, THEMES.length, "one option per registered theme");
            const select = query<HTMLSelectElement>(h.mount.element, "select");
            assertEqual(select.options.length, THEMES.length, "the DOM agrees with the model");
            assertEqual(select.value, "builtin.dark", "the ACTIVE theme is preselected");
            // High contrast is called out in the label: it is a different KIND of choice, not a
            // differently-named colour scheme, and a user who needs it must be able to find it.
            const labels = Array.from(select.options).map((option) => option.textContent ?? "");
            assert(
                labels.some((label) => label.includes("high contrast")),
                "the high-contrast theme is labelled as such",
            );
        },
    },
    {
        name: "settings: choosing a theme requests it exactly once and reports 'saving'",
        run: (): void => {
            const h = mount();
            h.mount.selectTheme("builtin.light");
            assertEqual(h.selected, ["builtin.light"], "the choice reached the caller once");
            assertEqual(h.mount.selectedThemeId, "builtin.light", "the model followed");
            assertEqual(query<HTMLSelectElement>(h.mount.element, "select").value, "builtin.light",
                        "the control followed too");
            assertEqual(h.mount.saveState, "pending", "the panel says a save is in flight");
        },
    },
    {
        name: "settings: re-choosing the ACTIVE theme is not a change",
        run: (): void => {
            // Re-applying would re-run the 350ms cross-fade and re-write the config for nothing; the
            // ordinary way a user produces this event is closing and reopening the select.
            const h = mount();
            h.mount.selectTheme("builtin.dark");
            assertEqual(h.selected, [], "no request was made");
            assertEqual(h.mount.saveState, "idle", "and no save was claimed");
        },
    },
    {
        name: "settings: the save outcome is REPORTED, never implied",
        run: (): void => {
            const h = mount();
            h.mount.selectTheme("builtin.light");
            h.mount.reportSave({ stored: true, diagnostic: "" });
            assertEqual(h.mount.saveState, "saved", "a successful write says so");
            const badge = query<HTMLElement>(h.mount.element, ".ctx-badge");
            assertEqual(badge.textContent, "Saved", "the live badge carries the verdict");

            h.mount.selectTheme("user.mine");
            h.mount.reportSave({ stored: false, diagnostic: "the disk is read-only" });
            assertEqual(h.mount.saveState, "failed", "a failed write says THAT");
            // The theme already changed on screen, so without a loud failure the editor would look
            // like it had saved. A toast is that loudness.
            const toast = query<HTMLElement>(h.mount.element, ".ctx-toast");
            assert(
                (toast.textContent ?? "").includes("read-only"),
                "the toast names the CAUSE the Shell reported",
            );
        },
    },
    {
        name: "settings: a host that cannot persist says so instead of pretending",
        run: (): void => {
            const h = mount({ writable: false });
            assertEqual(h.mount.saveState, "session-only", "no home directory is an honest state");
            const badge = query<HTMLElement>(h.mount.element, ".ctx-badge");
            assertEqual(badge.textContent, "This session only", "and it is on screen, not implied");
            h.mount.selectTheme("builtin.light");
            assertEqual(h.selected, ["builtin.light"], "the theme still switches for the session");
            assertEqual(h.mount.saveState, "session-only", "...and no save is ever claimed");
        },
    },
    {
        name: "settings: 'Match system appearance' resolves the host preference at press time",
        run: (): void => {
            const h = mount();
            const button = query<HTMLButtonElement>(h.mount.element, "button.ctx-button");
            button.click();
            assertEqual(h.systemCalls.length, 1, "the probe was asked when the button was pressed");
            assertEqual(h.selected, ["builtin.light"], "and its answer was applied");
            assertEqual(query<HTMLSelectElement>(h.mount.element, "select").value, "builtin.light",
                        "the picker reflects it");
        },
    },
    {
        name: "settings: the keymap section shows the file path, read-only",
        run: (): void => {
            const h = mount();
            h.mount.selectTab(SETTINGS_TAB_KEYMAP);
            const input = query<HTMLInputElement>(h.mount.element, "input[type=text]");
            assertEqual(
                input.value,
                "C:/Users/dev/.context/keybindings.json",
                "the Shell-resolved path is shown verbatim",
            );
            assert(input.readOnly, "it is read-only — editor-core cannot write that file either");
        },
    },
    {
        name: "settings: with no home directory the keymap section degrades to an empty state",
        run: (): void => {
            const h = mount({ keybindingsPath: "" });
            h.mount.selectTab(SETTINGS_TAB_KEYMAP);
            const empty = query<HTMLElement>(h.mount.element, ".ctx-empty-state");
            assert(
                (empty.textContent ?? "").includes("No keybindings file location"),
                "the panel explains the gap rather than showing an empty box",
            );
        },
    },
    {
        name: "settings: the updates section states the honest gap",
        run: (): void => {
            const h = mount();
            h.mount.selectTab(SETTINGS_TAB_UPDATES);
            const empty = query<HTMLElement>(h.mount.element, ".ctx-empty-state");
            assert(
                (empty.textContent ?? "").includes("No update channel"),
                "no invented 'Check for updates' button that could not check",
            );
        },
    },

    // --- the a11y gate this panel's coverage-manifest `ts-a11y` marker stands for -----------------
    {
        name: "settings a11y: the panel is a labelled landmark",
        run: (): void => {
            const h = mount();
            assertEqual(h.mount.element.getAttribute("role"), "region", "a landmark");
            assertEqual(h.mount.element.getAttribute("aria-label"), "Settings", "with a name");
            assert(
                h.mount.element.classList.contains(SETTINGS_ROOT_CLASS),
                "carrying the root class app.css lays out",
            );
        },
    },
    {
        name: "settings a11y: the sections are a named tablist with correct selection state",
        run: (): void => {
            const h = mount();
            const tablist = query<HTMLElement>(h.mount.element, "[role=tablist]");
            assert(
                (tablist.getAttribute("aria-label") ?? "") !== "",
                "a bare 'tab list' announcement helps nobody",
            );
            const tabs = Array.from(tablist.querySelectorAll<HTMLElement>("[role=tab]"));
            assertEqual(tabs.length, 3, "Appearance / Keymap / Updates");
            assertEqual(h.mount.activeTabId, SETTINGS_TAB_APPEARANCE, "Appearance opens first");
            assertEqual(
                tabs[0]?.getAttribute("aria-selected"),
                "true",
                "the open tab is the selected one",
            );
            h.mount.selectTab(SETTINGS_TAB_UPDATES);
            assertEqual(h.mount.activeTabId, SETTINGS_TAB_UPDATES, "selection follows");
            assertEqual(tabs[2]?.getAttribute("aria-selected"), "true", "and so does the ARIA state");
            assertEqual(tabs[0]?.getAttribute("aria-selected"), "false", "exactly one is selected");
        },
    },
    {
        name: "settings a11y: the tablist is operable by keyboard alone",
        run: (): void => {
            // The kit's roving index, exercised through a REAL key event rather than assumed: a
            // keyboard-only user reaches the sections with arrows, and nothing else in this panel
            // would surface a regression there.
            const h = mount();
            const tablist = query<HTMLElement>(h.mount.element, "[role=tablist]");
            const tabs = Array.from(tablist.querySelectorAll<HTMLElement>("[role=tab]"));
            tabs[0]?.focus();
            tabs[0]?.dispatchEvent(
                new KeyboardEvent("keydown", { key: "ArrowRight", bubbles: true }),
            );
            assertEqual(
                h.mount.activeTabId,
                SETTINGS_TAB_KEYMAP,
                "ArrowRight moved to the next section",
            );
            assertEqual(document.activeElement, tabs[1], "and focus moved with it");
        },
    },
    {
        name: "settings a11y: every control is associated with a visible label",
        run: (): void => {
            const h = mount();
            const select = query<HTMLSelectElement>(h.mount.element, "select");
            const selectLabel = query<HTMLLabelElement>(h.mount.element, `label[for="${select.id}"]`);
            assertEqual(selectLabel.textContent, "Theme", "the picker is named");
            assert(
                (select.getAttribute("aria-describedby") ?? "") !== "",
                "and described, so the persistence behaviour is announced with it",
            );

            h.mount.selectTab(SETTINGS_TAB_KEYMAP);
            const input = query<HTMLInputElement>(h.mount.element, "input[type=text]");
            const inputLabel = query<HTMLLabelElement>(h.mount.element, `label[for="${input.id}"]`);
            assertEqual(inputLabel.textContent, "Keybindings file", "the path field is named");
        },
    },
    {
        name: "settings a11y: the save state is a live region",
        run: (): void => {
            // The narrow case the kit's `live` flag documents: the user just made a choice, and the
            // only evidence it was remembered is this word changing.
            const h = mount();
            const badge = query<HTMLElement>(h.mount.element, ".ctx-badge");
            assertEqual(badge.tagName.toLowerCase(), "output", "an output element");
            assertEqual(badge.getAttribute("aria-label"), "Theme save state", "with a stable name");
        },
    },
];
