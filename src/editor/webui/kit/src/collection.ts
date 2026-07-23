// The COLLECTION families (M9 e06c2; design 06 §3): TREES, LISTS and TABLES.
//
// Grouped in one module because they share a mechanism, not merely a shape — each renders a caller's
// data as semantic markup whose STRUCTURE is the accessibility contract (`role="tree"` + `aria-level`,
// a real `<ul>`, a real `<table>` with header scopes). Splitting them would have duplicated that
// reasoning three times; the families stay separately named in the kit's roster.
//
// TREES AND LISTS BUILD ON e06c1's ROLE PRIMITIVES (`tree`/`treeitem`, `list`/`listitem`), so an
// authored tree paints exactly like the Scene tree a C++ panel model hydrates into: same ink, same
// hover step, same focus ring, decided once in `kit.css`.
//
// ⚠ THE TWO COLLECTIONS DELIBERATELY DIFFER IN THEIR KEYBOARD MODEL, and the difference is a
// decision rather than an inconsistency.
//
//   * A TREE is a composite widget: it owns ONE tab stop and the arrow keys walk it (the ARIA tree
//     pattern). A tree of 400 scene entities that put 400 stops in the tab order would be unusable
//     for exactly the users the pattern exists to serve.
//   * A LIST's items, when activatable, are REAL `<button>` elements and each is its own tab stop.
//     That is not a weaker choice — it needs no roving state, no bespoke key handling and no ARIA
//     invention, and the browser gives Enter/Space activation for free. It suits the lists the editor
//     actually has (recent projects, a settings section index), which are short.
//
// A TABLE is not a widget at all: it is DATA, and the accessible answer is a real `<table>` with a
// `<caption>`, `scope`d headers and `aria-sort` on the sorted column — which screen readers navigate
// natively far better than any re-implementation. The only interactive part is the sort control, and
// that is a real `<button>` inside the `<th>` for the same reason.

import { createElement, focusRoving, rovingIndexFor, setText } from "./dom.js";
import { requireWidgetClass } from "./widgets.js";

/** The roster root classes (mirrored in `index.ts`'s family table, which the coverage gate reads). */
export const TREE_CLASS = "ctx-tree";
export const LIST_CLASS = "ctx-list";
export const TABLE_CLASS = "ctx-table";

// ------------------------------------------------------------------------------------------ trees

export interface TreeNode {
    readonly id: string;
    readonly label: string;
    readonly children?: readonly TreeNode[];
    /** Whether a node WITH children starts open. Ignored for a leaf. */
    readonly expanded?: boolean;
}

export interface TreeOptions {
    readonly nodes: readonly TreeNode[];
    /** Names the tree for assistive technology. */
    readonly label: string;
    readonly onActivate?: (id: string) => void;
    readonly onToggle?: (id: string, expanded: boolean) => void;
}

export interface KitTree {
    readonly element: HTMLElement;
    /** The id of the item that currently holds the tree's single tab stop, or `""` when empty. */
    readonly focusedId: string;
    /** Move the tab stop (and focus) to an item by id. No-op for an unknown or hidden id. */
    focus(id: string): void;
}

/** Is this treeitem inside a collapsed group? Collapsed groups are `hidden`, so ask the ancestors. */
function isVisibleItem(item: HTMLElement, root: HTMLElement): boolean {
    let node: HTMLElement | null = item.parentElement;
    while (node !== null && node !== root) {
        if (node instanceof HTMLElement && node.hidden) {
            return false;
        }
        node = node.parentElement;
    }
    return true;
}

/**
 * Build an accessible tree.
 *
 * `aria-level` / `aria-setsize` / `aria-posinset` are written explicitly even though the DOM nesting
 * implies them: assistive technology derives them from the nesting only when the group markup is
 * exactly right, and an editor tree is the one place a user MUST be told "3 of 17, two levels deep"
 * rather than left to count.
 */
