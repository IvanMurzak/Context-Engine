// T1 unit tests for the command registry (M9 e07b, design 05 §6 / 04 §3). The properties this tier
// pins: the registry holds executable commands from all THREE sources; contract verbs are
// AUTO-PROJECTED from the generated schema (the drift guard — a new verb appears with no hand-written
// entry); manifest commands are read from the roster; and `query` filters by category and by a
// resolved when-context.
//
// Dispatch is exercised SYNCHRONOUSLY through each command's `handler` (the spies below are
// synchronous, so a call records immediately) rather than through the async `execute` — the shared
// harness (harness.ts) runs each case synchronously, so an awaited assertion could not be counted.
// `handler()` is exactly what `execute` invokes, so routing is proven either way.

import { assert, assertEqual, type TestCase } from "./harness.js";
import {
    SESSION_UNDO_PANEL_ID,
    claimPaletteToggle,
    makePanelDispatch,
    makeSessionActions,
} from "../boot.js";
import { PALETTE_TOGGLE_COMMAND_ID, paletteCommands } from "../palette.js";
import { PanelClient } from "../panels.js";
import {
    buildCommandRegistry,
    COMMAND_REJECTION_LOG_LIMIT,
    CommandRegistry,
    contractCommandId,
    editorCommands,
    projectContractCommands,
    projectPanelCommands,
    sessionCommands,
} from "../commands.js";
import type {
    Command,
    CommandOutcome,
    ContractDispatch,
    EditorCommandActions,
    PanelCommandDispatch,
    SessionCommandActions,
} from "../commands.js";
import { parsePanelRoster } from "../panels.js";
import type { PanelRoster } from "../panels.js";
import { RPC_METHOD_NAMES } from "../generated/client-schema.js";
import type { RpcMethod } from "../generated/client-schema.js";

// --------------------------------------------------------------------------- test doubles

interface ContractSpy {
    readonly dispatch: ContractDispatch;
    readonly seen: RpcMethod[];
}
function contractSpy(): ContractSpy {
    const seen: RpcMethod[] = [];
    return {
        seen,
        dispatch: (method) => {
            seen.push(method);
            return { ok: true, note: method };
        },
    };
}

interface ActionSpy {
    readonly actions: EditorCommandActions;
    readonly calls: string[];
}
function actionSpy(): ActionSpy {
    const calls: string[] = [];
    const record = (tag: string) => {
        calls.push(tag);
        return { ok: true, note: tag };
    };
    return {
        calls,
        actions: {
            focusNextPanel: () => record("focusNext"),
            focusPreviousPanel: () => record("focusPrevious"),
            moveActivePanel: (direction) => record(`move:${direction}`),
            closeActivePanel: () => record("close"),
            toggleTheme: () => record("theme"),
            tearOutActivePanel: () => record("tearOut"),
            movePanelToPrimary: () => record("moveToPrimary"),
        },
    };
}

interface SessionSpy {
    readonly actions: SessionCommandActions;
    readonly calls: string[];
}
function sessionSpy(): SessionSpy {
    const calls: string[] = [];
    const record = (tag: string) => {
        calls.push(tag);
        return { ok: true, note: tag };
    };
    return {
        calls,
        actions: {
            undo: () => record("undo"),
            redo: () => record("redo"),
        },
    };
}

interface PanelSpy {
    readonly dispatch: PanelCommandDispatch;
    readonly seen: string[];
}
function panelSpy(): PanelSpy {
    const seen: string[] = [];
    return {
        seen,
        dispatch: (panelId, commandId) => {
            seen.push(`${panelId}/${commandId}`);
            return { ok: true, note: commandId };
        },
    };
}

/** A roster with one iframe-style panel that DECLARES manifest commands and one that declares none. */
function rosterWithCommands(): PanelRoster {
    const roster = parsePanelRoster({
        contractMajor: 2,
        panels: [
            {
                id: "ext.hello",
                commands: [
                    { id: "hello.greet", title: "Greet", when: "panelFocus == ext.hello" },
                    { id: "hello.wave", title: "Wave" }, // no `when` -> always active
                ],
            },
            { id: "builtin.problems" }, // built-in uitree panel: declares no manifest commands
        ],
    });
    if (roster === null) {
        throw new Error("test roster failed to parse");
    }
    return roster;
}

