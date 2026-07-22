// The single command registry, JS side (M9 e07b, design 05 §6 / 04 §3).
//
// D8, "every interaction is a command": ONE registry in editor-core holds every invocable capability
// as a `{ id, title, category, when, handler }` entry carrying introspected docs, from THREE sources:
//
//   (a) CONTRACT VERBS, auto-projected from the registry-generated client schema (`RPC_METHODS`) —
//       the `help_model.h` pattern (a pure projection of `contract::Registry` into human-readable
//       help) generalized one layer up. A NEW contract verb (regenerating client-schema.ts) appears
//       here with NO hand-written entry — the DRIFT the `commands.test.ts` drift case pins.
//   (b) EDITOR COMMANDS — window / dock / theme / navigation, incl. the move-panel keyboard paths.
//       Declared here as specs; their handlers dispatch to an injected `EditorCommandActions` so the
//       action implementations (some PanelHost-backed today, theme when e06 lands) stay swappable.
//   (c) PANEL-MANIFEST COMMANDS — the `commands` a panel declares in its manifest v2 (04 §3), read
//       from e05b's promoted roster over `panel.list` (see panels.ts `PanelManifestCommand`).
//
// NO palette UI and NO keymap here (e07d / e07c). This module is the query/execute SUBSTRATE both
// consume: `query()` filters by category and by a resolved when-context; `execute()` runs a command's
// handler. Keeping the handler surface a plain thunk is what keeps that dispatch clean for both.

import { evaluateWhen, type WhenContext } from "./when.js";
import { RPC_METHODS, RPC_METHOD_NAMES } from "./generated/client-schema.js";
import type { RpcMethod, RpcMethodDescriptor } from "./generated/client-schema.js";
import type { PanelRoster } from "./panels.js";

/** Which source a command came from. Presentation (e07d) can group/filter on this. */
export type CommandCategory = "contract" | "editor" | "panel";

/** The introspected documentation a palette entry shows (05 §6 "palette entries carry the docs"). */
export interface CommandDoc {
    /** A one-line human/AI-readable summary. */
    readonly summary: string;
    /** Fuller introspected detail (params, flags, provenance) — never empty. */
    readonly detail: string;
}

/** The outcome of running a command handler. `ok: false` is an ordinary result (a refused action). */
export interface CommandOutcome {
    readonly ok: boolean;
    readonly note: string;
}

/** A command's action. A plain thunk so the palette and keymap dispatch it identically. */
export type CommandHandler = () => CommandOutcome | Promise<CommandOutcome>;

/** One registered command (05 §6 `{ id, title, category, when, handler }` + its introspected docs). */
export interface Command {
    /** Stable, unique across the whole registry. */
    readonly id: string;
    readonly title: string;
    readonly category: CommandCategory;
    /** The applicability guard (a when-clause, when.ts). `""` = always available. */
    readonly when: string;
    readonly docs: CommandDoc;
    readonly handler: CommandHandler;
}

/** A registry query. `category` narrows by source; `context` keeps only when-active commands. */
export interface CommandQuery {
    readonly category?: CommandCategory;
    readonly context?: WhenContext;
}

/**
 * The command registry.
 *
 * Holds the commands and NOTHING else — no bridge, no palette, no keymap. `register` is FAIL-CLOSED
 * on a duplicate id: two commands sharing an id is a real defect (the id is the execute key), never
 * silently last-wins.
 */
export class CommandRegistry {
    readonly #byId = new Map<string, Command>();

    /** Register one command. Throws on a duplicate id. */
    register(command: Command): void {
        if (this.#byId.has(command.id)) {
            throw new Error(`duplicate command id: ${command.id}`);
        }
        this.#byId.set(command.id, command);
    }

    /** Register many, in order. Throws on the first duplicate id. */
    registerAll(commands: Iterable<Command>): void {
        for (const command of commands) {
            this.register(command);
        }
    }

