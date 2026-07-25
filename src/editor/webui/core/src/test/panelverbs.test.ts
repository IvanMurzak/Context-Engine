// T1 cases for the PANEL BRIDGE API VERBS (M9 e13b-2, panelverbs.ts) — what a third-party package
// panel may ask editor-core for over its authenticated port, and what it may not.
//
// WHAT THIS TIER PROVES AND WHY IT IS THE RIGHT ONE. The verbs are PURE editor-core logic over the
// command registry and the `when` evaluator; the transport underneath them is already pinned by
// `panelport.test.ts` against real ports in a real browser. So these cases drive the REAL verb table
// `makePanelBridgeVerbs` builds — the same object `panelhost.ts` hands `PanelPortBridge` — rather than
// a re-implementation of it, and the one case that needs a live port lives next door in
// `panelport.test.ts` (the production table over a real `MessageChannel`).
//
// ⚠ THE CENTRAL PROPERTY IS A NEGATIVE, SO IT IS PROVED BY A POSITIVE CONTROL. `bridge.ui.subscribe`
// must ship HARD-DENIED (C-F18), and "a verb that always refuses" is exactly the shape that passes
// every negative test vacuously — panelport.ts says so about the deny-all table itself, and the
// reasoning does not stop being true one layer up. So the refusal is measured TWICE: once against the
// shipped `DENY_ALL_CAPABILITY_GRANTS` and once against a stub source that GRANTS `ui_events`. The
// refusal CODE differs between them, which is only possible if the grant lookup really runs. A
// hardcoded refusal fails that case; so does deleting the verb from the table.
//
// ⚠ NON-VACUITY PROVEN BY PLANTING, each reverted byte-exact — recorded inline at its own case as
// `⚠ PLANT (x)`.

import { assert, assertEqual, type TestCase } from "./harness.js";
import { CommandRegistry, buildCommandRegistry } from "../commands.js";
import type { Command, CommandOutcome, EditorCommandActions, SessionCommandActions } from "../commands.js";
import { parsePanelRoster } from "../panels.js";
import { PANEL_BRIDGE_REFUSALS, PanelVerbRefusal } from "../panelport.js";
import type { PanelBridgeReply, PanelVerbHandler } from "../panelport.js";
import {
    CAPABILITY_UI_EVENTS,
    DENY_ALL_CAPABILITY_GRANTS,
    PANEL_COMMAND_FIELD_MAX_LENGTH,
    PANEL_COMMAND_REGISTRATION_LIMIT,
    PANEL_VERB_COMMANDS_EXECUTE,
    PANEL_VERB_COMMANDS_LIST,
    PANEL_VERB_COMMANDS_REGISTER,
    PANEL_VERB_COMMANDS_UNREGISTER,
    PANEL_VERB_COMMAND_INVOKE,
    PANEL_VERB_UI_SUBSCRIBE,
    RESERVED_COMMAND_PREFIXES,
    capabilityDenial,
    makePanelBridgeVerbs,
    validatePackageCommandId,
} from "../panelverbs.js";
import type { PanelCapabilityGrants, PanelCommandView } from "../panelverbs.js";
import type { WhenContext } from "../when.js";

// ------------------------------------------------------------------------------------- the fixture

const PACKAGE_ID = "ext.hello";
const PANEL_ID = "ext.hello.panel";

/** A grant source that RECORDS what it was asked. The positive control's other half. */
interface GrantSpy extends PanelCapabilityGrants {
    readonly asked: string[];
}
function grantSpy(answer: boolean): GrantSpy {
    const asked: string[] = [];
    return {
        asked,
        granted: (packageId: string, capability: string): boolean => {
            asked.push(`${packageId}/${capability}`);
            return answer;
        },
    };
}

