// The command palette VIEW, JS side (M9 e07d, design 05 §6 / 04 / 10). The DOM half of the palette.
//
// A thin overlay — a filter input over a results listbox — that REFLECTS the `Palette` model and shows
// each command's introspected docs (05 §6). It owns NO command logic: it forwards the typed query to
// the model, renders the model's ranked results, and executes the chosen command through the model
// (which delegates to the ONE registry). Keeping the logic in the model is what lets the browser-free
// tier unit-test ranking/filtering while this file covers only the DOM the browser tier drives.
//
// CSP. The served document carries `script-src 'self'` (no inline script), so this overlay is built
// with `document.createElement` + CSS CLASSES declared in `app/app.css` — never an inline `<script>`
// and never a string of HTML. Its ARIA shape is the combobox/listbox pattern, so it is keyboard-
// operable while open (Up/Down move the selection, Enter runs it, Escape closes) — the palette IS the
// universal keyboard path to every command (R-A11Y-001 / R-CLI-001).

import type { Palette, PaletteEntry } from "./palette.js";
import type { WhenContext } from "./when.js";

/** The class names the overlay uses — declared in `app/app.css`. One place so a rename can't drift. */
export const PALETTE_ROOT_CLASS = "ctx-palette";
export const PALETTE_OPEN_CLASS = "ctx-palette--open";
export const PALETTE_INPUT_CLASS = "ctx-palette__input";
export const PALETTE_LIST_CLASS = "ctx-palette__list";
export const PALETTE_ITEM_CLASS = "ctx-palette__item";
export const PALETTE_ITEM_SELECTED_CLASS = "ctx-palette__item--selected";
export const PALETTE_ITEM_TITLE_CLASS = "ctx-palette__title";
export const PALETTE_ITEM_DOC_CLASS = "ctx-palette__doc";
export const PALETTE_EMPTY_CLASS = "ctx-palette__empty";

export interface PaletteViewOptions {
    /** Where the overlay mounts (the document body in the app; a test container in the tier). */
    readonly host: HTMLElement;
    readonly palette: Palette;
    /**
     * The resolved when-context to filter by, read fresh on every render. Injected because the real
     * `editor.ui` bus (05 §5) is a later seam — the app passes the stubbed resolver today and swaps
     * only this provider when it lands, with no change to the palette.
     */
    readonly contextProvider: () => WhenContext;
}

/**
 * The palette overlay.
 *
 * Lifecycle mirrors the rest of editor-core's DOM owners (HydrationRuntime, PanelHost): `mount` builds
 * the DOM and binds listeners, `sync` reflects the model, `dispose` removes both. It is total against a
 * missing `document` so importing the module never throws outside a browser (the barrel re-export runs
 * at load); a caller that never `mount`s it costs nothing.
 */
export class PaletteView {
    readonly #palette: Palette;
    readonly #contextProvider: () => WhenContext;
    readonly #host: HTMLElement;
    #root: HTMLElement | null = null;
    #input: HTMLInputElement | null = null;
    #list: HTMLElement | null = null;
    #entries: readonly PaletteEntry[] = [];
    #selected = 0;
    #disposed = false;

    constructor(options: PaletteViewOptions) {
        this.#palette = options.palette;
        this.#contextProvider = options.contextProvider;
        this.#host = options.host;
    }

    /** True once mounted and not yet disposed. */
    get mounted(): boolean {
        return this.#root !== null && !this.#disposed;
    }

    /** The currently highlighted entry, or `undefined` when the list is empty. */
    get selectedEntry(): PaletteEntry | undefined {
        return this.#entries[this.#selected];
    }