export function createTree(options: TreeOptions): KitTree {
    const element = createElement("ul", requireWidgetClass("tree"), TREE_CLASS);
    element.setAttribute("role", "tree");
    element.setAttribute("aria-label", options.label);

    const items: HTMLElement[] = [];
    const idOf = new Map<HTMLElement, string>();

    function build(nodes: readonly TreeNode[], parent: HTMLElement, level: number): void {
        nodes.forEach((node, index) => {
            const item = createElement("li", requireWidgetClass("treeitem"), "ctx-tree__item");
            item.setAttribute("role", "treeitem");
            item.setAttribute("aria-level", String(level));
            item.setAttribute("aria-setsize", String(nodes.length));
            item.setAttribute("aria-posinset", String(index + 1));
            item.tabIndex = -1;
            idOf.set(item, node.id);

            const label = createElement("span", "ctx-tree__label");
            setText(label, node.label);
            const children = node.children ?? [];
            if (children.length > 0) {
                const twisty = createElement("span", "ctx-tree__twisty");
                // `aria-hidden` because the EXPANDED STATE is already announced by `aria-expanded` on
                // the item; a decorative marker that also announced it would double up.
                twisty.setAttribute("aria-hidden", "true");
                item.append(twisty);
            }
            item.append(label);
            parent.append(item);
            items.push(item);

            if (children.length > 0) {
                const expanded = node.expanded === true;
                item.setAttribute("aria-expanded", expanded ? "true" : "false");
                const group = createElement("ul", "ctx-tree__group");
                group.setAttribute("role", "group");
                group.hidden = !expanded;
                item.append(group);
                build(children, group, level + 1);
            }
        });
    }

    build(options.nodes, element, 1);

    let focusedIndex = 0;

    function visibleItems(): readonly HTMLElement[] {
        return items.filter((item) => isVisibleItem(item, element));
    }

    function setExpanded(item: HTMLElement, expanded: boolean): void {
        if (item.getAttribute("aria-expanded") === null) {
            return;
        }
        item.setAttribute("aria-expanded", expanded ? "true" : "false");
        const group = item.querySelector<HTMLElement>(":scope > .ctx-tree__group");
        if (group !== null) {
            group.hidden = !expanded;
        }
        const id = idOf.get(item);
        if (id !== undefined) {
            options.onToggle?.(id, expanded);
        }
    }

    function moveTo(index: number): void {
        const visible = visibleItems();
        const target = focusRoving(visible, index);
        if (target === undefined) {
            return;
        }
        // Items outside the visible set must not keep a stale `tabindex="0"`, or expanding a group
        // would silently leave the tree with two tab stops.
        for (const item of items) {
            if (item !== target) {
                item.tabIndex = -1;
            }
        }
        focusedIndex = visible.indexOf(target);
    }

    // key-handler-ok: the ARIA tree pattern's INTRA-WIDGET navigation (Arrow keys / Home / End /
    // Enter activation on the focused item). It resolves to no command id, cannot be rebound and
    // never leaves the widget — the same line 05 §6 draws for hydration.ts's Enter/Space activation.
    element.addEventListener("keydown", (event: KeyboardEvent) => {
        const visible = visibleItems();
        const current = Math.max(0, visible.findIndex((item) => item.tabIndex === 0));
        const item = visible[current];
        if (item === undefined) {
            return;
        }
        const expandable = item.getAttribute("aria-expanded") !== null;
        const expanded = item.getAttribute("aria-expanded") === "true";

        if (event.key === "ArrowRight" && expandable && !expanded) {
            event.preventDefault();
            setExpanded(item, true);
            return;
        }
        if (event.key === "ArrowLeft" && expandable && expanded) {
            event.preventDefault();
            setExpanded(item, false);
            return;
        }
        if (event.key === "Enter" || event.key === " ") {
            event.preventDefault();
            const id = idOf.get(item);
            if (id !== undefined) {
                options.onActivate?.(id);
            }
            return;
        }
        const next = rovingIndexFor(event.key, current, visible.length, "vertical");
        if (next === undefined) {
            return;
        }
        event.preventDefault();
        moveTo(next);
    });

    element.addEventListener("click", (event: MouseEvent) => {
        const target = event.target;
        if (!(target instanceof Element)) {
            return;
        }
        const item = target.closest<HTMLElement>('[role="treeitem"]');
        if (item === null || !element.contains(item)) {
            return;
        }
        const visible = visibleItems();
        const index = visible.indexOf(item);
        if (index >= 0) {
            moveTo(index);
        }
        if (target.classList.contains("ctx-tree__twisty")) {
            setExpanded(item, item.getAttribute("aria-expanded") !== "true");
            return;
        }
        const id = idOf.get(item);
        if (id !== undefined) {
            options.onActivate?.(id);
        }
    });

    // The tree owns ONE tab stop; give it to the first item without stealing focus at construction.
    const first = visibleItems()[0];
    if (first !== undefined) {
        first.tabIndex = 0;
    }

    return {
        element,
        get focusedId(): string {
            const item = visibleItems()[focusedIndex];
            return item === undefined ? "" : (idOf.get(item) ?? "");
        },
        focus: (id: string): void => {
            const visible = visibleItems();
            const index = visible.findIndex((item) => idOf.get(item) === id);
            if (index >= 0) {
                moveTo(index);
            }
        },
    };
}

// ------------------------------------------------------------------------------------------- lists

export interface ListItem {
    readonly id: string;
    readonly label: string;
    /** A dimmed second line — a path, a description. */
    readonly detail?: string;
}

export interface ListOptions {
    readonly items: readonly ListItem[];
    readonly label: string;
    /** When given, every row becomes a real `<button>` (see the module header's keyboard note). */
    readonly onActivate?: (id: string) => void;
    readonly selectedId?: string;
}

export interface KitList {
    readonly element: HTMLElement;
    setSelected(id: string): void;
}

