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
import { PALETTE_TOGGLE_COMMAND_ID, fuzzyMatch } from "./palette.js";
import { SETTINGS_PANEL_ID } from "./settings.js";
import type { SessionSelectSender } from "./session.js";
import type { UpdateState } from "./banners.js";
import { evaluateWhen, type WhenContext } from "./when.js";
import { isRecord } from "./bridge.js";
import { UI_TOPIC_MENU, type EditorUiBus, type EditorUiSubscription } from "./uibus.js";
import { LABEL_MAXIMIZE, LABEL_RESTORE, type ChromeWindowControls } from "./chrome.js";
import { admits, type PanelOpenResult } from "./panelhost.js";
import type { PanelManifest, PanelRoster } from "./panels.js";
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

/**
 * The Window menu's search-field-plus-panel-tree section (editor-UX d1, design 04 §4 / D9).
 *
 * A MARKER, not a static item list — unlike every other `MenuEntry`, its content is LIVE (the
 * roster, and which instances are open, can change after boot) and is rendered by a DEDICATED web
 * widget (`mountMenubar`'s `renderPanelSearchSection`), never by `renderEntries`'s generic
 * command/separator/submenu loop. `menuModelJson` DROPS it from the native macOS NSMenu wire shape
 * (see its own comment): a live search field has no NSMenu analogue, and the honest degrade is to
 * publish the menu as it stood before this task rather than a static, non-interactive stand-in.
 */
export interface MenuPanelSearchEntry {
    readonly kind: "panelSearch";
}

export type MenuEntry =
    | MenuCommandEntry
    | MenuSeparatorEntry
    | MenuSubmenuEntry
    | MenuPanelSearchEntry;

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

function command(commandId: string, label: string, disabledReason = ""): MenuCommandEntry {
    return {
        kind: "command",
        commandId,
        label,
        accelerator: menuAcceleratorFor(commandId),
        disabledReason,
    };
}

const SEPARATOR: MenuSeparatorEntry = { kind: "separator" };

/** The Window menu's one `panelSearch` marker (see `MenuPanelSearchEntry`) — a singleton, since it
 *  carries no per-build data of its own. */
const PANEL_SEARCH_ENTRY: MenuPanelSearchEntry = { kind: "panelSearch" };