    /** Build the overlay DOM, bind its listeners, and render the (closed) initial state. */
    mount(): void {
        if (this.#disposed || this.#root !== null || typeof document === "undefined") {
            return;
        }
        const root = document.createElement("div");
        root.className = PALETTE_ROOT_CLASS;
        root.setAttribute("role", "dialog");
        root.setAttribute("aria-label", "Command Palette");
        root.hidden = true;

        const input = document.createElement("input");
        input.className = PALETTE_INPUT_CLASS;
        input.type = "text";
        input.setAttribute("role", "combobox");
        input.setAttribute("aria-expanded", "true");
        input.setAttribute("aria-controls", "ctx-palette-list");
        input.setAttribute("aria-autocomplete", "list");
        input.setAttribute("placeholder", "Type a command…");
        input.setAttribute("spellcheck", "false");

        const list = document.createElement("ul");
        list.className = PALETTE_LIST_CLASS;
        list.id = "ctx-palette-list";
        list.setAttribute("role", "listbox");
        list.setAttribute("aria-label", "Commands");

        root.appendChild(input);
        root.appendChild(list);
        this.#host.appendChild(root);

        input.addEventListener("input", this.#onInput);
        // key-handler-ok: palette-local list navigation/activation while OPEN (ARIA combobox), not a
        // global shortcut — it dispatches the chosen command through the ONE registry, never bypassing
        // the keymap/command path (05 §6). The palette is OPENED by a keymap-bound command.
        input.addEventListener("keydown", this.#onKeyDown);

        this.#root = root;
        this.#input = input;
        this.#list = list;
        this.sync();
    }

    /**
     * Reflect the model: show/hide the overlay by its open state, and (when open) re-render the ranked
     * results for the current query + context. Cheap enough to call on every keystroke.
     */
    sync(): void {
        if (this.#root === null || this.#input === null || this.#disposed) {
            return;
        }
        const open = this.#palette.isOpen;
        this.#root.hidden = !open;
        this.#root.classList.toggle(PALETTE_OPEN_CLASS, open);
        if (!open) {
            this.#entries = [];
            return;
        }
        if (this.#input.value !== this.#palette.query) {
            this.#input.value = this.#palette.query;
        }
        this.#entries = this.#palette.results(this.#contextProvider());
        if (this.#selected >= this.#entries.length) {
            this.#selected = this.#entries.length === 0 ? 0 : this.#entries.length - 1;
        }
        this.#renderList();
        this.#input.focus();
    }

    /** Dispose: remove listeners and the overlay DOM. Idempotent. */
    dispose(): void {
        if (this.#disposed) {
            return;
        }
        this.#disposed = true;
        this.#input?.removeEventListener("input", this.#onInput);
        this.#input?.removeEventListener("keydown", this.#onKeyDown);
        this.#root?.remove();
        this.#root = null;
        this.#input = null;
        this.#list = null;
        this.#entries = [];
    }

    // --------------------------------------------------------------------------- listeners

    readonly #onInput = (): void => {
        if (this.#input === null) {
            return;
        }
        this.#palette.setQuery(this.#input.value);
        this.#selected = 0;
        this.sync();
    };

    readonly #onKeyDown = (event: KeyboardEvent): void => {
        switch (event.key) {
            case "Escape":
                event.preventDefault();
                this.#palette.close();
                this.sync();
                return;
            case "ArrowDown":
                event.preventDefault();
                this.#move(1);
                return;
            case "ArrowUp":
                event.preventDefault();
                this.#move(-1);
                return;
            case "Enter":
                event.preventDefault();
                void this.#activateSelected();
                return;
            default:
                return;
        }
    };

    #move(delta: number): void {
        if (this.#entries.length === 0) {
            return;
        }
        const count = this.#entries.length;
        this.#selected = (this.#selected + delta + count) % count;
        this.#renderList();
    }

    async #activateSelected(): Promise<void> {
        const entry = this.selectedEntry;
        if (entry === undefined) {
            return;
        }
        // Execute through the model (→ the ONE registry), which also closes the palette.
        await this.#palette.execute(entry.command.id);
        this.sync();
    }

    // --------------------------------------------------------------------------- rendering

    #renderList(): void {
        if (this.#list === null) {
            return;
        }
        this.#list.replaceChildren();
        if (this.#entries.length === 0) {
            const empty = document.createElement("li");
            empty.className = PALETTE_EMPTY_CLASS;
            empty.setAttribute("role", "option");
            empty.setAttribute("aria-disabled", "true");
            empty.textContent = "No matching commands";
            this.#list.appendChild(empty);
            return;
        }
        this.#entries.forEach((entry, index) => {
            const item = document.createElement("li");
            item.className = PALETTE_ITEM_CLASS;
            item.setAttribute("role", "option");
            item.setAttribute("data-command-id", entry.command.id);
            const selected = index === this.#selected;
            item.setAttribute("aria-selected", selected ? "true" : "false");
            item.classList.toggle(PALETTE_ITEM_SELECTED_CLASS, selected);

            const title = document.createElement("span");
            title.className = PALETTE_ITEM_TITLE_CLASS;
            title.textContent = entry.command.title;

            // The introspected docs the entry carries (05 §6 "palette entries carry the docs"): a
            // one-line summary shown under the title, so the palette is self-describing.
            const doc = document.createElement("span");
            doc.className = PALETTE_ITEM_DOC_CLASS;
            doc.textContent = entry.command.docs.summary;

            item.appendChild(title);
            item.appendChild(doc);
            // key-handler-ok: a pointer click on a result activates it via the registry (same command
            // path as Enter); not a key handler at all — click parity for the a11y activation contract.
            item.addEventListener("click", () => {
                this.#selected = index;
                void this.#activateSelected();
            });
            this.#list?.appendChild(item);
        });
    }
}
