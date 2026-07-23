// The TABS family (M9 e06c2; design 06 §3). Net-new — the hydration vocabulary has no `tab` role, so
// there is no e06c1 primitive to build on and nothing to fork.
//
// ⚠ THIS IS *NOT* DOCKVIEW'S TAB STRIP, AND CONFLATING THE TWO IS A KNOWN, PAID-FOR MISTAKE. The
// docking engine renders its own `.dv-tab` chrome and WRITES INLINE CSSOM STYLES onto it at runtime;
// an inline style beats any stylesheet selector, so no specificity argument can win there — that
// surface is skinned through dockview's OWN `--dv-*` custom properties (the `DOCKVIEW_CHROME` map in
// `core/src/theme.ts`, and the `html .dockview-theme-dark` re-point in `app.css`), never by styling
// its elements. What this family provides is IN-PANEL tabs: the segmented control a panel body uses
// to switch between views of its own content, which dockview never touches. The distinction cost
// e06b two CI rounds, which is why `kit_components.test.ts` asserts the RENDERED computed style of a
// `.ctx-tabs__tab` rather than reasoning about the cascade.
//
// THE ARIA PATTERN IS THE POINT, NOT DECORATION. A tablist is one Tab stop, not N: exactly one tab
// carries `tabindex="0"` and the arrow keys move which (the roving pattern in `dom.ts`). Getting that
// wrong is not a cosmetic a11y nit — a panel with eight tabs would otherwise cost a keyboard user
// eight Tab presses to get past, every time. `Home`/`End` are part of the pattern, not an extra.
// Activation is AUTOMATIC (moving selects), which is the right choice for cheap, local view switches
// and is what the pattern recommends when the panel is already rendered.

import { createElement, rovingIndexFor, setText, uniqueId } from "./dom.js";

/** The family's root class. */
export const TABS_CLASS = "ctx-tabs";

export interface TabDefinition {
    /** Stable within one tabs instance; what `select()` and `onSelect` speak. */
    readonly id: string;
    readonly label: string;
    /** The panel body. A Node so a caller can hand over anything, including another kit component. */
    readonly content: Node;
}

export interface TabsOptions {
    readonly tabs: readonly TabDefinition[];
    /** Names the tablist for assistive technology — a bare "tab list" announcement helps nobody. */
    readonly label: string;
    readonly activeId?: string;
    readonly onSelect?: (id: string) => void;
}

export interface KitTabs {
    readonly element: HTMLElement;
    readonly activeId: string;
    select(id: string): void;
}

/** Build an in-panel tab set. Throws on an empty tab list — a tablist with no tabs is a bug. */
export function createTabs(options: TabsOptions): KitTabs {
    if (options.tabs.length === 0) {
        throw new Error("@context-engine/editor-kit: createTabs needs at least one tab");
    }
    const element = createElement("div", TABS_CLASS);
    const tablist = createElement("div", "ctx-tabs__list");
    tablist.setAttribute("role", "tablist");
    tablist.setAttribute("aria-label", options.label);
    tablist.setAttribute("aria-orientation", "horizontal");
    element.append(tablist);

    const buttons: HTMLButtonElement[] = [];
    const panels: HTMLElement[] = [];
    const ids: string[] = [];

    for (const tab of options.tabs) {
        const tabId = uniqueId("ctx-tab");
        const panelId = uniqueId("ctx-tabpanel");

        const button = createElement("button", "ctx-tabs__tab");
        button.type = "button";
        button.id = tabId;
        button.setAttribute("role", "tab");
        button.setAttribute("aria-controls", panelId);
        setText(button, tab.label);
        tablist.append(button);

        const panel = createElement("div", "ctx-tabs__panel");
        panel.id = panelId;
        panel.setAttribute("role", "tabpanel");
        panel.setAttribute("aria-labelledby", tabId);
        // The PANEL is focusable, not its contents: the pattern's contract is that Tab from the
        // selected tab lands in the panel it controls, and a panel whose body happens to start with
        // no focusable element would otherwise strand the user in the tab strip.
        panel.tabIndex = 0;
        panel.append(tab.content);
        element.append(panel);

        buttons.push(button);
        panels.push(panel);
        ids.push(tab.id);
    }

    let activeIndex = Math.max(
        0,
        options.activeId === undefined ? 0 : ids.indexOf(options.activeId),
    );

    function paint(index: number): void {
        for (let i = 0; i < buttons.length; i += 1) {
            const button = buttons[i];
            const panel = panels[i];
            const selected = i === index;
            if (button !== undefined) {
                button.setAttribute("aria-selected", selected ? "true" : "false");
                button.tabIndex = selected ? 0 : -1;
            }
            if (panel !== undefined) {
                panel.hidden = !selected;
            }
        }
    }

    function selectIndex(index: number, moveFocus: boolean): void {
        if (index === activeIndex && !moveFocus) {
            return;
        }
        activeIndex = index;
        // `paint` is what redistributes the roving tabindex, so focus can simply follow it.
        paint(index);
        if (moveFocus) {
            buttons[index]?.focus();
        }
        const id = ids[index];
        if (id !== undefined) {
            options.onSelect?.(id);
        }
    }

    for (let i = 0; i < buttons.length; i += 1) {
        buttons[i]?.addEventListener("click", () => {
            selectIndex(i, true);
        });
    }

    // The ARIA tablist roving-navigation pattern (ArrowLeft/Right/Home/End) is an INTRA-WIDGET focus
    // move, not an editor shortcut: it resolves to no command id and cannot be rebound, which is
    // exactly the line 05 §6 draws (the same justification hydration.ts's Enter/Space ARIA
    // activation carries).
    // key-handler-ok: ARIA tablist roving navigation — intra-widget focus, no command id.
    tablist.addEventListener("keydown", (event: KeyboardEvent) => {
        const next = rovingIndexFor(event.key, activeIndex, buttons.length, "horizontal");
        if (next === undefined) {
            return;
        }
        // Only once we KNOW the key is ours: swallowing an unhandled key would eat the editor's own
        // keymap dispatch for every other keystroke that reaches a focused tab.
        event.preventDefault();
        selectIndex(next, true);
    });

    // Paint only — a component must NOT steal focus the moment it is constructed. `paint` sets the
    // roving tabindex distribution, which is the part that matters before the user arrives.
    paint(activeIndex);

    return {
        element,
        get activeId(): string {
            return ids[activeIndex] ?? "";
        },
        select: (id: string): void => {
            const index = ids.indexOf(id);
            if (index >= 0) {
                selectIndex(index, false);
            }
        },
    };
}