/** The Open Recent submenu — one entry per recent, or the honest disabled placeholder. */
function openRecentSubmenu(recents: readonly RecentProject[]): MenuSubmenuEntry {
    if (recents.length === 0) {
        return {
            kind: "submenu",
            label: "Open Recent",
            items: [
                // Never registered, so it renders disabled (fact 2's honesty) with its reason.
                command("project.openRecent.none", "No Recent Projects", NO_RECENTS_REASON),
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

/**
 * The window ids the Window menu names — self + peers, deduped, ascending. The ONE derivation both
 * the model's window-list entries and the registered `window.focus.<id>` commands read, so a model
 * entry and its backing command cannot drift apart.
 */
function windowMenuIds(selfWindowId: number, windows: readonly number[]): readonly number[] {
    return [...new Set<number>([selfWindowId, ...windows])].sort((a, b) => a - b);
}

/** The Window menu's window list — one entry per live window, each naming its focus command. */
function windowListEntries(
    windows: readonly number[],
    selfWindowId: number,
): readonly MenuCommandEntry[] {
    return windowMenuIds(selfWindowId, windows).map((id) =>
        command(
            `${WINDOW_FOCUS_COMMAND_PREFIX}${String(id)}`,
            `Window ${String(id + 1)}${id === selfWindowId ? " (this window)" : ""}`,
        ),
    );
}

// --------------------------------------------------------------- the Window panel tree (d1 / D9)
// "Window opens things; Panel acts on the focused one" (04 §4). Everything below is the panel-open
// half: a generic command per rostered panel, a search over its `path`+title, and the row-state that
// makes a singleton/limited panel's instance rule VISIBLE rather than merely enforced. Purely data
// in, data out — no DOM — so it is testable without a browser; `mountMenubar`'s
// `renderPanelSearchSection` is the DOM half that reads it, the palette_view.ts split.

/** The registered id for opening one rostered panel: `view.panel.open.<panelId>` (04 §4's own
 *  wording). Deliberately the panel's REAL id, not the abbreviated `view.panel.open.settings` the
 *  macOS App menu already registers for Settings — the two ids name the SAME action through the SAME
 *  `PanelHost.openInstance` call, so a build simply carries one entry point per menu location; they
 *  are not required to collide, and keeping them apart means this generic source never has to special
 *  -case Settings. */
export const WINDOW_PANEL_OPEN_COMMAND_PREFIX = "view.panel.open.";

/** `${WINDOW_PANEL_OPEN_COMMAND_PREFIX}${panelId}` — the one place that spells the join. */
export function windowPanelOpenCommandId(panelId: string): string {
    return `${WINDOW_PANEL_OPEN_COMMAND_PREFIX}${panelId}`;
}

/**
 * What the Window menu's panel section needs from `PanelHost` — a narrow structural slice (the
 * `openPanel`/`hostHolder` late-bind pattern above), so a real `PanelHost` satisfies it with no
 * adapter and a T1 fixture can satisfy it with a three-method fake.
 */
export interface PanelSearchHost {
    openInstance(panelId: string, requestedInstanceId?: string): PanelOpenResult;
    /** The live copies of one kind, in mount order (mirrors `PanelHost.instancesOf`). */
    instancesOf(panelId: string): readonly string[];
    /** `null` until `PanelHost.start()` has read `panel.list` — the section degrades honestly then. */
    readonly roster: PanelRoster | null;
}

/**
 * One panel row's visible state (design 04 §4: "instance rules are visible, not just enforced").
 *
 * THREE CASES, mirroring `PanelHost.open`'s own three outcomes exactly, computed the SAME way it
 * computes them (`admits`, imported rather than re-derived) so the menu can never show a state the
 * actual open would contradict:
 *   - `singleton` already holding a live copy FOCUSES rather than refuses (the c3 correction) — this
 *     is checked FIRST, exactly as `PanelHost.open` special-cases it before consulting `admits`.
 *   - `limited` at its ceiling is `disabled`, carrying the SAME refusal text `open()` would answer
 *     with, so the row's tooltip and an actual refused click can never disagree.
 *   - everything else (`unlimited`, or under a `limited` ceiling) is `open` — the row mints a new copy.
 */
export type PanelRowState =
    | { readonly kind: "open" }
    | { readonly kind: "focus"; readonly instanceId: string }
    | { readonly kind: "disabled"; readonly reason: string };

export function panelRowState(manifest: PanelManifest, live: readonly string[]): PanelRowState {
    if (manifest.instances.mode === "singleton" && live.length >= 1) {
        const first = live[0];
        if (first !== undefined) {
            return { kind: "focus", instanceId: first };
        }
    }
    const refusal = admits(manifest, live.length);
    return refusal === "" ? { kind: "open" } : { kind: "disabled", reason: refusal };
}

/**
 * Project the roster into ONE generic command per panel — `view.panel.open.<panelId>` — dispatching
 * to `PanelHost.openInstance` (04 §4 fact 1: "no second dispatch system"). Registered through the
 * ONE registry like every other command (boot.ts, `startCommandLayer`), so the palette and the
 * keymap can invoke exactly what the Window menu's rows invoke — `menu.test.ts`'s single-dispatch
 * assertion for the d3 set applies here unchanged.
 *
 * The row's FOCUS-vs-OPEN distinction is purely presentational (`panelRowState`, read by the DOM
 * renderer): `PanelHost.open` already special-cases "singleton already open" into a `focused`
 * outcome with no `requestedInstanceId` needed, so one handler serves every row state honestly.
 */
export function windowPanelOpenCommands(
    roster: PanelRoster,
    host: PanelSearchHost,
): readonly Command[] {
    return roster.panels.map((manifest) => ({
        id: windowPanelOpenCommandId(manifest.id),
        title: `Open ${manifest.title}`,
        category: "editor",
        when: "",
        docs: {
            summary:
                `Open the ${manifest.title} panel` +
                (manifest.path === "" ? "" : ` (${manifest.path})`),
            detail:
                `window action (d1, editor-UX D9): PanelHost.openInstance("${manifest.id}") — a ` +
                "singleton already open focuses it, a limited panel past its max refuses naming the " +
                "limit, everything else mints a new copy",
        },
        handler: (): CommandOutcome => {
            const result = host.openInstance(manifest.id);
            switch (result.outcome) {
                case "opened":
                    return { ok: true, note: `opened ${manifest.title}` };
                case "focused":
                    return { ok: true, note: `${manifest.title} was already open — focused it` };
                default:
                    return { ok: false, note: result.diagnostic };
            }
        },
    }));
}

// ---------------------------------------------------------------------- the panel tree (browse mode)

/** One row the browse-mode (empty query) tree renders: a non-interactive path GROUP header, or a
 *  panel leaf. Top-level panels (`path === ""`) carry no header at all. */
export type PanelTreeEntry =
    | { readonly kind: "group"; readonly label: string }
    | { readonly kind: "panel"; readonly manifest: PanelManifest };

/**
 * Group the roster by its declared `path` (04 §2: slash-separated, empty = top level) for the
 * browse-mode tree. ONE grouping level — every panel sharing a `path` sits under ONE header naming
 * the full path (`"Scene/Debug"`), rather than true nested folders per segment: the web menubar's
 * generic submenu renderer already documents that a SECOND nesting level renders inert
 * (`renderEntries`'s `depth >= 1` guard), and a hand-built widget that reproduced true recursive
 * folders here would be the one part of the Window menu a screen reader's flat announcement and a
 * mouse-hover flyout would disagree about depth-wise. Top-level panels render first (no header),
 * then one header per distinct path, groups sorted alphabetically, panels in roster order within
 * each group.
 */
export function panelTreeRows(panels: readonly PanelManifest[]): readonly PanelTreeEntry[] {
    const top: PanelManifest[] = [];
    const groups = new Map<string, PanelManifest[]>();
    for (const panel of panels) {
        if (panel.path === "") {
            top.push(panel);
            continue;
        }
        const list = groups.get(panel.path);
        if (list === undefined) {
            groups.set(panel.path, [panel]);
        } else {
            list.push(panel);
        }
    }
    const rows: PanelTreeEntry[] = top.map((manifest) => ({ kind: "panel", manifest }));
    for (const path of [...groups.keys()].sort((a, b) => a.localeCompare(b))) {
        rows.push({ kind: "group", label: path });
        for (const manifest of groups.get(path) ?? []) {
            rows.push({ kind: "panel", manifest });
        }
    }
    return rows;
}

// --------------------------------------------------------------------- the panel search (filtered)

/**
 * The ONE candidate string a search query is matched against — the path's segments and the title,
 * SPACE-joined (`"Scene Debug Tilemap Painter"`), or bare `title` at the top level. What
 * `PanelSearchResult.positions` indexes into.
 *
 * ⚠ SPACES, NOT SLASHES. `fuzzyMatch` is reused UNCHANGED (04 §4's own rule) and matches candidate
 * characters LITERALLY — a query's space can only match a literal space in the candidate, never a
 * `/`. Joining with `/` (matching `path`'s own display spelling) would make 04 §4's own worked
 * example, `dbg tile` finding `Scene/Debug → Tilemap Painter`, fail: the query's space would have
 * to land on the `/` between "Debug" and "Tilemap", and a bare character-equality match cannot
 * treat the two as interchangeable (verified: replacing `/` with a space is what makes `fuzzyMatch`
 * carry the match across the boundary at all — a slash-joined candidate returns no match here).
 */
export function panelSearchCandidate(manifest: PanelManifest): string {
    return manifest.path === "" ? manifest.title : `${manifest.path.replace(/\//g, " ")} ${manifest.title}`;
}

export interface PanelSearchResult {
    readonly manifest: PanelManifest;
    readonly score: number;
    /** Indices into `candidate` — `fuzzyMatch`'s own positions, unmodified. */
    readonly positions: readonly number[];
    readonly candidate: string;
}

/**
 * Rank the roster against a query — 04 §4: "matches every path segment and the panel name
 * simultaneously… `dbg tile` finds `Scene/Debug → Tilemap Painter`". Reuses `fuzzyMatch`
 * (`palette.ts:102`) UNCHANGED against the joined `path/title` candidate, which is what makes a
 * cross-boundary query like `dbg tile` an ordinary in-order subsequence match rather than a second
 * matcher: "dbg" lands in `.../deBuG`, the space matches the `/` separator, and "tile" lands at the
 * start of `Tilemap`. An empty (or all-whitespace) query yields no results — the caller reads that as
 * "show the browse-mode tree instead", mirroring `Palette.results`'s own empty-query special case.
 */
export function searchPanels(
    panels: readonly PanelManifest[],
    query: string,
): readonly PanelSearchResult[] {
    const trimmed = query.trim();
    if (trimmed === "") {
        return [];
    }
    const results: PanelSearchResult[] = [];
    for (const manifest of panels) {
        const candidate = panelSearchCandidate(manifest);
        const match = fuzzyMatch(trimmed, candidate);
        if (match !== null) {
            results.push({ manifest, score: match.score, positions: match.positions, candidate });
        }
    }
    results.sort((a, b) => b.score - a.score || a.manifest.title.localeCompare(b.manifest.title));
    return results;
}

/** One contiguous run of `candidate` — matched (a `positions` index) or not — for highlight rendering. */
export interface HighlightRun {
    readonly text: string;
    readonly matched: boolean;
}

/**
 * Split `text` into contiguous matched/unmatched runs from a `FuzzyMatch.positions` list (04 §4:
 * "it returns `{score, positions}` so matched characters can be highlighted the way the palette
 * highlights them" — the DOM half `renderPanelSearchSection` builds ONE span per run from this,
 * never `innerHTML`). Pure and DOM-free so the mapping is unit-testable on its own; an empty
 * `positions` (the browse-mode tree, which never highlights) yields the whole text as one
 * unmatched run.
 */
export function highlightRuns(text: string, positions: readonly number[]): readonly HighlightRun[] {
    if (text === "") {
        return [];
    }
    if (positions.length === 0) {
        return [{ text, matched: false }];
    }
    const marked = new Set(positions);
    const runs: HighlightRun[] = [];
    let current = "";
    let currentMatched = marked.has(0);
    for (let i = 0; i < text.length; i += 1) {
        const isMatched = marked.has(i);
        if (i > 0 && isMatched !== currentMatched) {
            runs.push({ text: current, matched: currentMatched });
            current = "";
        }
        current += text[i] ?? "";
        currentMatched = isMatched;
    }
    if (current !== "") {
        runs.push({ text: current, matched: currentMatched });
    }
    return runs;
}

/**
 * Build the ONE menu model — exactly 03's table (fact 2), shaped by platform: `hybrid` (macOS)
 * carries the App menu (About / Settings… / Quit) and no File>Exit / Help>About; every other mode
 * carries Exit in File and About in Help. An entry carries NO applicability guard of its own:
 * enablement reads the backing command's registered `when` through `CommandAvailability`, so the
 * menu and the palette can never disagree about applicability.
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
            command("session.undo", "Undo"),
            command("session.redo", "Redo"),
            SEPARATOR,
            // The ⏳ rows (03): enabled ONLY under textInputFocus (their registered `when`), where
            // they delegate to the browser's native editing; disabled elsewhere with the reason in
            // the tooltip.
            command(EDIT_CUT_COMMAND_ID, "Cut", CLIPBOARD_DISABLED_REASON),
            command(EDIT_COPY_COMMAND_ID, "Copy", CLIPBOARD_DISABLED_REASON),
            command(EDIT_PASTE_COMMAND_ID, "Paste", CLIPBOARD_DISABLED_REASON),
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
            command("view.panel.focusNext", "Focus Next Panel"),
            command("view.panel.focusPrevious", "Focus Previous Panel"),
            command("view.panel.close", "Close Panel"),
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
            command("view.window.tearOut", "Tear Out Panel"),
            command("view.window.moveToPrimary", "Move Panel to Primary"),
            SEPARATOR,
            command("view.panel.move.left", "Move Panel Left"),
            command("view.panel.move.right", "Move Panel Right"),
            command("view.panel.move.up", "Move Panel Up"),
            command("view.panel.move.down", "Move Panel Down"),
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
            // D9 (editor-UX d1): "Window opens things" — a search field, then the panel tree built
            // from manifest `path`. LIVE content (`PANEL_SEARCH_ENTRY` is a marker; the roster can
            // change after boot), rendered by `mountMenubar`'s dedicated widget, never by the
            // generic command/separator/submenu loop. Its own separator follows so the trailing OS-
            // window list — 04 §4's "the existing OS-window list" — stays visually distinct.
            PANEL_SEARCH_ENTRY,
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

/** The Maximize item's live label (02 §5: it flips on the `maximized` fact, never on a poll) —
 *  the SAME label pair the caption button renders (chrome.ts), so the two spellings cannot drift. */
export function toggleMaximizeLabel(maximized: boolean): string {
    return maximized ? LABEL_RESTORE : LABEL_MAXIMIZE;
}

/** One entry's LIVE label — the single home of the Maximize/Restore flip, read by BOTH renderings
 *  (the wire serializer and the web dropdown), so a state-dependent label can never show stale in
 *  exactly one of the two. */
function menuEntryLabel(entry: MenuCommandEntry, maximized: boolean): string {
    return entry.commandId === WINDOW_TOGGLE_MAXIMIZE_COMMAND_ID
        ? toggleMaximizeLabel(maximized)
        : entry.label;
}

// ------------------------------------------------------------------------------- the enablement

/**
 * How the renderer/serializer resolves a command id against the late-bound registry: the backing
 * command's own `when` guard (`""` = always applicable) when the id resolves, `null` when it does
 * not. Sourcing the guard HERE — instead of carrying a copy on each model entry — keeps every
 * command's applicability in its ONE registration site, so the menu and the palette read the same
 * truth by construction.
 */
export type CommandAvailability = (commandId: string) => string | null;

/**
 * One entry's enablement (fact 2): the backing command must EXIST in the registry (a window-list
 * entry for a closed window, the recents placeholder, a command layer still coming up — all read
 * as "not available") AND its registered `when` guard must hold in the CURRENT context — the same
 * `evaluateWhen` over the same registered clause the palette filters with, so the two can never
 * disagree.
 */
export function menuEntryEnabled(
    entry: MenuCommandEntry,
    context: WhenContext,
    available: CommandAvailability,
): boolean {
    const when = available(entry.commandId);
    return when !== null && evaluateWhen(when, context);
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

/**
 * `null` for a `panelSearch` marker (see below) — every other kind serializes to one wire item.
 * `serializeEntries` is what drops the `null`s, so a caller never has to remember to filter.
 */
function entryJson(entry: MenuEntry, options: MenuSerializeOptions): Record<string, unknown> | null {
    if (entry.kind === "separator") {
        return { type: MENU_ITEM_TYPE_SEPARATOR };
    }
    if (entry.kind === "submenu") {
        return {
            type: MENU_ITEM_TYPE_SUBMENU,
            label: entry.label,
            items: serializeEntries(entry.items, options),
        };
    }
    if (entry.kind === "panelSearch") {
        // The Window menu's live search-field-plus-panel-tree (d1/D9) has no NSMenu analogue: a
        // native menu bar cannot host an interactive text field the way the web overlay does. The
        // honest degrade is to DROP it from the wire rather than publish a static, non-interactive
        // stand-in that would silently disagree with what the web menu bar shows — the native bar
        // simply keeps its pre-d1 Window menu shape (Minimize / Maximize / separator / window list).
        return null;
    }
    const enabled = menuEntryEnabled(entry, options.context, options.available);
    const out: Record<string, unknown> = {
        type: MENU_ITEM_TYPE_COMMAND,
        id: entry.commandId,
        label: menuEntryLabel(entry, options.maximized),
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

/** `entryJson` over a whole item list, dropping the `panelSearch` markers `entryJson` reports `null`
 *  for — the ONE place that filters, so `menuModelJson` and a submenu's own items agree by
 *  construction. */
function serializeEntries(
    entries: readonly MenuEntry[],
    options: MenuSerializeOptions,
): Record<string, unknown>[] {
    const out: Record<string, unknown>[] = [];
    for (const entry of entries) {
        const json = entryJson(entry, options);
        if (json !== null) {
            out.push(json);
        }
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
            items: serializeEntries(menu.items, options),
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
    // The ONE open-outcome mapping — File > Open Project… and every Open Recent entry ride it, so
    // the "what counts as opened" wording cannot drift between them.
    const openPath = async (path: string): Promise<CommandOutcome> => {
        const result = await deps.project.open(path);
        return result !== null && result.opened
            ? { ok: true, note: `opened ${result.path} (${result.action})` }
            : { ok: false, note: "open refused (no welcome surface behind this Shell)" };
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
            return openPath(path);
        },
        openRecent: openPath,
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
            // The primary-window close path IS the quit (03's table: window_bridge close policy) —
            // and since the close-button fix it genuinely quits: the Shell asks every window to
            // close. From a SECONDARY window this still closes only that window (docs/shell.md).
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

/** What `menuCommands` needs beyond the actions: the boot data the per-entry commands close over —
 *  exactly the model's own options minus the platform shape. */
export type MenuCommandData = Omit<MenuModelOptions, "mode">;

/**
 * The d3 command set (03's table, "new" column) — registered through `buildCommandRegistry`'s
 * `menuCommands` source, BEFORE the panel source, so incumbent-wins protects the ids. Each
 * registration HERE is the single home of its command's `when` guard — the menu model carries no
 * copy (`menuEntryEnabled` reads the registry), so the palette and the menu agree by construction.
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
        // The three ⏳ clipboard rows differ only in verb, id, and summary — one table, one stanza.
        ...(
            [
                ["cut", EDIT_CUT_COMMAND_ID, "Cut", "Cut the selection in the focused text field"],
                ["copy", EDIT_COPY_COMMAND_ID, "Copy", "Copy the selection in the focused text field"],
                ["paste", EDIT_PASTE_COMMAND_ID, "Paste", "Paste into the focused text field"],
            ] as const
        ).map(([verb, id, title, summary]) =>
            editor(
                id,
                title,
                "textInputFocus",
                summary,
                "edit action (d3, the 03 ⏳ row): delegates to the browser's native editing inside a " +
                    "focused text field; app-level clipboard is future work",
                () => actions.editClipboard(verb),
            ),
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
    for (const id of windowMenuIds(data.selfWindowId, data.windows)) {
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

// --------------------------------------------------------------- the Window panel search (d1/D9)
// The DOM classes `renderPanelSearchSection` builds with — declared here alongside the rest of the
// menu's classes (fact 4: styled in app.css from existing tokens, no inline style anywhere).

export const MENU_PANEL_SEARCH_CLASS = "ctx-menu__panels";
export const MENU_PANEL_SEARCH_INPUT_CLASS = "ctx-menu__panels-input";
export const MENU_PANEL_SEARCH_LIST_CLASS = "ctx-menu__panels-list";
export const MENU_PANEL_SEARCH_ITEM_CLASS = "ctx-menu__panels-item";
export const MENU_PANEL_SEARCH_SELECTED_CLASS = "ctx-menu__panels-item--selected";
export const MENU_PANEL_SEARCH_GROUP_CLASS = "ctx-menu__panels-group";
export const MENU_PANEL_SEARCH_EMPTY_CLASS = "ctx-menu__panels-empty";
export const MENU_PANEL_SEARCH_HIGHLIGHT_CLASS = "ctx-menu__panels-highlight";
/** The `id` `renderPanelSearchSection`'s listbox carries — the input's `aria-controls` target, the
 *  `ctx-palette__list` precedent. One Window menu exists per menubar, so a fixed id is safe. */
export const MENU_PANEL_SEARCH_LIST_ID = "ctx-menu-panels-list";

// ------------------------------------------------------------------------------- the menubar

export interface MountMenubarOptions {
    readonly model: MenuModel;
    /** The resolved when-context, read fresh on every dropdown open — the palette-view rule. */
    readonly contextProvider: () => WhenContext;
    /** Dispatch through the late-bound registry (boot closes over the holder — the a2 pattern). */
    readonly executeCommand: (commandId: string) => void;
    /** Resolve a command id to its registered `when` guard, or `null` when it does not resolve
     *  (`liveRegistry.current?.get` — late-bound too). */
    readonly commandAvailable: CommandAvailability;
    /** The maximized fact the Maximize/Restore label flips on (chrome.ts mount's own state). */
    readonly isMaximized: () => boolean;
    /**
     * The Window menu's panel section, LATE-BOUND (the `hostHolder`/`openPanel` pattern above):
     * `mountMenubar` runs before `PanelHost` exists (boot.ts: `startMenu` precedes `startPanels`),
     * so this holder starts `undefined` and boot.ts fills it once the host is up. Read at OPEN time,
     * never cached, so the section always reflects whichever roster/instances are live right now.
     */
    readonly panelHost: { readonly current: PanelSearchHost | undefined };
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

    /**
     * Render the Window menu's search-field-plus-panel-tree section (d1/D9) into `container` — a
     * SELF-CONTAINED widget with its own keyboard model (04 §4: "arrow keys move through results
     * while the field keeps text focus, and Escape closes the menu rather than only clearing the
     * field"), which is why it owns its `keydown` listener rather than routing through the roving-
     * tabindex system every OTHER item in this dropdown uses (`menuItems`/`focusItem` above): a
     * text field cannot lose DOM focus to a button on every arrow press and stay a text field.
     *
     * Rebuilt FRESH every time the Window menu opens (`renderEntries`'s own freshness rule): the
     * query always starts empty, the palette's own "always opens on the full list" precedent.
     * Degrades honestly when `options.panelHost.current` (or its roster) is not up yet — the same
     * late-bind boot ordering `MenuActionDeps.openPanel` already tolerates.
     */
    const renderPanelSearchSection = (container: HTMLElement): void => {
        const wrapper = doc.createElement("div");
        wrapper.className = MENU_PANEL_SEARCH_CLASS;

        const input = doc.createElement("input");
        input.type = "text";
        input.className = MENU_PANEL_SEARCH_INPUT_CLASS;
        input.setAttribute("role", "combobox");
        input.setAttribute("aria-expanded", "true");
        input.setAttribute("aria-controls", MENU_PANEL_SEARCH_LIST_ID);
        input.setAttribute("aria-autocomplete", "list");
        input.setAttribute("placeholder", "Search panels…");
        input.setAttribute("spellcheck", "false");

        const list = doc.createElement("div");
        list.id = MENU_PANEL_SEARCH_LIST_ID;
        list.className = MENU_PANEL_SEARCH_LIST_CLASS;
        list.setAttribute("role", "listbox");
        list.setAttribute("aria-label", "Panels");

        wrapper.append(input, list);
        container.append(wrapper);

        interface SelectableRow {
            readonly element: HTMLButtonElement;
            readonly manifest: PanelManifest;
            readonly enabled: boolean;
        }
        let selectable: SelectableRow[] = [];
        let selected = 0;

        const dispatchRow = (manifest: PanelManifest): void => {
            closeAll();
            options.executeCommand(windowPanelOpenCommandId(manifest.id));
        };

        /** ONE span per highlight run (`highlightRuns`) — never `innerHTML` (fact 2). */
        const buildLabel = (text: string, positions: readonly number[]): HTMLSpanElement => {
            const label = doc.createElement("span");
            label.className = MENU_ITEM_LABEL_CLASS;
            for (const run of highlightRuns(text, positions)) {
                if (!run.matched) {
                    label.append(doc.createTextNode(run.text));
                    continue;
                }
                const mark = doc.createElement("span");
                mark.className = MENU_PANEL_SEARCH_HIGHLIGHT_CLASS;
                mark.textContent = run.text;
                label.append(mark);
            }
            return label;
        };

        const buildRow = (
            manifest: PanelManifest,
            labelText: string,
            positions: readonly number[],
            live: readonly string[],
        ): SelectableRow => {
            const state = panelRowState(manifest, live);
            const enabled = state.kind !== "disabled";
            const row = doc.createElement("button");
            row.type = "button";
            row.className = MENU_PANEL_SEARCH_ITEM_CLASS;
            row.setAttribute("role", "option");
            row.setAttribute("data-panel-id", manifest.id);
            row.tabIndex = -1; // reached only through this widget's own virtual selection
            // Disabled = aria-disabled, never hidden (04 §4: "an item is disabled, never hidden, so
            // the menu's shape cannot flicker") — the SAME honest-degrade rule the ⏳ rows follow.
            row.setAttribute("aria-disabled", enabled ? "false" : "true");
            row.setAttribute("aria-selected", "false");
            if (state.kind === "disabled") {
                row.title = state.reason;
            } else if (state.kind === "focus") {
                row.title = `${manifest.title} is already open — selecting focuses it`;
            }
            row.append(buildLabel(labelText, positions));
            row.addEventListener("click", (): void => {
                if (row.getAttribute("aria-disabled") === "true") {
                    return; // disabled-item honesty, the same rule every other menu item follows
                }
                dispatchRow(manifest);
            });
            return { element: row, manifest, enabled };
        };

        // The `PALETTE_EMPTY_CLASS` shape (palette_view.ts): `role="option"` + `aria-disabled`
        // rather than `role="presentation"` — a listbox with zero options still reads as one to
        // assistive tech, so the placeholder occupies that one slot honestly instead of vanishing.
        const emptyRow = (text: string): HTMLDivElement => {
            const empty = doc.createElement("div");
            empty.className = MENU_PANEL_SEARCH_EMPTY_CLASS;
            empty.setAttribute("role", "option");
            empty.setAttribute("aria-disabled", "true");
            empty.textContent = text;
            return empty;
        };

        const setSelected = (index: number): void => {
            if (selectable.length === 0) {
                selected = 0;
                return;
            }
            selected = ((index % selectable.length) + selectable.length) % selectable.length;
            selectable.forEach((row, i) => {
                const isSelected = i === selected;
                row.element.setAttribute("aria-selected", isSelected ? "true" : "false");
                row.element.classList.toggle(MENU_PANEL_SEARCH_SELECTED_CLASS, isSelected);
            });
        };

        /** Rebuild the results/tree from the live host + the current query. Called at mount and on
         *  every keystroke — cheap enough (a handful of rostered panels), the palette's own rule. */
        const rebuild = (): void => {
            list.replaceChildren();
            selectable = [];
            const host = options.panelHost.current;
            const roster = host?.roster ?? null;
            if (host === undefined || roster === null) {
                list.append(emptyRow("Panels are not available yet"));
                return;
            }
            const query = input.value;
            if (query.trim() === "") {
                // Browse mode: the path tree (04 §4), no highlighting (positions are empty).
                let any = false;
                for (const treeEntry of panelTreeRows(roster.panels)) {
                    if (treeEntry.kind === "group") {
                        const header = doc.createElement("div");
                        header.className = MENU_PANEL_SEARCH_GROUP_CLASS;
                        header.setAttribute("role", "presentation");
                        header.textContent = treeEntry.label;
                        list.append(header);
                        continue;
                    }
                    any = true;
                    const live = host.instancesOf(treeEntry.manifest.id);
                    const row = buildRow(treeEntry.manifest, treeEntry.manifest.title, [], live);
                    list.append(row.element);
                    selectable.push(row);
                }
                if (!any) {
                    list.append(emptyRow("No panels available"));
                }
            } else {
                // Search mode: a flat, score-ranked list (`searchPanels`), highlighted by position.
                const results = searchPanels(roster.panels, query);
                if (results.length === 0) {
                    list.append(emptyRow("No matching panels"));
                }
                for (const result of results) {
                    const live = host.instancesOf(result.manifest.id);
                    const row = buildRow(result.manifest, result.candidate, result.positions, live);
                    list.append(row.element);
                    selectable.push(row);
                }
            }
            setSelected(0);
        };

        input.addEventListener("input", (): void => {
            rebuild();
        });
        // The search-in-menu pattern (04 §4): arrows move the VIRTUAL row selection while the
        // field keeps text focus; Enter dispatches through `dispatchRow` → the ONE registry;
        // Escape closes the whole menu. See `renderPanelSearchSection`'s own doc comment above.
        // key-handler-ok: on-the-command-path list navigation for an OPEN widget, the palette_view
        // .ts precedent — `stopPropagation` keeps the roving-tabindex handler below from reacting.
        input.addEventListener("keydown", (event: KeyboardEvent): void => {
            switch (event.key) {
                case "ArrowDown":
                    event.preventDefault();
                    event.stopPropagation();
                    setSelected(selected + 1);
                    return;
                case "ArrowUp":
                    event.preventDefault();
                    event.stopPropagation();
                    setSelected(selected - 1);
                    return;
                case "Home":
                    event.preventDefault();
                    event.stopPropagation();
                    setSelected(0);
                    return;
                case "End":
                    event.preventDefault();
                    event.stopPropagation();
                    setSelected(selectable.length - 1);
                    return;
                case "Enter": {
                    event.preventDefault();
                    event.stopPropagation();
                    const row = selectable[selected];
                    if (row !== undefined && row.enabled) {
                        dispatchRow(row.manifest);
                    }
                    return;
                }
                case "Escape":
                    event.preventDefault();
                    event.stopPropagation();
                    closeAll();
                    return;
                default:
                    return;
            }
        });

        rebuild();
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
                    if (!submenu.hidden) {
                        return; // already open — freshness is "at open", so no rebuild on re-hover
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
            if (entry.kind === "panelSearch") {
                renderPanelSearchSection(dropdown);
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
            label.textContent = menuEntryLabel(entry, options.isMaximized());
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
            // The Window menu's search field (d1/D9) wins initial focus over the roving-tabindex
            // system — the design's own ordering puts "a search field" first, and every command-
            // palette-shaped widget in this codebase auto-focuses its input on open (palette_view.ts
            // `sync`). A mouse click still reaches Minimize/Maximize/the window list beneath it.
            const searchInput = entry.dropdown.querySelector<HTMLInputElement>(
                `.${MENU_PANEL_SEARCH_INPUT_CLASS}`,
            );
            if (searchInput !== null) {
                searchInput.focus();
            } else {
                focusItem(menuItems(entry.dropdown), 0);
            }
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
        // The items of the INNERMOST open menu the focus is in (submenu-aware) — the ONE scope rule
        // vertical navigation and Home/End share; `null` while no dropdown is open.
        const scopedItems = (): HTMLButtonElement[] | null => {
            const dropdown = openIndex === -1 ? undefined : topLevel[openIndex]?.dropdown;
            if (dropdown === undefined) {
                return null;
            }
            const scope = active instanceof Element ? active.closest(`.${MENU_CLASS}`) : null;
            return menuItems(scope instanceof HTMLElement && !scope.hidden ? scope : dropdown);
        };
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
                    // The APG menubar pattern: both arrows open the focused menu; Down enters at
                    // the FIRST item, Up at the LAST.
                    openMenuAt(topIndex, event.key === "ArrowDown");
                    if (event.key === "ArrowUp") {
                        const opened = topLevel[topIndex];
                        if (opened !== undefined) {
                            const items = menuItems(opened.dropdown);
                            focusItem(items, items.length - 1);
                        }
                    }
                    return;
                }
                const items = scopedItems();
                if (items === null || items.length === 0) {
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
                const items = scopedItems();
                if (items === null) {
                    return;
                }
                event.preventDefault();
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
    // ONE dialog at a time: re-activating Help > About replaces the open dialog rather than
    // stacking a second copy on top of it (the re-render rule, not a toggle — the fresh state
    // renders either way).
    for (const open of [...host.querySelectorAll(`.${ABOUT_CLASS}`)]) {
        open.remove();
    }
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