/** Build a themed list, built on the `list` / `listitem` role primitives. */
export function createList(options: ListOptions): KitList {
    const element = createElement("ul", requireWidgetClass("list"), LIST_CLASS);
    element.setAttribute("aria-label", options.label);
    const rows = new Map<string, HTMLElement>();

    for (const item of options.items) {
        const row = createElement("li", requireWidgetClass("listitem"), "ctx-list__item");
        rows.set(item.id, row);

        const label = createElement("span", "ctx-list__label");
        setText(label, item.label);

        let content: HTMLElement = row;
        const onActivate = options.onActivate;
        if (onActivate !== undefined) {
            const button = createElement("button", "ctx-list__button");
            button.type = "button";
            button.addEventListener("click", () => {
                onActivate(item.id);
            });
            row.append(button);
            content = button;
        }
        content.append(label);
        if (item.detail !== undefined) {
            const detail = createElement("span", "ctx-list__detail");
            setText(detail, item.detail);
            content.append(detail);
        }
        element.append(row);
    }

    function setSelected(id: string): void {
        for (const [rowId, row] of rows) {
            const selected = rowId === id;
            row.classList.toggle("ctx-list__item--selected", selected);
            // `aria-current`, not `aria-selected`: these are ordinary list items, not options in a
            // listbox, and `aria-selected` on a plain `<li>` is ignored by assistive technology.
            if (selected) {
                row.setAttribute("aria-current", "true");
            } else {
                row.removeAttribute("aria-current");
            }
        }
    }

    if (options.selectedId !== undefined) {
        setSelected(options.selectedId);
    }
    return { element, setSelected };
}

// ------------------------------------------------------------------------------------------ tables

export type SortDirection = "ascending" | "descending";

export interface TableColumn {
    readonly key: string;
    readonly label: string;
    /** Right-aligns the column and gives it the theme's tabular numerals. */
    readonly numeric?: boolean;
    readonly sortable?: boolean;
}

export interface TableRow {
    readonly id: string;
    readonly cells: Readonly<Record<string, string>>;
}

export interface TableOptions {
    readonly columns: readonly TableColumn[];
    readonly rows: readonly TableRow[];
    /** A real `<caption>` — the accessible name of the table, and visible by default. */
    readonly caption: string;
    readonly onSort?: (key: string, direction: SortDirection) => void;
}

export interface KitTable {
    readonly element: HTMLTableElement;
    setRows(rows: readonly TableRow[]): void;
    /** The current sort, or `undefined` while unsorted. */
    readonly sort: { readonly key: string; readonly direction: SortDirection } | undefined;
}

/** Build a themed data table. Throws on an empty column set — a table with no columns is a bug. */
export function createTable(options: TableOptions): KitTable {
    if (options.columns.length === 0) {
        throw new Error("@context-engine/editor-kit: createTable needs at least one column");
    }
    const element = createElement("table", TABLE_CLASS);
    const caption = createElement("caption", "ctx-table__caption");
    setText(caption, options.caption);
    element.append(caption);

    const head = createElement("thead");
    const headRow = createElement("tr");
    const headers = new Map<string, HTMLTableCellElement>();
    let sort: { key: string; direction: SortDirection } | undefined;

    function paintSort(): void {
        for (const [key, header] of headers) {
            // `none` rather than a REMOVED attribute on the unsorted columns: the sortable columns
            // must announce that they ARE sortable, which is what `aria-sort="none"` says. Dropping
            // it entirely makes a sortable column indistinguishable from a fixed one.
            header.setAttribute("aria-sort", sort?.key === key ? sort.direction : "none");
        }
    }

    for (const column of options.columns) {
        const header = createElement("th", "ctx-table__header");
        header.scope = "col";
        if (column.numeric === true) {
            header.setAttribute("data-numeric", "true");
        }
        if (column.sortable === true) {
            const button = createElement("button", "ctx-table__sort");
            button.type = "button";
            setText(button, column.label);
            button.addEventListener("click", () => {
                const direction: SortDirection =
                    sort?.key === column.key && sort.direction === "ascending"
                        ? "descending"
                        : "ascending";
                sort = { key: column.key, direction };
                paintSort();
                options.onSort?.(column.key, direction);
            });
            header.append(button);
            headers.set(column.key, header);
        } else {
            setText(header, column.label);
        }
        headRow.append(header);
    }
    head.append(headRow);
    element.append(head);
    paintSort();

    const body = createElement("tbody");
    element.append(body);

    function setRows(rows: readonly TableRow[]): void {
        body.replaceChildren();
        for (const row of rows) {
            const tr = createElement("tr", "ctx-table__row");
            tr.setAttribute("data-row-id", row.id);
            options.columns.forEach((column, index) => {
                // The FIRST column is a row header (`<th scope="row">`), so a screen reader reading
                // any later cell announces which row it belongs to. A table of all-`<td>` cells makes
                // the user count columns.
                const cell: HTMLTableCellElement =
                    index === 0
                        ? createElement("th", "ctx-table__cell")
                        : createElement("td", "ctx-table__cell");
                if (index === 0) {
                    cell.scope = "row";
                }
                if (column.numeric === true) {
                    cell.setAttribute("data-numeric", "true");
                }
                setText(cell, row.cells[column.key] ?? "");
                tr.append(cell);
            });
            body.append(tr);
        }
    }

    setRows(options.rows);

    return {
        element,
        setRows,
        get sort(): { readonly key: string; readonly direction: SortDirection } | undefined {
            return sort;
        },
    };
}