interface VerbFixture {
    readonly verbs: ReadonlyMap<string, PanelVerbHandler>;
    readonly registry: CommandRegistry;
    /** Every `commands.invoke` the host sent DOWN the port, in order. */
    readonly invoked: string[];
    /**
     * Swap the live when-context — how a case drives the `when`-eval delegation.
     *
     * A METHOD rather than a mutable field, deliberately: the verb table closes over the provider,
     * and a field on the returned object is one spread away from being a COPY the provider never
     * reads. (It was exactly that at first, and the `when`-eval case caught it — a fixture bug that
     * would otherwise have read as "the delegation does not work".)
     */
    setContext(context: WhenContext): void;
}

interface FixtureOptions {
    readonly grants?: PanelCapabilityGrants;
    readonly declaredCapabilities?: readonly string[];
    readonly manifestCommandIds?: readonly string[];
    /** Replaces the registry with `undefined` — the "command layer never came up" arm. */
    readonly withoutRegistry?: boolean;
    /** Panel replies to `commands.invoke` with this. Default: accepted. */
    readonly invokeReply?: PanelBridgeReply;
}

/**
 * Build the REAL verb table over a REAL registry.
 *
 * The registry is the production assembly (`buildCommandRegistry`), not a hand-stocked one, so the
 * built-in ids these cases refuse to let a package touch are the ids the editor actually ships.
 */
function fixture(options: FixtureOptions = {}): VerbFixture {
    const roster = parsePanelRoster({
        contractMajor: 2,
        panels: [
            {
                id: PANEL_ID,
                capabilities: options.declaredCapabilities ?? [],
                commands: [
                    { id: "hello.manifest", title: "Manifest Command", when: "" },
                    { id: "hello.gated", title: "Gated", when: "panelFocus == ext.hello.panel" },
                ],
            },
        ],
    });
    if (roster === null) {
        throw new Error("test roster failed to parse");
    }
    const invoked: string[] = [];
    const noopEditor: EditorCommandActions = {
        focusNextPanel: () => ({ ok: true, note: "" }),
        focusPreviousPanel: () => ({ ok: true, note: "" }),
        moveActivePanel: () => ({ ok: true, note: "" }),
        closeActivePanel: () => ({ ok: true, note: "" }),
        toggleTheme: () => ({ ok: true, note: "theme" }),
        tearOutActivePanel: () => ({ ok: true, note: "" }),
        movePanelToPrimary: () => ({ ok: true, note: "" }),
    };
    const noopSession: SessionCommandActions = {
        undo: () => ({ ok: true, note: "" }),
        redo: () => ({ ok: true, note: "" }),
    };
    const registry = buildCommandRegistry({
        contractDispatch: (method) => ({ ok: true, note: method }),
        editorActions: noopEditor,
        sessionActions: noopSession,
        roster,
        panelDispatch: (panelId, commandId) => {
            invoked.push(`panel.command:${panelId}/${commandId}`);
            return { ok: true, note: commandId };
        },
    });
    const state: { context: WhenContext } = {
        context: { panelFocus: "", textInputFocus: false },
    };
    const verbs = makePanelBridgeVerbs({
        panelId: PANEL_ID,
        packageId: PACKAGE_ID,
        declaredCapabilities: options.declaredCapabilities ?? [],
        manifestCommandIds: options.manifestCommandIds ?? ["hello.manifest", "hello.gated"],
        registry: () => (options.withoutRegistry === true ? undefined : registry),
        whenContext: () => state.context,
        grants: options.grants ?? DENY_ALL_CAPABILITY_GRANTS,
        request: (verb: string, params: unknown): Promise<PanelBridgeReply> => {
            invoked.push(`${verb}:${JSON.stringify(params)}`);
            return Promise.resolve(options.invokeReply ?? { ok: true, result: null });
        },
    });
    return {
        registry,
        invoked,
        verbs,
        setContext: (context: WhenContext): void => {
            state.context = context;
        },
    };
}

/** Run a verb, awaiting whatever it returns. Throws exactly what the handler throws. */
async function call(fx: VerbFixture, verb: string, params?: unknown): Promise<unknown> {
    const handler = fx.verbs.get(verb);
    if (handler === undefined) {
        throw new Error(`the production verb table has no "${verb}"`);
    }
    return await handler(params);
}

