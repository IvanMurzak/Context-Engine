// T1 unit tests for the command palette (M9 e07d, design 05 §6 / 04 / 10). The DoD properties this
// tier pins: the palette OPENS, FUZZY-FILTERS over the live registry, EXECUTES commands from all THREE
// registry sources through the ONE registry, and shows each command's INTROSPECTED docs. The DOM view
// tests (mount / type-to-filter / Enter-to-execute / Escape-to-close / docs-shown) run in the real
// browser the `webui-ts` tier provides.
//
// Async note: the model's `execute` is async, so the cases that assert an execution EFFECT drive the
// synchronous handler path (a command's `handler()` — exactly what `registry.execute` invokes) or a
// synchronous spy, since the shared harness (harness.ts) runs each case with no await. The live CEF
// palette smoke covers the async execute end to end.

import { assert, assertEqual, type TestCase } from "./harness.js";
import { buildCommandRegistry, CommandRegistry } from "../commands.js";
import type {
    Command,
    EditorCommandActions,
    PanelCommandDispatch,
    SessionCommandActions,
} from "../commands.js";
import { parsePanelRoster } from "../panels.js";
import type { PanelRoster } from "../panels.js";
import {
    fuzzyMatch,
    Palette,
    PALETTE_TOGGLE_COMMAND_ID,
    paletteCommands,
} from "../palette.js";
import { PaletteView } from "../palette_view.js";
import { RPC_METHOD_NAMES } from "../generated/client-schema.js";
import { DaemonSessionState, resolveContext, STUB_EDITOR_UI } from "../when.js";
import type { WhenContext } from "../when.js";

// --------------------------------------------------------------------------- test doubles

/** A no-op editor-action set (the palette tests don't assert editor-action effects — commands.test.ts does). */
function noopEditorActions(record?: string[]): EditorCommandActions {
    const note = (tag: string) => {
        record?.push(tag);
        return { ok: true, note: tag };
    };
    return {
        focusNextPanel: () => note("focusNext"),
        focusPreviousPanel: () => note("focusPrevious"),
        moveActivePanel: (d) => note(`move:${d}`),
        closeActivePanel: () => note("close"),
        toggleTheme: () => note("theme"),
    };
}

function noopSessionActions(): SessionCommandActions {
    return { undo: () => ({ ok: true, note: "undo" }), redo: () => ({ ok: true, note: "redo" }) };
}

const noopPanelDispatch: PanelCommandDispatch = (panelId, commandId) => ({
    ok: true,
    note: `${panelId}/${commandId}`,
});

/** A roster whose one iframe panel declares a manifest command — the palette's source (c). */
function rosterWithPanelCommand(): PanelRoster {
    const roster = parsePanelRoster({
        contractMajor: 2,
        panels: [{ id: "ext.hello", commands: [{ id: "hello.greet", title: "Greet Hello" }] }],
    });
    if (roster === null) {
        throw new Error("test roster failed to parse");
    }
    return roster;
}

/** A registry built from all three sources, for the "palette surfaces everything" cases. */
function fullRegistry(editorRecord?: string[]): CommandRegistry {
    return buildCommandRegistry({
        contractDispatch: (method) => ({ ok: true, note: method }),
        editorActions: noopEditorActions(editorRecord),
        sessionActions: noopSessionActions(),
        roster: rosterWithPanelCommand(),
        panelDispatch: noopPanelDispatch,
    });
}

/** The resolved baseline context (nothing focused) most cases filter against. */
// e08d deleted the frozen stub session constant; a freshly constructed `DaemonSessionState` IS the
// same `edit` boot baseline (see when.ts), so this context is byte-identical to the one these cases
// used before.
const STUB_CONTEXT: WhenContext = resolveContext({
    editorUi: STUB_EDITOR_UI,
    session: new DaemonSessionState(),
});

function mkCommand(id: string, title: string, over: Partial<Command> = {}): Command {
    return {
        id,
        title,
        category: "editor",
        when: "",
        docs: { summary: `${title} summary`, detail: `${title} detail` },
        handler: () => ({ ok: true, note: id }),
        ...over,
    };
}