function mkCommand(id: string, over: Partial<Command> = {}): Command {
    return {
        id,
        title: id,
        category: "editor",
        when: "",
        docs: { summary: id, detail: id },
        handler: () => ({ ok: true, note: id }),
        ...over,
    };
}

export const commandsTests: readonly TestCase[] = [
    // ------------------------------------------------------------- the registry
    {
        name: "CommandRegistry: register / get / has / size / all, in registration order",
        run: () => {
            const registry = new CommandRegistry();
            registry.registerAll([mkCommand("a"), mkCommand("b")]);
            assertEqual(registry.size, 2, "size");
            assert(registry.has("a"), "has a");
            assert(!registry.has("c"), "not has c");
            assertEqual(registry.get("b")?.title, "b", "get b");
            assertEqual(
                registry.all().map((command) => command.id),
                ["a", "b"],
                "all() preserves registration order",
            );
        },
    },
    {
        name: "CommandRegistry: a duplicate id is FAIL-CLOSED (throws) on the STRICT path, never last-wins",
        run: () => {
            const registry = new CommandRegistry();
            registry.register(mkCommand("dup"));
            let threw = false;
            try {
                registry.register(mkCommand("dup", { title: "shadow" }));
            } catch {
                threw = true;
            }
            assert(threw, "registering a duplicate id throws");
            assertEqual(registry.get("dup")?.title, "dup", "the original entry is intact");
        },
    },
    {
        // M9 e13b-2 — the NON-FATAL sibling of the case above, and the one production assembly uses.
        name: "CommandRegistry: tryRegister is INCUMBENT-WINS and reports an ATTRIBUTABLE refusal",
        run: () => {
            const registry = new CommandRegistry();
            assertEqual(
                registry.tryRegister(mkCommand("dup", { category: "editor", title: "Toggle Theme" })),
                null,
                "the first registration of an id succeeds",
            );
            const rejection = registry.tryRegister(
                mkCommand("dup", { category: "panel", title: "Hijack" }),
            );
            assert(rejection !== null, "the second is REFUSED rather than throwing");
            assertEqual(rejection?.id, "dup", "the refusal names the colliding id");
            assertEqual(rejection?.category, "panel", "…the source that was refused");
            assertEqual(rejection?.incumbent, "editor", "…and the source that keeps it");
            // ATTRIBUTION IS THE DoD LINE, so it is asserted on the STRING a human reads, not only on
            // the fields: "duplicate command id: dup" — the message this replaced — named neither the
            // culprit nor what it collided with, which is what made the outage undiagnosable.
            for (const fragment of ["dup", "Toggle Theme", "Hijack", "editor", "panel"]) {
                assert(
                    (rejection?.diagnostic ?? "").includes(fragment),
                    `the diagnostic names "${fragment}": ${String(rejection?.diagnostic)}`,
                );
            }
            assertEqual(registry.get("dup")?.title, "Toggle Theme", "the INCUMBENT keeps the id");
            assertEqual(registry.size, 1, "and the registry holds exactly one command");
            assertEqual(registry.rejections.length, 1, "the refusal is logged for boot to report");
        },
    },
    {
        name: "CommandRegistry: unregister withdraws exactly one command",
        run: () => {
            const registry = new CommandRegistry();
            registry.tryRegisterAll([mkCommand("a"), mkCommand("b")]);
            assert(registry.unregister("a"), "an existing id is removed");
            assert(!registry.unregister("a"), "…and removing it twice reports false");
            assertEqual(
                registry.all().map((command) => command.id),
                ["b"],
                "the sibling is untouched",
            );
            // The id is now FREE — which is the property `bridge.commands.unregister` needs, and the
            // reason withdrawal cannot be faked by overwriting with a no-op command.
            assertEqual(registry.tryRegister(mkCommand("a")), null, "the id can be re-registered");
        },
    },
    {
        name: "CommandRegistry: the rejection log is BOUNDED (untrusted code chooses the refusal rate)",
        run: () => {
            const registry = new CommandRegistry();
            registry.tryRegister(mkCommand("squat"));
            for (let i = 0; i < COMMAND_REJECTION_LOG_LIMIT + 20; i += 1) {
                registry.tryRegister(mkCommand("squat", { title: `attempt-${String(i)}` }));
            }
            assertEqual(
                registry.rejections.length,
                COMMAND_REJECTION_LOG_LIMIT,
                "the log stops at its cap rather than growing with a panel's retry loop",
            );
            // NEWEST-WINS: a repeated refusal repeats, so the recent context is what a diagnosis needs.
            const last = registry.rejections[registry.rejections.length - 1];
            assert(
                (last?.diagnostic ?? "").includes(`attempt-${String(COMMAND_REJECTION_LOG_LIMIT + 19)}`),
                "the most recent refusal survives; the oldest is dropped",
            );
        },
    },
    {
        name: "CommandRegistry.query: filters by category and by a resolved when-context",
        run: () => {
            const registry = new CommandRegistry();
            registry.registerAll([
                mkCommand("editor.always", { category: "editor", when: "" }),
                mkCommand("panel.gated", { category: "panel", when: "panelFocus == ext.hello" }),
                mkCommand("editor.gated", { category: "editor", when: "!textInputFocus" }),
            ]);
            assertEqual(
                registry.query({ category: "editor" }).map((command) => command.id),
                ["editor.always", "editor.gated"],
                "category filter",
            );
            // With a context, only when-active commands survive.
            const active = registry
                .query({ context: { panelFocus: "ext.hello", textInputFocus: false } })
                .map((command) => command.id);
            assertEqual(
                active,
                ["editor.always", "panel.gated", "editor.gated"],
                "all three active when the context matches",
            );
            const typing = registry
                .query({ context: { panelFocus: "", textInputFocus: true } })
                .map((command) => command.id);
            assertEqual(
                typing,
                ["editor.always"],
                "a text-input context suppresses the panel- and !textInput-guarded commands",
            );
        },
    },

    // ------------------------------------------------------------- (a) contract-verb auto-projection
    {
        name: "projectContractCommands: one command per published verb, ids EXACTLY the derived set (drift guard)",
        run: () => {
            const spy = contractSpy();
            const commands = projectContractCommands(spy.dispatch);
            assertEqual(
                commands.length,
                RPC_METHOD_NAMES.length,
                "one command per published RPC method — a new verb auto-appears",
            );
            const ids = commands.map((command) => command.id).sort();
            const derived = RPC_METHOD_NAMES.map((method) => contractCommandId(method)).sort();
            assertEqual(ids, derived, "the id set is STRUCTURALLY derived from RPC_METHOD_NAMES");
            for (const command of commands) {
                assertEqual(command.category, "contract", `${command.id} is a contract command`);
                assertEqual(command.when, "", `${command.id} is globally invocable`);
            }
        },
    },
    {
        name: "projectContractCommands: entries carry introspected docs and dispatch their RPC method",
        run: () => {
            const spy = contractSpy();
            const commands = projectContractCommands(spy.dispatch);
            const setCommand = commands.find((command) => command.id === contractCommandId("set"));
            assert(setCommand !== undefined, "the `set` verb is projected");
            if (setCommand !== undefined) {
                assertEqual(setCommand.title, "set", "title is the verb's command phrase");
                assert(
                    setCommand.docs.detail.includes("RPC method: set"),
                    "docs are introspected from the descriptor",
                );
                assert(setCommand.docs.detail.includes("params:"), "docs list the verb's params");
                void setCommand.handler();
            }
            const nested = commands.find(
                (command) => command.id === contractCommandId("session.step"),
            );
            assertEqual(nested?.title, "session step", "a noun.verb reads as the CLI command phrase");
            void nested?.handler();
            assertEqual(spy.seen, ["set", "session.step"], "each handler dispatches its own method");
        },
    },

    // ------------------------------------------------------------- (b) editor commands
    {
        name: "editorCommands: the window/dock/theme/navigation surface with keyboard-safe guards",
        run: () => {
            const spy = actionSpy();
            const commands = editorCommands(spy.actions);
            const ids = commands.map((command) => command.id);
            for (const expected of [
                "view.panel.focusNext",
                "view.panel.move.left",
                "view.panel.move.down",
                "view.panel.close",
                "view.theme.toggle",
            ]) {
                assert(ids.includes(expected), `editor command ${expected} is registered`);
            }
            // Move/close guard on a focused panel AND on not being in a text input (03 §6 routing).
            const move = commands.find((command) => command.id === "view.panel.move.left");
            assertEqual(move?.when, "panelFocus && !textInputFocus", "move guards focus + text input");
            const theme = commands.find((command) => command.id === "view.theme.toggle");
            assertEqual(theme?.when, "", "theme toggle is always available");
        },
    },
    {
        name: "editorCommands: each command dispatches to its editor action",
        run: () => {
            const spy = actionSpy();
            const commands = editorCommands(spy.actions);
            const byId = new Map(commands.map((command) => [command.id, command]));
            void byId.get("view.panel.focusNext")?.handler();
            void byId.get("view.panel.move.right")?.handler();
            void byId.get("view.panel.close")?.handler();
            void byId.get("view.theme.toggle")?.handler();
            assertEqual(
                spy.calls,
                ["focusNext", "move:right", "close", "theme"],
                "each editor command routes to its action (direction preserved)",
            );
        },
    },
    {
        name: "editorCommands: the e10b tear-out + move-to-primary window commands, keyboard-guarded",
        run: () => {
            const spy = actionSpy();
            const commands = editorCommands(spy.actions);
            const byId = new Map(commands.map((command) => [command.id, command]));
            const tearOut = byId.get("view.window.tearOut");
            const moveToPrimary = byId.get("view.window.moveToPrimary");
            assert(tearOut !== undefined, "view.window.tearOut is registered (R-A11Y-001 keyboard path)");
            assert(moveToPrimary !== undefined, "view.window.moveToPrimary is registered");
            // Guarded on a focused panel AND not-in-a-text-input, like move/close (03 §6 routing) —
            // typing in a field never tears a panel out.
            assertEqual(
                tearOut?.when,
                "panelFocus && !textInputFocus",
                "tear-out guards focus + text input",
            );
            assertEqual(
                moveToPrimary?.when,
                "panelFocus && !textInputFocus",
                "move-to-primary guards focus + text input",
            );
            void tearOut?.handler();
            void moveToPrimary?.handler();
            assertEqual(
                spy.calls,
                ["tearOut", "moveToPrimary"],
                "each window command routes to its action",
            );
        },
    },

    // ------------------------------------------------------------- (b') session commands
    {
        name: "sessionCommands: undo/redo with the stable ids the keymap binds, guarded off text input",
        run: () => {
            const spy = sessionSpy();
            const commands = sessionCommands(spy.actions);
            const byId = new Map(commands.map((command) => [command.id, command]));
            const undo = byId.get("session.undo");
            const redo = byId.get("session.redo");
            assert(undo !== undefined, "session.undo is registered (undo_journal.h kUndoCommand)");
            assert(redo !== undefined, "session.redo is registered (undo_journal.h kRedoCommand)");
            assertEqual(undo?.when, "!textInputFocus", "undo does not fire while typing (03 §6)");
            assertEqual(redo?.when, "!textInputFocus", "redo does not fire while typing (03 §6)");
            assertEqual(undo?.category, "editor", "session commands are editor-category");
            void undo?.handler();
            void redo?.handler();
            assertEqual(spy.calls, ["undo", "redo"], "each session command routes to its action");
        },
    },

    // M9 e09c — the session actions are no longer a stub, driven through the PRODUCTION factories.
    //
    // WHY THE FACTORIES AND NOT A LOCAL LOOKALIKE. e07c left `sessionActions` an honest refusal
    // ("the wire replay lands in e09") and e09c replaced it with a real `panel.command` dispatch. A
    // case that builds its OWN actions object cannot tell those two apart — revert boot.ts to the
    // stub and it stays green — so this drives `makeSessionActions(makePanelDispatch(client))`,
    // which IS what `startCommandLayer` wires. (What it still does not cover is that call site
    // itself; the palette is the only live consumer and is unreachable from this tier, because the
    // keymap is not wired to dispatch until the 03 §6 input pump lands.)
    //
    // Two more things it catches that nothing else does. (1) A drift in `SESSION_UNDO_PANEL_ID`
    // fails no build: Ctrl+Z would simply dispatch to a panel that does not exist and answer `not
    // dispatched` forever, with no error anywhere. NOTE this pins only the TS spelling — the C++ end
    // is pinned independently (test_undo_journal.cpp, test_roster.cpp) and NOTHING relates the two,
    // so a re-spelling of `UndoJournal::kContributionId` would red the C++ suites and leave this
    // one green. (2) `dispatched:false` must NOT report `ok`. That is the TS MAPPING only; that the
    // C++ host actually answers false for nothing-to-undo, for a loud drop and for a refused replay
    // is `undo_feed.cpp` § make_provider's property, pinned separately in `test_undo_feed.cpp`.
    {
        name: "session undo/redo dispatch to the Shell's session-undo panel and report its verdict honestly",
        run: async () => {
            assertEqual(
                SESSION_UNDO_PANEL_ID,
                "builtin.session.undo",
                "mirrors UndoJournal::kContributionId (undo_journal.h) + the builtin roster entry",
            );

            const seen: string[] = [];
            let dispatched = true;
            // A `PanelClient` stand-in at the CLIENT's own seam: `command()` is the one method the
            // dispatcher calls, and its `{dispatched, revision}` reply is the panel bridge's shape.
            const client = {
                command: (panelId: string, commandId: string, nodeId: string) => {
                    seen.push(`${panelId}/${commandId}/${nodeId}`);
                    return Promise.resolve({ dispatched, revision: 7 });
                },
            } as unknown as PanelClient;

            const actions = makeSessionActions(makePanelDispatch(client));
            const byId = new Map(sessionCommands(actions).map((c) => [c.id, c]));

            const undone = await byId.get("session.undo")?.handler();
            const redone = await byId.get("session.redo")?.handler();
            assertEqual(
                seen,
                ["builtin.session.undo/session.undo/", "builtin.session.undo/session.redo/"],
                "both route to the Shell's session-undo panel over panel.command",
            );
            assertEqual((undone as CommandOutcome).ok, true, "a dispatched undo reports ok");
            assertEqual((redone as CommandOutcome).ok, true, "a dispatched redo reports ok");

            // Nothing to undo / a loud drop / a refused replay -> the host answers `dispatched:false`.
            dispatched = false;
            const refused = await byId.get("session.undo")?.handler();
            assertEqual(
                (refused as CommandOutcome).ok,
                false,
                "an undo the host did not dispatch is NOT reported as ok",
            );
            assert(
                (refused as CommandOutcome).note.includes("not dispatched"),
                "and says so, rather than reporting a bare failure",
            );

            // A bridge failure (`command` resolves null) is the same honest false, never a throw.
            const deadClient = {
                command: () => Promise.resolve(null),
            } as unknown as PanelClient;
            const dead = await makeSessionActions(makePanelDispatch(deadClient)).undo();
            assertEqual(dead.ok, false, "an unreachable panel bridge refuses honestly");
        },
    },

    // ------------------------------------------------------------- (c) panel-manifest commands
    {
        name: "projectPanelCommands: reads manifest commands from the roster, carrying their when-clause",
        run: () => {
            const spy = panelSpy();
            const commands = projectPanelCommands(rosterWithCommands(), spy.dispatch);
            assertEqual(commands.length, 2, "only the panel that declares commands contributes");
            const greet = commands.find((command) => command.id === "hello.greet");
            assert(greet !== undefined, "the manifest command is projected");
            if (greet !== undefined) {
                assertEqual(greet.category, "panel", "category");
                assertEqual(greet.title, "Greet", "title from the manifest");
                assertEqual(greet.when, "panelFocus == ext.hello", "the manifest when-clause is carried");
                assert(greet.docs.detail.includes("panel=ext.hello"), "docs name the source panel");
            }
            const wave = commands.find((command) => command.id === "hello.wave");
            assertEqual(wave?.when, "", "a command with no manifest when is always active");
        },
    },
    {
        name: "projectPanelCommands: a projected command dispatches to (panelId, commandId)",
        run: () => {
            const spy = panelSpy();
            const commands = projectPanelCommands(rosterWithCommands(), spy.dispatch);
            const greet = commands.find((command) => command.id === "hello.greet");
            void greet?.handler();
            assertEqual(spy.seen, ["ext.hello/hello.greet"], "dispatch carries panel id + command id");
        },
    },

    // ------------------------------------------------------------- assembly of all three sources
    {
        name: "buildCommandRegistry: unions all three sources and filters them by context together",
        run: () => {
            const contract = contractSpy();
            const editor = actionSpy();
            const session = sessionSpy();
            const panel = panelSpy();
            const registry = buildCommandRegistry({
                contractDispatch: contract.dispatch,
                editorActions: editor.actions,
                sessionActions: session.actions,
                roster: rosterWithCommands(),
                panelDispatch: panel.dispatch,
            });
            const contractCount = RPC_METHOD_NAMES.length;
            const editorCount = editorCommands(editor.actions).length;
            const sessionCount = sessionCommands(session.actions).length;
            const panelCount = 2;
            assertEqual(
                registry.size,
                contractCount + editorCount + sessionCount + panelCount,
                "every source's commands are present",
            );
            assert(registry.has(contractCommandId("describe")), "a contract command is present");
            assert(registry.has("view.theme.toggle"), "an editor command is present");
            assert(registry.has("session.undo"), "a session command is present");
            assert(registry.has("hello.greet"), "a panel command is present");

            // A focused-ext.hello context activates hello.greet; typing suppresses the guarded ones.
            const focused = registry.query({
                category: "panel",
                context: { panelFocus: "ext.hello", textInputFocus: false },
            });
            assertEqual(
                focused.map((command) => command.id).sort(),
                ["hello.greet", "hello.wave"],
                "both ext.hello panel commands are active when it is focused",
            );
            const elsewhere = registry.query({
                category: "panel",
                context: { panelFocus: "builtin.problems", textInputFocus: false },
            });
            assertEqual(
                elsewhere.map((command) => command.id),
                ["hello.wave"],
                "the panel-focus-guarded command drops when a different panel is focused",
            );
        },
    },
    {
        // ⚠ THE PART-1 GATE (M9 e13b-2). This case REPLACES one that asserted the opposite — that
        // assembly THROWS on a colliding manifest command — because that throw was the defect: it
        // propagated to `startCommandLayer`, which catches it into a generic "command layer
        // unavailable", so ONE colliding id from ONE package disabled the whole palette AND every
        // keybinding. e13b-2 is the change that makes the collision an EXTERNALLY SUPPLIED input, so
        // the old behaviour is a package-triggerable editor-wide outage.
        //
        // ⚠ NON-VACUITY PROVEN BY PLANTING, three ways, each reverted byte-exact:
        //   (P1) put the throwing `registerAll` back in `buildCommandRegistry` -> RED here (the
        //        colliding roster throws out of assembly, exactly as it used to).
        //   (P2) make `tryRegister` LAST-WINS (overwrite the incumbent) -> RED on the
        //        `view.theme.toggle` identity assertion below: the package's "Hijack" would answer a
        //        built-in id, which is the shadowing this rule exists to refuse.
        //   (P3) drop the rejection from the log (return it without recording) -> RED on the
        //        `rejections` assertion, i.e. the failure would be survivable but SILENT, which is
        //        only half the DoD.
        name: "buildCommandRegistry: a colliding manifest command costs ONE command, never the registry",
        run: () => {
            const collidingRoster = parsePanelRoster({
                contractMajor: 2,
                panels: [
                    {
                        id: "ext.evil",
                        commands: [
                            { id: "view.theme.toggle", title: "Hijack" },
                            // A SECOND, non-colliding command on the SAME panel. Without it the case
                            // could not tell "the colliding entry was refused" from "the whole panel
                            // source was dropped", which is a materially different failure mode.
                            { id: "evil.ok", title: "Fine" },
                        ],
                    },
                ],
            });
            assert(collidingRoster !== null, "roster parses");
            const editor = actionSpy();
            const panel = panelSpy();
            const registry = buildCommandRegistry({
                contractDispatch: contractSpy().dispatch,
                editorActions: editor.actions,
                sessionActions: sessionSpy().actions,
                roster: collidingRoster ?? { contractMajor: 2, panels: [] },
                panelDispatch: panel.dispatch,
            });

            // 1. THE PALETTE SURVIVES. Every other source is intact and complete.
            assertEqual(
                registry.size,
                RPC_METHOD_NAMES.length +
                    editorCommands(editor.actions).length +
                    sessionCommands(sessionSpy().actions).length +
                    1, // the panel's non-colliding command
                "assembly completed: every command except the colliding one is registered",
            );
            assert(registry.has(contractCommandId("describe")), "contract commands survive");
            assert(registry.has("session.undo"), "session commands survive");
            assert(registry.has("evil.ok"), "the panel's OTHER command survives");

            // 2. THE INCUMBENT KEEPS THE ID — the package did not shadow a built-in.
            assertEqual(
                registry.get("view.theme.toggle")?.title,
                "Toggle Theme",
                "the EDITOR command keeps view.theme.toggle; the package's Hijack was refused",
            );
            void registry.get("view.theme.toggle")?.handler();
            assertEqual(
                editor.calls,
                ["theme"],
                "…and executing that id runs the EDITOR action, never the package dispatch",
            );
            assertEqual(panel.seen, [], "the refused command's dispatch was never reachable");

            // 3. THE REFUSAL IS ATTRIBUTABLE — the DoD's second line.
            assertEqual(registry.rejections.length, 1, "exactly one refusal was recorded");
            const rejection = registry.rejections[0];
            assertEqual(rejection?.id, "view.theme.toggle", "it names the colliding id");
            assertEqual(rejection?.category, "panel", "…the source that lost");
            assertEqual(rejection?.incumbent, "editor", "…and the source that won");
        },
    },

    // ------------------------------------------- the palette's own id is unconditionally the editor's
    {
        // ⚠ PLANT: drop the `unregister` from `claimPaletteToggle` (leave the bare `tryRegisterAll`)
        // and this case goes RED — the package keeps the id and `execute` runs the PACKAGE's handler,
        // which is Ctrl+Shift+P dispatching into a package instead of opening the palette.
        //
        // This is the ONE built-in registered AFTER the panel source, so incumbent-wins — which
        // protects every other editor id — runs the WRONG way for it. See boot.ts § claimPaletteToggle.
        name: "commands: a squatter on workbench.palette.toggle is EVICTED, so the palette is never lost",
        run: (): void => {
            const registry = new CommandRegistry();
            let squatterRan = false;
            // A package manifest got there first — the state `projectPanelCommands` can produce today,
            // since manifest ids are projected as authored.
            registry.tryRegister({
                id: PALETTE_TOGGLE_COMMAND_ID,
                title: "Definitely The Palette",
                category: "panel",
                when: "",
                docs: { summary: "squat", detail: "squat" },
                handler: () => {
                    squatterRan = true;
                    return { ok: true, note: "squatter" };
                },
            });
            assertEqual(
                registry.get(PALETTE_TOGGLE_COMMAND_ID)?.category,
                "panel",
                "precondition: the package holds the palette's id",
            );

            let toggled = false;
            const evicted = claimPaletteToggle(
                registry,
                paletteCommands({
                    toggle: () => {
                        toggled = true;
                        return { ok: true, note: "palette opened" };
                    },
                }),
            );

            assertEqual(evicted?.title, "Definitely The Palette", "the squatter is reported, not silently dropped");
            assertEqual(
                registry.get(PALETTE_TOGGLE_COMMAND_ID)?.category,
                "editor",
                "the EDITOR now holds the palette's own id (the squatter's was `panel`)",
            );
            void registry.execute(PALETTE_TOGGLE_COMMAND_ID);
            assert(toggled, "Ctrl+Shift+P resolves to the palette");
            assert(!squatterRan, "…and never to the package that tried to take it");
        },
    },

    // The eviction is not a blanket unregister: with nobody squatting it must change nothing and
    // report nothing, or every clean boot would announce an eviction that never happened.
    {
        name: "commands: claimPaletteToggle reports no eviction on a clean registry",
        run: (): void => {
            const registry = new CommandRegistry();
            const evicted = claimPaletteToggle(registry, paletteCommands({ toggle: () => ({ ok: true, note: "" }) }));
            assertEqual(evicted, undefined, "nothing was evicted");
            assert(
                registry.get(PALETTE_TOGGLE_COMMAND_ID) !== undefined,
                "…and the palette command registered normally",
            );
        },
    },
];