/** Run a verb expecting a `PanelVerbRefusal`, and return it. Fails the case when none is thrown. */
async function refusalFrom(fx: VerbFixture, verb: string, params?: unknown): Promise<PanelVerbRefusal> {
    try {
        await call(fx, verb, params);
    } catch (error) {
        if (error instanceof PanelVerbRefusal) {
            return error;
        }
        throw error;
    }
    throw new Error(`"${verb}" answered where a PanelVerbRefusal was required`);
}

// ------------------------------------------------------------------------------------------ cases

export const panelVerbsTests: readonly TestCase[] = [
    // ------------------------------------------------- the ui_events gate (C-F18) — the point
    {
        // ⚠ PLANT (a): replace the `requireCapability` call in the `bridge.ui.subscribe` handler with
        // a bare `throw new PanelVerbRefusal(capabilityNotGranted, …)` — a hardcoded refusal — and
        // this case stays GREEN while the sibling positive-control case below goes RED. That split is
        // exactly why the two are separate cases: this one alone proves nothing about the lookup.
        name: "panelverbs: bridge.ui.subscribe is HARD-DENIED under the shipped deny-all grants (C-F18)",
        run: async (): Promise<void> => {
            const fx = fixture({ declaredCapabilities: [CAPABILITY_UI_EVENTS] });
            const refusal = await refusalFrom(fx, PANEL_VERB_UI_SUBSCRIBE, { topics: ["editor.ui.focus"] });
            assertEqual(
                refusal.code,
                PANEL_BRIDGE_REFUSALS.capabilityNotGranted,
                "the refusal is CAPABILITY-shaped, not the deny-all verb refusal: the verb exists and " +
                    "this package was not granted it, which is the only thing install consent can act on",
            );
            assert(
                refusal.message.includes(CAPABILITY_UI_EVENTS),
                `the refusal names the capability: ${refusal.message}`,
            );
            assert(
                refusal.message.includes(PACKAGE_ID),
                `…and the package it is about: ${refusal.message}`,
            );
        },
    },
    {
        // ⚠ THE POSITIVE CONTROL — this is what makes the case above non-vacuous. A verb that always
        // refuses cannot be told from a verb that consults a grant source; a verb whose refusal CODE
        // changes when the source's answer changes can only be the second.
        //
        // ⚠ PLANT (b): drop `bridge.ui.subscribe` from the table entirely (the "just omit it" design
        // this task rejected) -> RED here AND in the case above, because `call` reports a missing
        // verb. That is the proof that the ONE NAMED ENFORCEMENT POINT exists at all, rather than the
        // denial being an accident of the lookup miss.
        name: "panelverbs: the ui_events denial comes from a real GRANT LOOKUP (the e13c seam)",
        run: async (): Promise<void> => {
            const denying = grantSpy(false);
            const denied = await refusalFrom(
                fixture({ grants: denying, declaredCapabilities: [CAPABILITY_UI_EVENTS] }),
                PANEL_VERB_UI_SUBSCRIBE,
            );
            assertEqual(
                denying.asked,
                [`${PACKAGE_ID}/${CAPABILITY_UI_EVENTS}`],
                "the grant source was asked, exactly once, about THIS package and THIS capability",
            );
            assertEqual(
                denied.code,
                PANEL_BRIDGE_REFUSALS.capabilityNotGranted,
                "a refused grant is a capability refusal",
            );

            // The SAME verb, the SAME build, one different answer from the grant source.
            const granting = grantSpy(true);
            const allowed = await refusalFrom(
                fixture({ grants: granting, declaredCapabilities: [CAPABILITY_UI_EVENTS] }),
                PANEL_VERB_UI_SUBSCRIBE,
            );
            assertEqual(
                granting.asked,
                [`${PACKAGE_ID}/${CAPABILITY_UI_EVENTS}`],
                "the same lookup ran",
            );
            assertEqual(
                allowed.code,
                PANEL_BRIDGE_REFUSALS.verbNotGranted,
                "a GRANTED package gets past the gate and meets e13c's unbuilt half instead — a " +
                    "DIFFERENT refusal, which is the whole measurement: the code could not move if " +
                    "the denial were hardcoded",
            );
            assert(
                !allowed.message.includes("granted it"),
                "…and it no longer complains about the grant",
            );
        },
    },
    {
        name: "panelverbs: a manifest DECLARATION is not a grant, and the two diagnostics differ",
        run: () => {
            // The threat wearing the control's clothes: if the manifest were the gate, a package would
            // grant itself `ui_events` by editing a file it authors.
            assert(
                capabilityDenial(
                    DENY_ALL_CAPABILITY_GRANTS,
                    PACKAGE_ID,
                    CAPABILITY_UI_EVENTS,
                    [CAPABILITY_UI_EVENTS],
                ) !== "",
                "declaring the capability does NOT grant it",
            );
            const declared = capabilityDenial(
                DENY_ALL_CAPABILITY_GRANTS,
                PACKAGE_ID,
                CAPABILITY_UI_EVENTS,
                [CAPABILITY_UI_EVENTS],
            );
            const undeclared = capabilityDenial(
                DENY_ALL_CAPABILITY_GRANTS,
                PACKAGE_ID,
                CAPABILITY_UI_EVENTS,
                [],
            );
            assert(
                declared !== undeclared,
                "a package that asked and was refused is an install-consent question; one that never " +
                    "asked is a manifest bug — one message would send an author to the wrong file",
            );
            assertEqual(
                capabilityDenial(grantSpy(true), PACKAGE_ID, CAPABILITY_UI_EVENTS, []),
                "",
                "a grant is a grant even with nothing declared — the grant source is the decision",
            );
        },
    },

    // ------------------------------------------------------------------ bridge.commands.* scoping
    {
        name: "panelverbs: bridge.commands.list reports ONLY this panel's commands, when-resolved",
        run: async (): Promise<void> => {
            const fx = fixture();
            const listed = (await call(fx, PANEL_VERB_COMMANDS_LIST)) as PanelCommandView[];
            assertEqual(
                listed.map((view) => view.id).sort(),
                ["hello.gated", "hello.manifest"],
                "the panel's own manifest commands, and NOT the editor's whole command surface — a " +
                    "package must not be able to enumerate what the editor can do",
            );
            assertEqual(
                listed.find((view) => view.id === "hello.gated")?.active,
                false,
                "a when-guarded command is reported INACTIVE while its clause does not hold",
            );
            assertEqual(
                listed.find((view) => view.id === "hello.manifest")?.active,
                true,
                "an unguarded one is active",
            );

            // THE `when`-EVAL DELEGATION: the SAME table, one different context.
            fx.setContext({ panelFocus: PANEL_ID, textInputFocus: false });
            const focused = (await call(fx, PANEL_VERB_COMMANDS_LIST)) as PanelCommandView[];
            assertEqual(
                focused.find((view) => view.id === "hello.gated")?.active,
                true,
                "…and ACTIVE once the live when-context satisfies it",
            );
        },
    },
    {
        // ⚠ PLANT (c): drop the `owns(id)` guard from `bridge.commands.execute` -> RED here, and the
        // package can drive `view.window.tearOut` (open an OS window) and every projected contract
        // verb. This is the escalation the scoping exists to refuse.
        name: "panelverbs: bridge.commands.execute refuses every command that is not this panel's",
        run: async (): Promise<void> => {
            const fx = fixture();
            for (const foreign of [
                "view.window.tearOut", // an editor command with real effect
                "view.theme.toggle",
                "session.undo",
                "contract.describe", // a projected contract verb
                "no.such.command", // and one that does not exist
            ]) {
                const refusal = await refusalFrom(fx, PANEL_VERB_COMMANDS_EXECUTE, { id: foreign });
                assertEqual(
                    refusal.code,
                    PANEL_BRIDGE_REFUSALS.capabilityNotGranted,
                    `${foreign}: refused`,
                );
                // IDENTICAL wording for a real id and an imaginary one — otherwise the refusal is an
                // ORACLE for the editor's command surface, which is an information leak a sandboxed
                // package should not get for free.
                assert(
                    refusal.message.includes(PANEL_ID) && refusal.message.includes(foreign),
                    `${foreign}: the refusal names the panel and the id`,
                );
            }
            assertEqual(fx.invoked, [], "nothing was dispatched anywhere");
        },
    },
    {
        name: "panelverbs: bridge.commands.execute runs the panel's OWN command, honouring its when",
        run: async (): Promise<void> => {
            const fx = fixture();
            // The unguarded one runs and reaches the PANEL over the port (`commands.invoke`) — the
            // route an iframe panel's command must take, since it has no C++ model to answer
            // `panel.command`.
            const ok = (await call(fx, PANEL_VERB_COMMANDS_EXECUTE, {
                id: "hello.manifest",
            })) as CommandOutcome;
            assertEqual(ok.ok, true, "its own command executes");
            assertEqual(
                fx.invoked,
                [`panel.command:${PANEL_ID}/hello.manifest`],
                "…through the registry's own handler, which is the projected panel dispatch",
            );

            // The GUARDED one is refused as an ORDINARY OUTCOME (not a PanelVerbRefusal): "not
            // applicable right now" is a fact about context, not about authority.
            const gated = (await call(fx, PANEL_VERB_COMMANDS_EXECUTE, {
                id: "hello.gated",
            })) as CommandOutcome;
            assertEqual(gated.ok, false, "a when-inactive command does not run");
            assert(
                gated.note.includes("hello.gated"),
                `…and says which one: ${gated.note}`,
            );
            assertEqual(fx.invoked.length, 1, "nothing further was dispatched");
        },
    },

    // ------------------------------------------------------------ bridge.commands.register grammar
    {
        name: "panelverbs: bridge.commands.register accepts a namespaced id and wires it to the port",
        run: async (): Promise<void> => {
            const fx = fixture();
            const before = fx.registry.size;
            const result = (await call(fx, PANEL_VERB_COMMANDS_REGISTER, {
                commands: [{ id: `${PACKAGE_ID}.refresh`, title: "Refresh", when: "" }],
            })) as { registered: string[]; rejected: unknown[] };
            assertEqual(result.registered, [`${PACKAGE_ID}.refresh`], "it was accepted");
            assertEqual(result.rejected, [], "with nothing refused");
            assertEqual(fx.registry.size, before + 1, "the ONE registry grew by exactly one");

            // It is a REAL registry entry — the palette and the keymap read the same object.
            const command = fx.registry.get(`${PACKAGE_ID}.refresh`) as Command;
            assertEqual(command.category, "panel", "categorised as a panel command");
            assert(
                command.docs.detail.includes(PACKAGE_ID),
                `its docs attribute it to the package: ${command.docs.detail}`,
            );

            // And executing it reaches the PACKAGE over the port, not `panel.command`.
            const outcome = await command.handler();
            assertEqual(outcome.ok, true, "the handler resolves through the port");
            assertEqual(
                fx.invoked,
                [`${PANEL_VERB_COMMAND_INVOKE}:{"id":"${PACKAGE_ID}.refresh"}`],
                "the host asked the PANEL to run it — the commands.invoke direction e13b-1 built",
            );
        },
    },
    {
        // ⚠ PLANT (d): delete the `RESERVED_COMMAND_PREFIXES` loop from `validatePackageCommandId` ->
        // RED here on the first reserved id. Without it a package can SQUAT an id the editor has not
        // shipped yet (incumbent-wins cannot help: the package would simply be first).
        name: "panelverbs: a package may not register into the editor's namespaces, nor outside its own",
        run: async (): Promise<void> => {
            const fx = fixture();
            const bad = [
                // The editor's own sources, every reserved prefix, plus one id that is merely not
                // namespaced under the package.
                ...RESERVED_COMMAND_PREFIXES.map((prefix) => `${prefix}squatted`),
                "otherpkg.command", // another package's namespace
                "bare", // no namespace at all
                PACKAGE_ID, // the bare package id is not a sub-command of it
                "", // empty
                `${PACKAGE_ID}.${"x".repeat(PANEL_COMMAND_FIELD_MAX_LENGTH)}`, // over the bound
            ];
            const result = (await call(fx, PANEL_VERB_COMMANDS_REGISTER, {
                commands: bad.map((id) => ({ id, title: "Squat" })),
            })) as { registered: string[]; rejected: { id: string; diagnostic: string }[] };
            assertEqual(result.registered, [], "not one of them was accepted");
            assertEqual(
                result.rejected.length,
                // TWO of them never reach the grammar at all: the PARSER drops an id that is not a
                // bounded, non-empty string, so the empty one and the over-long one are refused a
                // layer earlier and are not reported back (there is nothing safe to name them by —
                // echoing an unbounded attacker-chosen id in a diagnostic is the amplification
                // `boundedString` exists to refuse). MEASURED, not assumed: this count was 9 first,
                // and the tier reported 8.
                bad.length - 2,
                "every id the grammar sees is reported back with its own diagnostic",
            );
            for (const rejection of result.rejected) {
                assert(rejection.diagnostic.length > 0, `${rejection.id}: has a diagnostic`);
            }
            // Nothing leaked into the registry — asserted on the ids, not only on the size.
            for (const id of bad) {
                assert(!fx.registry.has(id), `${id} is not in the registry`);
            }
            // And the built-ins those prefixes belong to are untouched.
            assertEqual(
                fx.registry.get("view.theme.toggle")?.category,
                "editor",
                "the editor keeps its own commands",
            );
        },
    },
    {
        name: "panelverbs: a runtime registration colliding with a built-in is refused, non-fatally",
        run: async (): Promise<void> => {
            // The one shape that gets past the namespace grammar and still collides: another package
            // panel already holding the id, or this panel registering the same id twice.
            const fx = fixture();
            const first = (await call(fx, PANEL_VERB_COMMANDS_REGISTER, {
                commands: [{ id: `${PACKAGE_ID}.dup`, title: "First" }],
            })) as { registered: string[] };
            assertEqual(first.registered.length, 1, "the first wins");
            const second = (await call(fx, PANEL_VERB_COMMANDS_REGISTER, {
                commands: [{ id: `${PACKAGE_ID}.dup`, title: "Second" }],
            })) as { registered: string[]; rejected: { id: string; diagnostic: string }[] };
            assertEqual(second.registered, [], "the second is refused");
            assertEqual(second.rejected.length, 1, "…and reported");
            assert(
                second.rejected[0]?.diagnostic.includes("First") === true,
                `the refusal names the incumbent: ${String(second.rejected[0]?.diagnostic)}`,
            );
            assertEqual(
                fx.registry.get(`${PACKAGE_ID}.dup`)?.title,
                "First",
                "incumbent-wins holds for the runtime path too",
            );
            // THE POINT OF PART 1: the collision did not take the registry with it.
            assert(fx.registry.has("view.theme.toggle"), "the palette's command set is intact");
        },
    },
    {
        // ⚠ PLANT (e): remove the `PANEL_COMMAND_REGISTRATION_LIMIT` check from `registerOne` -> RED
        // here. Without it `bridge.commands.register` is unbounded allocation reachable from
        // untrusted code, and every entry is also a palette row.
        name: "panelverbs: a panel's runtime registrations are BOUNDED",
        run: async (): Promise<void> => {
            const fx = fixture();
            const many = Array.from({ length: PANEL_COMMAND_REGISTRATION_LIMIT + 10 }, (_unused, i) => ({
                id: `${PACKAGE_ID}.c${String(i)}`,
                title: `C${String(i)}`,
            }));
            const first = (await call(fx, PANEL_VERB_COMMANDS_REGISTER, { commands: many })) as {
                registered: string[];
                rejected: unknown[];
            };
            assertEqual(
                first.registered.length,
                PANEL_COMMAND_REGISTRATION_LIMIT,
                "the cap is enforced within a single call",
            );
            // …and across calls, which is the loop an unbounded panel would actually write.
            const again = (await call(fx, PANEL_VERB_COMMANDS_REGISTER, {
                commands: [{ id: `${PACKAGE_ID}.overflow`, title: "Overflow" }],
            })) as { registered: string[]; rejected: { diagnostic: string }[] };
            assertEqual(again.registered, [], "a later call cannot get past it either");
            assert(
                again.rejected[0]?.diagnostic.includes("maximum") === true,
                `the refusal says why: ${String(again.rejected[0]?.diagnostic)}`,
            );
        },
    },
    {
        name: "panelverbs: bridge.commands.unregister withdraws only what THIS panel registered",
        run: async (): Promise<void> => {
            const fx = fixture();
            await call(fx, PANEL_VERB_COMMANDS_REGISTER, {
                commands: [{ id: `${PACKAGE_ID}.temp`, title: "Temp" }],
            });
            const result = (await call(fx, PANEL_VERB_COMMANDS_UNREGISTER, {
                ids: [
                    `${PACKAGE_ID}.temp`, // its own runtime registration — withdrawn
                    "hello.manifest", // its own MANIFEST command — not its to withdraw
                    "view.theme.toggle", // an editor command — certainly not
                ],
            })) as { unregistered: string[]; refused: { id: string }[] };
            assertEqual(result.unregistered, [`${PACKAGE_ID}.temp`], "only the runtime one goes");
            assertEqual(
                result.refused.map((entry) => entry.id).sort(),
                ["hello.manifest", "view.theme.toggle"],
                "the manifest command and the editor command are both refused",
            );
            assert(!fx.registry.has(`${PACKAGE_ID}.temp`), "the withdrawn id is gone");
            assert(fx.registry.has("hello.manifest"), "the manifest command survives");
            assert(fx.registry.has("view.theme.toggle"), "and so does the editor's");
        },
    },

    // ------------------------------------------------------------------------- totality + fallbacks
    {
        name: "panelverbs: every verb is TOTAL against malformed params from an untrusted peer",
        run: async (): Promise<void> => {
            const fx = fixture();
            const junk: unknown[] = [
                undefined,
                null,
                42,
                "string",
                [],
                {},
                { commands: "not-an-array" },
                { commands: [null, 7, { title: "no id" }, { id: 5 }] },
                { ids: "not-an-array" },
                { ids: [null, {}, 1] },
                { id: { nested: true } },
            ];
            for (const params of junk) {
                // A malformed `register` / `unregister` / `list` must ANSWER, never throw: an
                // uncaught throw out of a handler reaches the generic host-fault path and tells the
                // panel nothing.
                await call(fx, PANEL_VERB_COMMANDS_LIST, params);
                const registered = (await call(fx, PANEL_VERB_COMMANDS_REGISTER, params)) as {
                    registered: string[];
                };
                assertEqual(registered.registered, [], `register(${JSON.stringify(params)}): nothing`);
                const unregistered = (await call(fx, PANEL_VERB_COMMANDS_UNREGISTER, params)) as {
                    unregistered: string[];
                };
                assertEqual(unregistered.unregistered, [], "unregister: nothing");
                // `execute` refuses rather than answering — it is the one verb with an authority
                // question, and an unreadable id is not one of this panel's commands.
                const refusal = await refusalFrom(fx, PANEL_VERB_COMMANDS_EXECUTE, params);
                assertEqual(refusal.code, PANEL_BRIDGE_REFUSALS.capabilityNotGranted, "execute refuses");
            }
            assertEqual(
                fx.registry.rejections.length,
                0,
                "and none of it reached the registry at all",
            );
        },
    },
    {
        name: "panelverbs: with no command layer, every command verb refuses honestly",
        run: async (): Promise<void> => {
            // `startCommandLayer` returns `undefined` when the layer did not come up. The tables are
            // built before it runs, so this arm is real rather than defensive.
            const fx = fixture({ withoutRegistry: true });
            for (const verb of [
                PANEL_VERB_COMMANDS_LIST,
                PANEL_VERB_COMMANDS_REGISTER,
                PANEL_VERB_COMMANDS_UNREGISTER,
                PANEL_VERB_COMMANDS_EXECUTE,
            ]) {
                const refusal = await refusalFrom(fx, verb, { commands: [], ids: [], id: "x" });
                assertEqual(
                    refusal.code,
                    PANEL_BRIDGE_REFUSALS.verbNotGranted,
                    `${verb}: refused, not crashed`,
                );
            }
        },
    },
    {
        name: "panelverbs: a panel that REFUSES commands.invoke yields an honest ok:false outcome",
        run: async (): Promise<void> => {
            const fx = fixture({
                invokeReply: {
                    ok: false,
                    error: { code: PANEL_BRIDGE_REFUSALS.timeout, message: "no reply" },
                },
            });
            await call(fx, PANEL_VERB_COMMANDS_REGISTER, {
                commands: [{ id: `${PACKAGE_ID}.dead`, title: "Dead" }],
            });
            const outcome = await (fx.registry.get(`${PACKAGE_ID}.dead`) as Command).handler();
            assertEqual(outcome.ok, false, "a panel that did not handle its own command reports so");
            assert(
                outcome.note.includes(PANEL_BRIDGE_REFUSALS.timeout),
                `…carrying the refusal code so the cause is legible: ${outcome.note}`,
            );
        },
    },
    {
        // ⚠ THE RESERVED-PREFIX RULE IS ONLY REACHABLE THROUGH A PACKAGE NAMED FOR ONE, and that is
        // the case that makes it non-vacuous. MEASURED: with an ordinary package id the namespace
        // rule already refuses `view.theme.toggle` (it is not under `ext.hello.`), so deleting the
        // reserved-prefix loop leaves every OTHER case in this file green — the check earns its place
        // solely against a package that CALLS ITSELF `view` / `session` / `contract` / `workbench` /
        // `editor`, for which the namespace rule is satisfied by construction and would otherwise
        // hand it the editor's own command ids.
        name: "panelverbs: the grammar's package-namespace rule mirrors uibus's, and is total",
        run: () => {
            assertEqual(validatePackageCommandId(PACKAGE_ID, `${PACKAGE_ID}.ok`), "", "the happy shape");
            for (const [pkg, id] of [
                [PACKAGE_ID, "view.theme.toggle"],
                [PACKAGE_ID, "otherpkg.x"],
                [PACKAGE_ID, PACKAGE_ID],
                [PACKAGE_ID, `${PACKAGE_ID}.`],
                ["", "anything.x"],
                [PACKAGE_ID, ""],
            ] as const) {
                assert(
                    validatePackageCommandId(pkg, id) !== "",
                    `("${pkg}", "${id}") is refused with a diagnostic`,
                );
            }
            // A package named for a reserved namespace satisfies the namespace rule and must STILL be
            // refused — the reserved-prefix loop's whole reason to exist.
            for (const prefix of RESERVED_COMMAND_PREFIXES) {
                const squatter = prefix.slice(0, -1); // "view." -> "view"
                const denial = validatePackageCommandId(squatter, `${squatter}.theme.toggle`);
                assert(
                    denial !== "",
                    `a package calling itself "${squatter}" may not register "${squatter}.theme.toggle"`,
                );
                assert(
                    denial.includes("reserved"),
                    `…and is told why, by the reserved-namespace rule: ${denial}`,
                );
            }
        },
    },
];