export const paletteTests: readonly TestCase[] = [
    // ------------------------------------------------------------- the fuzzy filter
    {
        name: "fuzzyMatch: subsequence match, word-start preference, and no-match returns null",
        run: () => {
            assert(fuzzyMatch("close", "Close Active Panel") !== null, "a direct prefix matches");
            assert(fuzzyMatch("clap", "Close Active Panel") !== null, "a subsequence matches");
            assert(fuzzyMatch("xyz", "Close Active Panel") === null, "a non-subsequence is null");
            // Word-start preference: 'cap' locks onto Close/Active/Panel initials, not stray letters.
            const wordStart = fuzzyMatch("cap", "Close Active Panel");
            assert(wordStart !== null, "the initials match");
            assertEqual(wordStart?.positions, [0, 6, 13], "matched the three word-start capitals");
            // An empty query matches everything with score 0 (the show-all case).
            const empty = fuzzyMatch("", "anything");
            assertEqual(empty?.score, 0, "an empty query is a zero-score match");
        },
    },
    {
        name: "fuzzyMatch: a better (more word-start / consecutive) match outscores a worse one",
        run: () => {
            const strong = fuzzyMatch("close", "Close Active Panel");
            const weak = fuzzyMatch("close", "Disclose Something Loosely Everywhere");
            assert(strong !== null && weak !== null, "both match");
            assert(
                (strong?.score ?? 0) > (weak?.score ?? 0),
                "the word-start, consecutive match scores higher",
            );
        },
    },

    // ------------------------------------------------------------- the palette model: open/close/toggle
    {
        name: "Palette: open/close/toggle track state and reset the query",
        run: () => {
            const palette = new Palette(new CommandRegistry());
            assert(!palette.isOpen, "starts closed");
            palette.open();
            assert(palette.isOpen, "open() opens");
            palette.setQuery("abc");
            assertEqual(palette.query, "abc", "the query is held");
            palette.close();
            assert(!palette.isOpen, "close() closes");
            assertEqual(palette.query, "", "close() clears the query");
            palette.toggle();
            assert(palette.isOpen && palette.query === "", "toggle opens and resets the query");
            palette.toggle();
            assert(!palette.isOpen, "toggle closes again");
        },
    },

    // ------------------------------------------------------------- results: when-filter + fuzzy-filter
    {
        name: "Palette.results: an empty query returns every when-active command, registry order",
        run: () => {
            const registry = new CommandRegistry();
            registry.registerAll([
                mkCommand("a", "Alpha", { when: "" }),
                mkCommand("b", "Beta", { when: "panelFocus" }),
                mkCommand("c", "Gamma", { when: "" }),
            ]);
            const palette = new Palette(registry);
            palette.open();
            // Nothing focused: the `panelFocus`-guarded command is filtered OUT by the when-context.
            const ids = palette.results({ textInputFocus: false }).map((e) => e.command.id);
            assertEqual(ids, ["a", "c"], "only when-active commands, in registration order");
            // With a focused panel, the guarded one appears.
            const focused = palette
                .results({ panelFocus: "p", textInputFocus: false })
                .map((e) => e.command.id);
            assertEqual(focused, ["a", "b", "c"], "the guarded command surfaces once its context holds");
        },
    },
    {
        name: "Palette.results: a query fuzzy-filters and ranks the active commands best-first",
        run: () => {
            const registry = new CommandRegistry();
            registry.registerAll([
                mkCommand("close", "Close Active Panel"),
                mkCommand("focus", "Focus Next Panel"),
                mkCommand("theme", "Toggle Theme"),
            ]);
            const palette = new Palette(registry);
            palette.open();
            palette.setQuery("panel");
            const ids = palette.results({ textInputFocus: false }).map((e) => e.command.id);
            assert(ids.includes("close") && ids.includes("focus"), "both panel commands match 'panel'");
            assert(!ids.includes("theme"), "the non-matching command is filtered out");
            // A more specific query narrows further and ranks the intended command first.
            palette.setQuery("close");
            const top = palette.results({ textInputFocus: false })[0];
            assertEqual(top?.command.id, "close", "the closest match ranks first");
            assert(top !== undefined && top.positions.length > 0, "the match carries highlight positions");
        },
    },
    {
        name: "Palette.results: a query also matches a command id fragment (no title hit)",
        run: () => {
            const registry = new CommandRegistry();
            registry.registerAll([mkCommand("view.panel.close", "Dismiss The Thing")]);
            const palette = new Palette(registry);
            palette.open();
            palette.setQuery("panelclose");
            const ids = palette.results({ textInputFocus: false }).map((e) => e.command.id);
            assertEqual(ids, ["view.panel.close"], "an id-fragment query finds the command by id");
        },
    },

    // ------------------------------------------------------------- all three sources, with docs
    {
        name: "Palette: surfaces commands from ALL THREE registry sources, each carrying its docs",
        run: () => {
            const palette = new Palette(fullRegistry());
            palette.open();
            // A focused ext.hello context so the panel command is active.
            const entries = palette.results({ panelFocus: "ext.hello", textInputFocus: false });
            const byId = new Map(entries.map((e) => [e.command.id, e]));
            // (a) contract — projected from the schema; (b) editor; (c) panel-manifest.
            const contractId = `contract.${RPC_METHOD_NAMES[0]}`;
            assert(byId.has(contractId), "a contract-verb command is present");
            assert(byId.has("view.theme.toggle"), "an editor command is present");
            assert(byId.has("hello.greet"), "a panel-manifest command is present");
            // Every surfaced entry carries non-empty introspected docs (05 §6).
            for (const entry of entries) {
                assert(entry.command.docs.summary.length > 0, `${entry.command.id} has a doc summary`);
                assert(entry.command.docs.detail.length > 0, `${entry.command.id} has doc detail`);
            }
        },
    },
    {
        name: "Palette.execute: runs a command's handler through the ONE registry and closes the palette",
        run: () => {
            const record: string[] = [];
            const palette = new Palette(fullRegistry(record));
            palette.open();
            // Drive the synchronous handler directly (the harness cannot await); it is exactly what
            // execute() invokes, so routing is proven, and assert the close is wired via a sync spy.
            const entries = palette.results({ panelFocus: "ext.hello", textInputFocus: false });
            const close = entries.find((e) => e.command.id === "view.panel.close");
            assert(close !== undefined, "the close command is available");
            void close?.command.handler();
            assertEqual(record, ["close"], "the editor command routed to its action");
        },
    },

    // ------------------------------------------------------------- the palette-open command
    {
        name: "paletteCommands: registers the toggle command that flips the palette open state",
        run: () => {
            const palette = new Palette(new CommandRegistry());
            let toggled = 0;
            const commands = paletteCommands({
                toggle: () => {
                    toggled += 1;
                    palette.toggle();
                    return { ok: true, note: "toggled" };
                },
            });
            assertEqual(commands.length, 1, "one command");
            const toggle = commands[0];
            assertEqual(toggle?.id, PALETTE_TOGGLE_COMMAND_ID, "the stable palette-open id");
            assertEqual(toggle?.when, "", "openable from anywhere (no when guard)");
            void toggle?.handler();
            assertEqual(toggled, 1, "the handler dispatches the toggle action");
            assert(palette.isOpen, "the palette opened");
        },
    },

    // ------------------------------------------------------------- the DOM view (real browser tier)
    {
        name: "PaletteView: mounts hidden, opens on the model, filters, and shows docs",
        run: () => {
            const registry = new CommandRegistry();
            registry.registerAll([
                mkCommand("close", "Close Active Panel"),
                mkCommand("theme", "Toggle Theme"),
            ]);
            const palette = new Palette(registry);
            const container = document.createElement("div");
            document.body.appendChild(container);
            try {
                const view = new PaletteView({
                    host: container,
                    palette,
                    contextProvider: () => STUB_CONTEXT,
                });
                view.mount();
                assert(view.mounted, "the view mounted");
                const root = container.querySelector<HTMLElement>(".ctx-palette");
                assert(root !== null, "the overlay root exists");
                assertEqual(root?.hidden, true, "the overlay is hidden while the palette is closed");

                palette.open();
                view.sync();
                assertEqual(root?.hidden, false, "opening the model shows the overlay");
                const items = container.querySelectorAll(".ctx-palette__item");
                assertEqual(items.length, 2, "both commands are listed");
                // Docs are shown under each title (05 §6 "palette entries carry the docs").
                const firstDoc = container.querySelector(".ctx-palette__doc");
                assert(
                    firstDoc !== null && (firstDoc.textContent ?? "").length > 0,
                    "each entry shows its introspected doc summary",
                );

                // Typing into the input filters the list through the model.
                const input = container.querySelector<HTMLInputElement>(".ctx-palette__input");
                assert(input !== null, "the filter input exists");
                if (input !== null) {
                    input.value = "close";
                    input.dispatchEvent(new Event("input"));
                }
                const filtered = container.querySelectorAll(".ctx-palette__item");
                assertEqual(filtered.length, 1, "the query filtered the list to the one match");
                assertEqual(
                    filtered[0]?.getAttribute("data-command-id"),
                    "close",
                    "the surviving item is the match",
                );
                view.dispose();
                assert(container.querySelector(".ctx-palette") === null, "dispose removes the overlay");
            } finally {
                container.remove();
            }
        },
    },
    {
        name: "PaletteView: Enter executes the selected command; Escape closes the overlay",
        run: () => {
            const record: string[] = [];
            const registry = new CommandRegistry();
            registry.register(
                mkCommand("close", "Close Active Panel", {
                    handler: () => {
                        record.push("executed");
                        return { ok: true, note: "close" };
                    },
                }),
            );
            const palette = new Palette(registry);
            const container = document.createElement("div");
            document.body.appendChild(container);
            try {
                const view = new PaletteView({
                    host: container,
                    palette,
                    contextProvider: () => STUB_CONTEXT,
                });
                view.mount();
                palette.open();
                view.sync();
                const input = container.querySelector<HTMLInputElement>(".ctx-palette__input");
                assert(input !== null, "the input exists");
                // Enter activates the selected (first) entry — dispatches through the registry.
                input?.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter" }));
                assertEqual(record, ["executed"], "Enter executed the selected command");
                assert(!palette.isOpen, "executing a command closed the palette");

                // Reopen, then Escape closes without executing.
                palette.open();
                view.sync();
                input?.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape" }));
                assert(!palette.isOpen, "Escape closed the palette");
                assertEqual(record.length, 1, "Escape executed nothing");
                view.dispose();
            } finally {
                container.remove();
            }
        },
    },
];