    /** How many commands are registered. */
    get size(): number {
        return this.#byId.size;
    }

    /** Look up one command, or `undefined`. */
    get(id: string): Command | undefined {
        return this.#byId.get(id);
    }

    /** Whether a command id is registered. */
    has(id: string): boolean {
        return this.#byId.has(id);
    }

    /**
     * The commands matching a query, in registration order.
     *
     * With `context` set, a command is kept only when its `when` is empty OR evaluates true against
     * the resolved context — the applicability filter the palette and keymap both drive.
     */
    query(query: CommandQuery = {}): readonly Command[] {
        const result: Command[] = [];
        for (const command of this.#byId.values()) {
            if (query.category !== undefined && command.category !== query.category) {
                continue;
            }
            if (query.context !== undefined && !evaluateWhen(command.when, query.context)) {
                continue;
            }
            result.push(command);
        }
        return result;
    }

    /** Every command, in registration order. */
    all(): readonly Command[] {
        return [...this.#byId.values()];
    }

    /**
     * Execute a command by id.
     *
     * An unknown id is an ordinary `ok: false` outcome, NOT a throw — the palette/keymap resolve ids
     * from user input, and a stale binding must degrade honestly rather than crash the dispatch path.
     */
    async execute(id: string): Promise<CommandOutcome> {
        const command = this.#byId.get(id);
        if (command === undefined) {
            return { ok: false, note: `unknown command: ${id}` };
        }
        return await command.handler();
    }
}

// ------------------------------------------------------------- (a) contract-verb auto-projection

/** The registry id a contract verb projects to (namespaced so it can never collide with (b)/(c)). */
export function contractCommandId(method: RpcMethod): string {
    return `contract.${method}`;
}

/** The human command phrase for a verb, e.g. `session step` / `set` — mirrors `VerbSpec::cli_command`. */
function contractCommandTitle(descriptor: RpcMethodDescriptor): string {
    return descriptor.noun === "" ? descriptor.verb : `${descriptor.noun} ${descriptor.verb}`;
}

/** Project a verb descriptor into its introspected docs (the generalized `help_model` projection). */
function contractCommandDoc(descriptor: RpcMethodDescriptor, title: string): CommandDoc {
    const lines = [`RPC method: ${descriptor.method}`];
    lines.push(`stability: ${descriptor.stability}${descriptor.deprecated ? " (deprecated)" : ""}`);
    if (descriptor.params.length > 0) {
        lines.push(`params: ${descriptor.params.join(", ")}`);
    }
    if (descriptor.flags.length > 0) {
        lines.push(`flags: ${descriptor.flags.map((flag) => `--${flag}`).join(" ")}`);
    }
    return {
        summary: `${title} — ${descriptor.stability} contract verb`,
        detail: lines.join("\n"),
    };
}

/** How a contract command reaches the daemon: the caller (e07c/e07d) supplies the dispatch. */
export type ContractDispatch = (method: RpcMethod) => CommandOutcome | Promise<CommandOutcome>;

/**
 * Project EVERY published RPC method into a command — the drift-proof property.
 *
 * The set is derived STRUCTURALLY from `RPC_METHOD_NAMES` (the generated schema's own order), never a
 * hand-maintained list, so a verb added to the contract regenerates client-schema.ts and appears here
 * automatically. Presentation (e07d) filters by `stability`/`category`; the registry stays complete.
 */
export function projectContractCommands(dispatch: ContractDispatch): readonly Command[] {
    return RPC_METHOD_NAMES.map((method): Command => {
        const descriptor = RPC_METHODS[method];
        const title = contractCommandTitle(descriptor);
        return {
            id: contractCommandId(method),
            title,
            category: "contract",
            when: "", // contract verbs are globally invocable; a specific verb may gain a when later
            docs: contractCommandDoc(descriptor, title),
            handler: () => dispatch(method),
        };
    });
}

// ------------------------------------------------------------------------- (b) editor commands

/** A dock direction for the move-panel keyboard paths (05 §6 / 04 §2). */
export type DockDirection = "left" | "right" | "up" | "down";

/**
 * The editor actions the built-in editor commands dispatch to.
 *
 * An INTERFACE so the implementations stay swappable and testable: the navigation/dock actions are
 * PanelHost-backed today, and `toggleTheme` is wired when the token kit (e06) lands. e07b registers
 * the command SURFACE; the app supplies the actions.
 */
export interface EditorCommandActions {
    focusNextPanel(): CommandOutcome | Promise<CommandOutcome>;
    focusPreviousPanel(): CommandOutcome | Promise<CommandOutcome>;
    moveActivePanel(direction: DockDirection): CommandOutcome | Promise<CommandOutcome>;
    closeActivePanel(): CommandOutcome | Promise<CommandOutcome>;
    toggleTheme(): CommandOutcome | Promise<CommandOutcome>;
}

/**
 * The built-in editor command set (window / dock / theme / navigation).
 *
 * Every dock/window/theme operation is reachable as a command with a keyboard-only path (feeds
 * R-A11Y-001 / R-CLI-001). The move/close commands guard on a focused panel and on NOT being in a
 * text input, so typing in a field never triggers them (03 §6 keyboard routing).
 */
export function editorCommands(actions: EditorCommandActions): readonly Command[] {
    const moveCommand = (direction: DockDirection): Command => ({
        id: `view.panel.move.${direction}`,
        title: `Move Panel ${direction[0]?.toUpperCase() ?? ""}${direction.slice(1)}`,
        category: "editor",
        when: "panelFocus && !textInputFocus",
        docs: {
            summary: `Move the focused panel ${direction} in the dock layout`,
            detail: `dock action; direction=${direction}; requires a focused panel and no text input`,
        },
        handler: () => actions.moveActivePanel(direction),
    });
    return [
        {
            id: "view.panel.focusNext",
            title: "Focus Next Panel",
            category: "editor",
            when: "!textInputFocus",
            docs: {
                summary: "Move focus to the next docked panel",
                detail: "navigation action; keyboard-only reachability for the dock (R-CLI-001)",
            },
            handler: () => actions.focusNextPanel(),
        },
        {
            id: "view.panel.focusPrevious",
            title: "Focus Previous Panel",
            category: "editor",
            when: "!textInputFocus",
            docs: {
                summary: "Move focus to the previous docked panel",
                detail: "navigation action; keyboard-only reachability for the dock (R-CLI-001)",
            },
            handler: () => actions.focusPreviousPanel(),
        },
        moveCommand("left"),
        moveCommand("right"),
        moveCommand("up"),
        moveCommand("down"),
        {
            id: "view.panel.close",
            title: "Close Active Panel",
            category: "editor",
            when: "panelFocus && !textInputFocus",
            docs: {
                summary: "Close the focused panel",
                detail: "dock action; requires a focused panel and no text input",
            },
            handler: () => actions.closeActivePanel(),
        },
        {
            id: "view.theme.toggle",
            title: "Toggle Theme",
            category: "editor",
            when: "",
            docs: {
                summary: "Toggle between the light and dark editor theme",
                detail: "theme action; wired to the token kit (06) when it lands",
            },
            handler: () => actions.toggleTheme(),
        },
    ];
}

// --------------------------------------------------------------------- (b') session commands
//
// Undo/redo are editor-level commands with STABLE ids (`session.undo` / `session.redo`,
// undo_journal.h `kUndoCommand`/`kRedoCommand`) that the e07c keymap binds to Ctrl+Z / Ctrl+Y. They
// are registered here so they resolve through the ONE registry like every other command; the keymap
// is just a consumer that dispatches them. The HANDLERS route to an injected `SessionCommandActions`
// so the actual undo/redo (the wire replay of the journal, undo_journal.h) can land later (e09)
// without touching the command surface — e07c delivers the binding + dispatch, not the replay.

/** The session actions the built-in session commands dispatch to (undo/redo). */
export interface SessionCommandActions {
    undo(): CommandOutcome | Promise<CommandOutcome>;
    redo(): CommandOutcome | Promise<CommandOutcome>;
}

/**
 * The built-in session command set (undo / redo).
 *
 * Guarded on `!textInputFocus`: when a DOM editable has focus, Ctrl+Z / Ctrl+Y belong to it (03 §6
 * keyboard routing), so the SESSION undo/redo must not fire — the keymap's `when` for these chords and
 * these commands' `when` agree, and the resolver honours both.
 */
export function sessionCommands(actions: SessionCommandActions): readonly Command[] {
    return [
        {
            id: "session.undo",
            title: "Undo",
            category: "editor",
            when: "!textInputFocus",
            docs: {
                summary: "Undo the last authored edit in the session",
                detail: "session action; bound to Ctrl+Z; the wire replay lands in e09 (undo_journal.h)",
            },
            handler: () => actions.undo(),
        },
        {
            id: "session.redo",
            title: "Redo",
            category: "editor",
            when: "!textInputFocus",
            docs: {
                summary: "Redo the last undone authored edit in the session",
                detail:
                    "session action; bound to Ctrl+Y / Ctrl+Shift+Z; the wire replay lands in e09 " +
                    "(undo_journal.h)",
            },
            handler: () => actions.redo(),
        },
    ];
}

// ------------------------------------------------------------------- (c) panel-manifest commands

/** How a panel-manifest command reaches its panel: the caller supplies the dispatch (panel.command). */
export type PanelCommandDispatch = (
    panelId: string,
    commandId: string,
) => CommandOutcome | Promise<CommandOutcome>;

/**
 * Project the manifest `commands` (04 §3) of every rostered panel into registry commands.
 *
 * Read straight from e05b's promoted roster (`panel.list` → `PanelRoster`): a panel that declares
 * commands in its manifest (chiefly iframe contributions, which have no C++ model to read them from)
 * contributes them here, each carrying its own `when` clause. Built-in uitree panels declare commands
 * on their C++ model instead, so their manifest `commands` are empty and contribute nothing — the
 * path is live and tested regardless.
 */
export function projectPanelCommands(
    roster: PanelRoster,
    dispatch: PanelCommandDispatch,
): readonly Command[] {
    const commands: Command[] = [];
    for (const panel of roster.panels) {
        for (const command of panel.commands) {
            commands.push({
                id: command.id,
                title: command.title,
                category: "panel",
                when: command.when,
                docs: {
                    summary: `${command.title} — from the ${panel.id} panel`,
                    detail: `panel-manifest command; panel=${panel.id}${
                        command.when === "" ? "" : `; when: ${command.when}`
                    }`,
                },
                handler: () => dispatch(panel.id, command.id),
            });
        }
    }
    return commands;
}

// --------------------------------------------------------------------------- assembly

/** Everything the projectors need to build the whole registry. */
export interface RegistrySources {
    readonly contractDispatch: ContractDispatch;
    readonly editorActions: EditorCommandActions;
    readonly sessionActions: SessionCommandActions;
    readonly roster: PanelRoster;
    readonly panelDispatch: PanelCommandDispatch;
}

/**
 * Build a registry from every source, in the (a) contract → (b) editor → (b') session → (c) panel
 * order.
 *
 * A duplicate id across sources throws (via `register`) rather than shadowing — a manifest command
 * that collides with an editor, session or contract id is a defect the build must surface, not
 * swallow.
 */
export function buildCommandRegistry(sources: RegistrySources): CommandRegistry {
    const registry = new CommandRegistry();
    registry.registerAll(projectContractCommands(sources.contractDispatch));
    registry.registerAll(editorCommands(sources.editorActions));
    registry.registerAll(sessionCommands(sources.sessionActions));
    registry.registerAll(projectPanelCommands(sources.roster, sources.panelDispatch));
    return registry;
}
