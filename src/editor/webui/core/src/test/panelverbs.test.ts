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
    PANEL_STATE_MAX_JSON_LENGTH,
    PANEL_VERB_COMMAND_INVOKE,
    PANEL_VERB_STATE_GET,
    PANEL_DAEMON_METHOD_MAX_LENGTH,
    PANEL_VERB_CALL,
    PANEL_VERB_EVENTS_ACK,
    PANEL_VERB_EVENTS_SUBSCRIBE,
    PANEL_VERB_EVENTS_UNSUBSCRIBE,
    PANEL_VERB_STATE_SET,
    PANEL_VERB_THEME_TOKENS,
    PANEL_VERB_UI_SUBSCRIBE,
    RESERVED_COMMAND_PREFIXES,
    capabilityDenial,
    readUiTopicsParam,
    daemonRefusalCode,
    makePanelBridgeVerbs,
    sanitizePanelState,
    validatePackageCommandId,
} from "../panelverbs.js";
import type {
    PanelCapabilityGrants,
    PanelCommandRejection,
    PanelCommandView,
    PanelDaemonOutcome,
    PanelUiSubscribeOutcome,
} from "../panelverbs.js";
import type { ThemeChangedPayload } from "../theme.js";
import { UI_TOPIC_FOCUS, UI_TOPIC_LAYOUT } from "../uibus.js";
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
    /** The table's teardown — what `IframePanelRenderer.dispose` calls. */
    readonly dispose: () => void;
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
    /** Swap the theme provider — a METHOD for the same reason `setContext` is one (M9 e13d). */
    setTheme(payload: ThemeChangedPayload | undefined): void;
    /** What the panel's own store currently holds, read from OUTSIDE the verbs (M9 e13d). */
    storedState(): unknown;
    /** Every `write` the store saw, in order — the witness that `set` reached the store at all. */
    readonly writes: unknown[];
    /**
     * Every daemon forward the table made, in order (M9 e13c-1).
     *
     * The witness for the property `bridge.call` exists to establish: the recorded entries carry the
     * METHOD and PARAMS the panel sent and NOTHING ELSE, because the package is closed over in the
     * `daemonCall` the fixture supplied. A case that sends `packageId` in its request and finds it
     * absent here has proved the identity is un-forgeable at this layer.
     */
    readonly daemonCalls: { method: string; params: unknown; arity: number }[];
    /**
     * Every `bridge.ui.subscribe` the table forwarded to the fan-out seam, in order (M9 e13c-4).
     *
     * The witness for the property the granted half exists to establish: an entry here means the
     * request got PAST `requireCapability` and reached the delivery seam, which is a fact no refusal
     * assertion can express. `arity` is what pins the no-package-argument shape (see `uiSubscribe`).
     */
    readonly uiSubscriptions: { topics: string[]; arity: number }[];
}

interface FixtureOptions {
    readonly grants?: PanelCapabilityGrants;
    /**
     * Model "no theme has been applied in this window" — the arm `bridge.theme.tokens` refuses on.
     *
     * A BOOLEAN, matching `withoutRegistry` below rather than an optional payload: an optional
     * `theme?: ThemeChangedPayload | undefined` cannot distinguish "absent" from "explicitly
     * undefined" without an `"theme" in options` probe, and every case that wants a theme wants the
     * default one anyway (the ones that need a DIFFERENT theme set it through `fx.setTheme`).
     */
    readonly withoutTheme?: boolean;
    readonly declaredCapabilities?: readonly string[];
    readonly manifestCommandIds?: readonly string[];
    /** Replaces the registry with `undefined` — the "command layer never came up" arm. */
    readonly withoutRegistry?: boolean;
    /**
     * Build over an EXISTING registry instead of a fresh one — how the reopened-panel case models
     * the real lifecycle, where the registry outlives every panel and only the verb table is new.
     */
    readonly registry?: CommandRegistry;
    /** Panel replies to `commands.invoke` with this. Default: accepted. */
    readonly invokeReply?: PanelBridgeReply;
    /**
     * What the Shell answers a forwarded `bridge.call` with (M9 e13c-1). Default: accepted, with the
     * method echoed back as the result so a case can prove WHICH method landed.
     */
    readonly daemonOutcome?: PanelDaemonOutcome;
    /**
     * What the `editor.ui` fan-out seam answers a granted `bridge.ui.subscribe` with (M9 e13c-4).
     * Default: ACCEPT every requested topic. Supply a `diagnostic` to drive the refusing arm.
     */
    readonly uiOutcome?: PanelUiSubscribeOutcome;
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
    const registry =
        options.registry ??
        buildCommandRegistry({
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
    // The e13d halves, held in the fixture so a case can drive them from outside the table: the
    // theme provider is late-bound in production too (the theme changes under a mounted panel), and
    // the store is the renderer's in production (its lifetime IS the panel's).
    const theme: { payload: ThemeChangedPayload | undefined } = {
        payload: options.withoutTheme === true ? undefined : themePayload(),
    };
    const store: { value: unknown } = { value: null };
    const writes: unknown[] = [];
    // The e13c-1 daemon fan-in. Note the SHAPE of this stand-in: it takes a method and params and no
    // package — the same signature `makePackageDaemonCall` returns after closing over one — so the
    // fixture cannot accidentally give the table a wider surface than production has.
    const daemonCalls: { method: string; params: unknown; arity: number }[] = [];
    // The e13c-4 `editor.ui` fan-in, recorded the same way and for the same reason.
    const uiSubscriptions: { topics: string[]; arity: number }[] = [];
    const table = makePanelBridgeVerbs({
        panelId: PANEL_ID,
        packageId: PACKAGE_ID,
        declaredCapabilities: options.declaredCapabilities ?? [],
        manifestCommandIds: options.manifestCommandIds ?? ["hello.manifest", "hello.gated"],
        registry: () => (options.withoutRegistry === true ? undefined : registry),
        whenContext: () => state.context,
        grants: options.grants ?? DENY_ALL_CAPABILITY_GRANTS,
        themeTokens: () => theme.payload,
        state: {
            read: (): unknown => store.value,
            write: (value: unknown): void => {
                store.value = value;
                writes.push(value);
            },
        },
        // ⚠ RECORDS ITS FULL ARITY, and that is precisely what makes the PLANT on the first case real.
        // A 2-arity arrow silently DISCARDS a third argument, so a recorded `{method, params}` object
        // literal is the FIXTURE's own shape and no production mutation can change it — the plant
        // "give `PanelDaemonCall` a package argument at all" would then stay GREEN, a non-vacuity
        // claim that was itself vacuous. Rest args are what let the fixture OBSERVE the extra argument
        // the production type promises not to have.
        daemonCall: (...args: unknown[]): Promise<PanelDaemonOutcome> => {
            const method = args[0] as string;
            daemonCalls.push({ method, params: args[1], arity: args.length });
            return Promise.resolve(
                options.daemonOutcome ?? { ok: true, result: { echoed: method } },
            );
        },
        // M9 e13c-4 — the `editor.ui` fan-out seam. RECORDS ITS FULL ARITY for exactly the reason
        // `daemonCall` above does: the production type promises NO package argument, and a fixture
        // that declared `(topics)` would silently discard one, making the plant "give
        // `PanelUiSubscribe` a package argument" vacuous. The default answer ACCEPTS, so the granted
        // half is measured against a working fan-out; `uiOutcome` supplies the refusing arm.
        uiSubscribe: (...args: unknown[]): PanelUiSubscribeOutcome => {
            const topics = args[0] as readonly string[];
            uiSubscriptions.push({ topics: [...topics], arity: args.length });
            return options.uiOutcome ?? { topics: [...topics], diagnostic: "" };
        },
        request: (verb: string, params: unknown): Promise<PanelBridgeReply> => {
            invoked.push(`${verb}:${JSON.stringify(params)}`);
            return Promise.resolve(options.invokeReply ?? { ok: true, result: null });
        },
    });
    return {
        registry,
        invoked,
        writes,
        daemonCalls,
        uiSubscriptions,
        verbs: table.verbs,
        dispose: table.dispose,
        setContext: (context: WhenContext): void => {
            state.context = context;
        },
        setTheme: (payload: ThemeChangedPayload | undefined): void => {
            theme.payload = payload;
        },
        storedState: (): unknown => store.value,
    };
}

/** A theme payload shaped exactly like the one `ThemeEngine` broadcasts. */
function themePayload(themeId = "builtin.dark"): ThemeChangedPayload {
    return {
        themeId,
        name: themeId === "builtin.dark" ? "Dark" : "Light",
        appearance: themeId === "builtin.dark" ? "dark" : "light",
        highContrast: false,
        reducedMotion: false,
        variables: { "--ctx-colors-panel": themeId === "builtin.dark" ? "#0a0a0a" : "#ffffff" },
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
    // -------------------------------- the daemon EVENT subscription (M9 e13c-2, design 04 §5 / 05 §1)
    {
        // ⚠ PLANT: point any of the three at another daemon method (`subscribe` -> `query`). The
        // per-verb method assertions below go RED, and only they do — which is what proves the mapping
        // is real rather than "some forward happened".
        name: "panelverbs: bridge.events.* forward the THREE subscription methods, each to its own",
        run: async (): Promise<void> => {
            const fx = fixture();
            await call(fx, PANEL_VERB_EVENTS_SUBSCRIBE, { topics: ["diagnostics"] });
            await call(fx, PANEL_VERB_EVENTS_UNSUBSCRIBE, { subId: "sub-1" });
            await call(fx, PANEL_VERB_EVENTS_ACK, { subId: "sub-1", seq: 9 });

            assertEqual(fx.daemonCalls.length, 3, "three forwards, one per verb");
            assertEqual(fx.daemonCalls[0]?.method, "subscribe", "…the daemon's own method names");
            assertEqual(fx.daemonCalls[1]?.method, "unsubscribe", "…one per verb, never collapsed");
            assertEqual(fx.daemonCalls[2]?.method, "ack", "…so the Shell allowlist sees exactly three");
            // THE PARAMS TRAVEL VERBATIM — the contract registry owns those shapes, and a second copy
            // here would be a drifting parser in front of the daemon's own validator.
            assertEqual(
                JSON.stringify(fx.daemonCalls[0]?.params),
                JSON.stringify({ topics: ["diagnostics"] }),
                "subscribe's topics reach the daemon unaltered",
            );
            assertEqual(
                JSON.stringify(fx.daemonCalls[2]?.params),
                JSON.stringify({ subId: "sub-1", seq: 9 }),
                "ack's cursor reaches the daemon unaltered",
            );
            // NO PACKAGE ARGUMENT, for the same structural reason `bridge.call` has none: the closure
            // IS the package, so no request member could ever name another package's session.
            for (const forwarded of fx.daemonCalls) {
                assertEqual(forwarded.arity, 2, "every forward is (method, params) and nothing more");
            }
        },
    },
    {
        // ⚠ PLANT: return the outcome instead of throwing (drop the `!outcome.ok` branch in
        // `forwardDaemon`). This case goes RED on the refusal that never arrives.
        name: "panelverbs: a daemon refusal on bridge.events.subscribe relays its ORIGINATING code",
        run: async (): Promise<void> => {
            const denied = fixture({
                daemonOutcome: {
                    ok: false,
                    code: "scope.denied",
                    message: "the daemon refused 'subscribe' for package 'hello-panel'",
                },
            });
            const refusal = await refusalFrom(denied, PANEL_VERB_EVENTS_SUBSCRIBE, { topics: [] });
            // THE SAME MAPPING `bridge.call` uses — written once (`forwardDaemon`) precisely so a
            // package cannot get a different answer depending on which verb provoked the refusal.
            assertEqual(
                refusal.code,
                PANEL_BRIDGE_REFUSALS.capabilityNotGranted,
                "scope.denied is a capability question, whichever verb met it",
            );
            assert(
                refusal.message.includes("scope.denied"),
                "and the originating code travels verbatim, or the enforcement would be unobservable",
            );
        },
    },
    {
        // The e13c-2 REGRESSION GUARD for the parent DoD line "`bridge.ui.subscribe` remains denied
        // without a grant". `bridge.events.*` and `bridge.ui.subscribe` are deliberately DIFFERENT
        // surfaces — daemon facts vs the editor-LOCAL `editor.ui` bus (D7: `editor.ui` never reaches
        // the daemon) — so wiring the first must not have wired the second. They share a MECHANISM,
        // not a route, and this is the assertion that keeps that true.
        name: "panelverbs: filling bridge.events.* did NOT open bridge.ui.subscribe (D7 / C-F18)",
        run: async (): Promise<void> => {
            // ⚠ THE GRANT IS SPY-ANSWERED **TRUE**, WHICH IS THE WHOLE POINT OF THIS CASE. Run with
            // the default deny-all source, `requireCapability` throws before ANY of the handler body
            // executes, so "no editor.ui subscription reached the daemon" holds because the path was
            // never taken — and the mutation this case exists to catch (replacing the handler's
            // terminal throw with `return await forwardDaemon("subscribe", params)`, i.e. wiring
            // `editor.ui` onto the daemon, the exact D7 violation) stayed GREEN. Passing the gate is
            // what puts the code under test in the way: past `requireCapability`, the only thing
            // between `bridge.ui.subscribe` and the daemon is the refusal below.
            const grants = grantSpy(true);
            const fx = fixture({ grants, declaredCapabilities: [CAPABILITY_UI_EVENTS] });
            // The events verbs answer…
            await call(fx, PANEL_VERB_EVENTS_SUBSCRIBE, { topics: ["diagnostics"] });
            assertEqual(fx.daemonCalls.length, 1, "the daemon subscription is live");
            // …while the ui bus verb goes somewhere else entirely.
            //
            // ⚠ UPDATED BY M9 e13c-4. Until e13c-4 this asserted a SECOND refusal (`verb_not_granted`,
            // "not wired"), because the granted half did not exist; now it is ANSWERED — and the D7
            // claim is STRONGER for it. The old shape could only say "the daemon was not reached by a
            // verb that reached nothing at all"; this one says the subscription really happened, on
            // the LOCAL bus seam, and the daemon still never saw it.
            const answered = await call(fx, PANEL_VERB_UI_SUBSCRIBE, {
                topics: [UI_TOPIC_FOCUS],
            });
            assertEqual(
                grants.asked,
                [`${PACKAGE_ID}/${CAPABILITY_UI_EVENTS}`],
                "the ui_events gate really ran and really said yes — the positive control",
            );
            assertEqual(
                answered,
                { topics: [UI_TOPIC_FOCUS] },
                "past the gate the editor.ui subscription is ACCEPTED",
            );
            // IT WENT TO THE LOCAL FAN-OUT SEAM...
            assertEqual(
                fx.uiSubscriptions.map((entry) => entry.topics),
                [[UI_TOPIC_FOCUS]],
                "the editor.ui subscription landed on the LOCAL bus seam",
            );
            // ...AND NEVER REACHED THE DAEMON. The positive artifact for D7: `bridge.events.subscribe`
            // above produced exactly one forward, and a `ui.subscribe` wired onto that same daemon
            // fan-out would show up as a SECOND one. The count is what pins `editor.ui` to the local
            // bus — and it is only meaningful because the subscribe SUCCEEDED, so the path really ran.
            assertEqual(fx.daemonCalls.length, 1, "no editor.ui subscription was forwarded to the daemon");
        },
    },
    // ------------------------------------- the daemon fan-in (M9 e13c-1, design 04 §5 / 08 §2)
    {
        // ⚠ PLANT: make the handler read a package id off `params` and hand it to `daemonCall` (i.e.
        // give `PanelDaemonCall` a package argument at all) — RED here, because the forged id would
        // then have to appear in the recorded call.
        name: "panelverbs: bridge.call forwards the method VERBATIM and takes NO package from the payload",
        run: async (): Promise<void> => {
            const fx = fixture({});

            const result = await call(fx, PANEL_VERB_CALL, {
                method: "editor.scene-tree",
                params: { path: "scenes/main.scene.json" },
                // ⚠ THE FORGERY. A hostile panel naming another package — the ONE thing the closed-over
                // identity has to make impossible. It must reach no code that reads it.
                packageId: "some.other.package",
            });

            assertEqual(fx.daemonCalls.length, 1, "exactly one forward happened");
            assertEqual(
                fx.daemonCalls[0]?.method,
                "editor.scene-tree",
                "the method travels VERBATIM — the panel-callable ALLOWLIST is the Shell's, at the " +
                    "forwarder that holds the session, so a second copy here would only drift",
            );
            assertEqual(
                JSON.stringify(fx.daemonCalls[0]?.params),
                JSON.stringify({ path: "scenes/main.scene.json" }),
                "…and so do the verb's own params, unfiltered: the contract registry owns each " +
                    "verb's shape, and a check here would be a second, drifting copy of it",
            );
            assertEqual(
                JSON.stringify(result),
                JSON.stringify({ echoed: "editor.scene-tree" }),
                "the daemon's result is returned WHOLE, not re-projected",
            );
            // THE FORGERY LANDED NOWHERE. `daemonCall` takes (method, params) and nothing else, so the
            // recorded entry is the complete set of what the panel could influence.
            assertEqual(
                fx.daemonCalls[0]?.arity,
                2,
                "the forward carries ONLY a method and its params — EXACTLY two arguments, so the " +
                    "package is closed over and a packageId in the request reaches no code that " +
                    "reads one. Asserted as an ARITY the fixture measures with rest args, not as the " +
                    "key set of an object the fixture itself built: the latter is a property of the " +
                    "test and no production change could ever move it",
            );

            // A no-argument call is ordinary, not malformed: `params` is simply absent.
            await call(fx, PANEL_VERB_CALL, { method: "describe" });
            assertEqual(fx.daemonCalls.length, 2, "the second forward happened");
            assertEqual(fx.daemonCalls[1]?.method, "describe", "…carrying the bare method");
            assertEqual(
                fx.daemonCalls[1]?.params,
                undefined,
                "…and an absent `params` stays absent rather than being invented as {}",
            );
        },
    },
    {
        // ⚠ PLANT: drop the `daemonRefusalMessage` code suffix (return `detail` alone) — RED on the
        // `scope.denied` assertion below. That is the point of the case: `daemonRefusalCode` collapses
        // several causes onto three panel-facing codes, so WITHOUT the relayed original the fact this
        // whole task exists to establish — a panel meets `scope.denied` IN THE DISPATCHER — would be
        // unobservable from the panel, and a regression that stopped enforcing it would look identical.
        name: "panelverbs: a daemon scope.denied reaches the panel as capability_not_granted, code intact",
        run: async (): Promise<void> => {
            const denied = fixture({
                daemonOutcome: {
                    ok: false,
                    code: "scope.denied",
                    message: "the daemon refused 'set' for package 'ext.hello'",
                },
            });
            const refusal = await refusalFrom(denied, PANEL_VERB_CALL, { method: "set" });
            assertEqual(
                refusal.code,
                PANEL_BRIDGE_REFUSALS.capabilityNotGranted,
                "a scope refusal is CAPABILITY-shaped — 'the verb is real and YOUR PACKAGE was not " +
                    "granted it', the one thing install consent (e13c-4) can act on",
            );
            assert(
                refusal.message.includes("scope.denied"),
                "…and the DAEMON's own catalog code is relayed verbatim, or the DoD line 'un-granted " +
                    "file_write is rejected IN THE DISPATCHER' is unobservable from a panel",
            );
            // NOT a copy of `daemonRefusalCode`'s table — the mapping is driven directly so a case
            // cannot re-implement the thing it is checking.
            assertEqual(
                daemonRefusalCode("scope.insufficient"),
                PANEL_BRIDGE_REFUSALS.capabilityNotGranted,
                "its sibling maps the same way",
            );
            assertEqual(
                daemonRefusalCode("panel.daemon.bad_params"),
                PANEL_BRIDGE_REFUSALS.malformedRequest,
                "a REQUEST fault is not a grant question",
            );
            assertEqual(
                daemonRefusalCode("usage.invalid"),
                PANEL_BRIDGE_REFUSALS.malformedRequest,
                "…nor is the daemon's own usage family",
            );
            assertEqual(
                daemonRefusalCode("panel.daemon.method_not_allowed"),
                PANEL_BRIDGE_REFUSALS.verbNotGranted,
                "a non-allowlisted method is 'this build grants nothing for that'",
            );
            assertEqual(
                daemonRefusalCode("panel.daemon.capacity"),
                PANEL_BRIDGE_REFUSALS.verbNotGranted,
                "…as is a transient capacity refusal a panel cannot act on differently",
            );
            // NO NEW REFUSAL CODE WAS MINTED. The set is closed and adding to it is a
            // cross-task-visible decision (panelport.ts § PANEL_BRIDGE_REFUSALS); e13c-1 reuses the
            // three codes that already carry these meanings.
            const mapped = [
                "scope.denied",
                "panel.daemon.method_not_allowed",
                "panel.daemon.capacity",
                "panel.daemon.unavailable",
                "panel.daemon.bad_params",
                "bridge.transport",
                "",
            ].map((code) => daemonRefusalCode(code));
            const closed = Object.values(PANEL_BRIDGE_REFUSALS) as string[];
            assert(
                mapped.every((code) => closed.includes(code)),
                "every mapped refusal is already a member of the CLOSED set",
            );
        },
    },
    {
        name: "panelverbs: bridge.call refuses an unusable method WITHOUT forwarding it",
        run: async (): Promise<void> => {
            const fx = fixture({});
            for (const params of [
                undefined,
                {},
                { method: "" },
                { method: 42 },
                { method: "x".repeat(PANEL_DAEMON_METHOD_MAX_LENGTH + 1) },
            ]) {
                const refusal = await refusalFrom(fx, PANEL_VERB_CALL, params);
                assertEqual(
                    refusal.code,
                    PANEL_BRIDGE_REFUSALS.malformedRequest,
                    "the fault is in the request itself, so no new code is spent on it",
                );
            }
            assertEqual(
                fx.daemonCalls.length,
                0,
                "NOTHING was forwarded — an unbounded attacker-chosen method must not reach the " +
                    "Shell, which echoes it again in its own diagnostics",
            );
            // The bound is INCLUSIVE, pinned on both sides so `>` becoming `>=` cannot pass unnoticed.
            await call(fx, PANEL_VERB_CALL, {
                method: "x".repeat(PANEL_DAEMON_METHOD_MAX_LENGTH),
            });
            assertEqual(fx.daemonCalls.length, 1, "a method AT the bound is forwarded, not refused");
        },
    },
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
            //
            // ⚠ UPDATED BY M9 e13c-4. Until e13c-4 a GRANTED package met a SECOND refusal here
            // (`verb_not_granted`, "delivery is not wired in this build") and the measurement was that
            // the CODE moved. The granted half is wired now, so the measurement is stronger: the same
            // request that is REFUSED without the grant is ANSWERED with it, and the answer names the
            // accepted topics. A hardcoded denial cannot produce that, and neither can a build whose
            // gate is bypassed — the denying arm three lines up would then also answer.
            const granting = grantSpy(true);
            const grantedFx = fixture({
                grants: granting,
                declaredCapabilities: [CAPABILITY_UI_EVENTS],
            });
            const answered = await call(grantedFx, PANEL_VERB_UI_SUBSCRIBE, {
                topics: [UI_TOPIC_FOCUS],
            });
            assertEqual(
                granting.asked,
                [`${PACKAGE_ID}/${CAPABILITY_UI_EVENTS}`],
                "the same lookup ran, exactly once, about THIS package and THIS capability",
            );
            // THE POSITIVE ARTIFACT: the exact accepted topic list came back. An `!== undefined` or a
            // `topics.length >= 1` here would be satisfied by any object the handler happened to
            // return, including one produced by a path that never consulted the fan-out.
            assertEqual(
                answered,
                { topics: [UI_TOPIC_FOCUS] },
                "a GRANTED package's subscribe is ACCEPTED and echoes the topics it now holds",
            );
            // ...and it reached the DELIVERY SEAM, which no refusal assertion can express: this is the
            // half that would stay green if the handler returned a plausible object without ever
            // subscribing anything.
            assertEqual(
                grantedFx.uiSubscriptions.map((entry) => entry.topics),
                [[UI_TOPIC_FOCUS]],
                "the fan-out was asked to subscribe exactly those topics",
            );
        },
    },
    {
        // ⚠ THE NO-PACKAGE-ARGUMENT PROPERTY, the `bridge.call` fan-in note applied to this seam: the
        // package identity is a property of the CLOSURE `boot.ts` binds, so there is no member of the
        // request a panel could set to subscribe another package's panels. Proved by SENDING one and
        // observing the arity the table actually called with.
        name: "panelverbs: bridge.ui.subscribe takes no package argument (the closure IS the scope)",
        run: async (): Promise<void> => {
            const fx = fixture({
                grants: grantSpy(true),
                declaredCapabilities: [CAPABILITY_UI_EVENTS],
            });
            await call(fx, PANEL_VERB_UI_SUBSCRIBE, {
                topics: [UI_TOPIC_LAYOUT],
                packageId: "ext.victim",
            });
            assertEqual(fx.uiSubscriptions.length, 1, "the subscribe reached the fan-out");
            assertEqual(
                fx.uiSubscriptions[0]?.arity,
                1,
                "the seam was called with the TOPICS alone — a package id in the request cannot " +
                    "reach it, because there is no parameter for one",
            );
            assertEqual(
                fx.uiSubscriptions[0]?.topics,
                [UI_TOPIC_LAYOUT],
                "and the smuggled `packageId` is not among the topics either",
            );
        },
    },
    {
        // The request is UNTRUSTED input, so its parse is fail-closed — and the parse runs AFTER the
        // gate, deliberately: an un-granted package must not be able to tell a wired verb from an
        // unwired one by the shape of its own refusal.
        name: "panelverbs: bridge.ui.subscribe parses its topics fail-closed, and only AFTER the gate",
        run: async (): Promise<void> => {
            const granted = (): VerbFixture =>
                fixture({ grants: grantSpy(true), declaredCapabilities: [CAPABILITY_UI_EVENTS] });
            for (const bad of [
                undefined,
                {},
                { topics: "editor.ui.focus" },
                { topics: [7] },
                { topics: [""] },
                { topics: ["x".repeat(PANEL_COMMAND_FIELD_MAX_LENGTH + 1)] },
            ]) {
                const fx = granted();
                const refusal = await refusalFrom(fx, PANEL_VERB_UI_SUBSCRIBE, bad);
                assertEqual(
                    refusal.code,
                    PANEL_BRIDGE_REFUSALS.malformedRequest,
                    `a malformed topics param is a REQUEST fault, not a consent question: ${JSON.stringify(bad)}`,
                );
                assertEqual(
                    fx.uiSubscriptions.length,
                    0,
                    "…and nothing was subscribed on the way to that refusal",
                );
            }
            // THE POSITIVE COUNTERPART, same fixture family: a well-formed request IS accepted, so the
            // six refusals above are a parse decision rather than a verb that refuses everything.
            const ok = granted();
            assertEqual(
                await call(ok, PANEL_VERB_UI_SUBSCRIBE, { topics: [UI_TOPIC_FOCUS] }),
                { topics: [UI_TOPIC_FOCUS] },
                "a well-formed request is accepted, so the refusals above are a parse decision",
            );

            // GATE BEFORE PARSE: an UN-granted package sending the SAME malformed request meets the
            // CAPABILITY refusal, never the malformed one. If the parse ran first, an un-granted
            // package could probe whether delivery is wired by watching which code comes back.
            const ungranted = await refusalFrom(
                fixture({ declaredCapabilities: [CAPABILITY_UI_EVENTS] }),
                PANEL_VERB_UI_SUBSCRIBE,
                { topics: "not-an-array" },
            );
            assertEqual(
                ungranted.code,
                PANEL_BRIDGE_REFUSALS.capabilityNotGranted,
                "the gate answers first, so a refusal leaks nothing about the request's own shape",
            );
        },
    },
    {
        // The fan-out's own refusal (an unsubscribable topic) is relayed as a REQUEST fault, because
        // the package holds the grant — sending it to an install prompt it cannot act on would be the
        // wrong diagnosis, and `capability_not_granted` is exactly that prompt's code.
        name: "panelverbs: a granted package refused a TOPIC gets malformed_request, not the capability code",
        run: async (): Promise<void> => {
            const fx = fixture({
                grants: grantSpy(true),
                declaredCapabilities: [CAPABILITY_UI_EVENTS],
                uiOutcome: { topics: [], diagnostic: '"ext.hello.private" is not subscribable' },
            });
            const refusal = await refusalFrom(fx, PANEL_VERB_UI_SUBSCRIBE, {
                topics: ["ext.hello.private"],
            });
            assertEqual(
                refusal.code,
                PANEL_BRIDGE_REFUSALS.malformedRequest,
                "the package HOLDS the grant: the fault is in what it asked for, not in its consent",
            );
            assert(
                refusal.message.includes("ext.hello.private"),
                `the fan-out's diagnostic is relayed verbatim: ${refusal.message}`,
            );
        },
    },
    {
        name: "panelverbs: readUiTopicsParam is total and fail-closed",
        run: () => {
            assertEqual(readUiTopicsParam({ topics: ["a", "b"] }), ["a", "b"], "a plain list parses");
            // An EMPTY array parses (it is well-formed); refusing it is the fan-out's call, which is
            // where the "at least one topic" rule lives. Pinned so the two layers cannot both assume
            // the other rejects it.
            assertEqual(readUiTopicsParam({ topics: [] }), [], "an empty list is well-formed here");
            for (const bad of [undefined, null, 7, "topics", [], { topics: {} }, { topics: [null] }]) {
                assertEqual(
                    readUiTopicsParam(bad),
                    null,
                    `refused: ${JSON.stringify(bad) ?? "undefined"}`,
                );
            }
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

    // --------------------------------------- a manifest DECLARATION never confers ownership (03 fix)
    {
        // ⚠ PLANT: drop the `isReservedCommandId` filter in `makePanelBridgeVerbs` (build `owns()`
        // from `context.manifestCommandIds` again) and this case goes RED on the very first assertion
        // — `execute` runs the EDITOR's `session.undo`.
        name: "panelverbs: a manifest declaring a RESERVED id does not get to execute the editor's command",
        run: async (): Promise<void> => {
            // The package declares ids inside the editor's own namespaces. `projectPanelCommands`
            // projects them as authored, incumbent-wins refuses each one, and the ids keep pointing at
            // the EDITOR's commands — which is exactly what made reading the raw manifest list as
            // ownership an escalation.
            // These two ARE in the assembled registry, so each is a live escalation target rather
            // than a lookup miss dressed up as a refusal. (`workbench.palette.toggle` is deliberately
            // NOT here: `buildCommandRegistry` does not register it — boot.ts does, separately — so a
            // precondition asserting its presence would be false. It is covered below instead, which
            // is the stronger case anyway.)
            const escalations = ["session.undo", "view.window.tearOut"];
            const fx = fixture({
                manifestCommandIds: [...escalations, "workbench.palette.toggle", "hello.manifest"],
            });

            for (const id of escalations) {
                assert(
                    fx.registry.get(id) !== undefined,
                    `precondition: the editor's "${id}" is in the registry`,
                );
                const refusal = await refusalFrom(fx, PANEL_VERB_COMMANDS_EXECUTE, { id });
                assertEqual(
                    refusal.code,
                    PANEL_BRIDGE_REFUSALS.capabilityNotGranted,
                    `"${id}" is refused as an ESCALATION even though the manifest names it`,
                );
                assert(
                    !fx.invoked.some((entry) => entry.includes(id)),
                    `…and nothing was dispatched for "${id}"`,
                );
            }

            // THE FILTER IS BY NAMESPACE, NOT BY REGISTRY PRESENCE. `workbench.palette.toggle` is not
            // in this registry at all (boot.ts registers the palette's own command separately), and it
            // is refused all the same — so ownership is decided by the id's namespace, never by "did
            // the lookup happen to miss". A registry-presence rule would hand the id to whichever
            // package declared it before the editor got there.
            const paletteRefusal = await refusalFrom(fx, PANEL_VERB_COMMANDS_EXECUTE, {
                id: "workbench.palette.toggle",
            });
            assertEqual(
                paletteRefusal.code,
                PANEL_BRIDGE_REFUSALS.capabilityNotGranted,
                "a reserved id absent from the registry is refused as an escalation, not as unknown",
            );

            // The SAME manifest's ordinary id still works — the filter drops reserved ids only, and a
            // rule that broke legitimate manifest commands would be a regression, not a fix.
            const outcome = (await call(fx, PANEL_VERB_COMMANDS_EXECUTE, {
                id: "hello.manifest",
            })) as CommandOutcome;
            assert(outcome.ok, "an ordinary manifest command is unaffected by the ownership filter");

            // `list` is built from the same owned set, so it cannot leak the editor's commands either.
            const listed = (await call(fx, PANEL_VERB_COMMANDS_LIST)) as PanelCommandView[];
            for (const id of escalations) {
                assert(
                    !listed.some((view) => view.id === id),
                    `bridge.commands.list does not report the editor's "${id}" as this panel's`,
                );
            }
        },
    },

    // ------------------------------------------------ the table's teardown withdraws its work (03 fix)
    {
        // ⚠ PLANT: make `dispose()` a no-op and BOTH halves below go red — the id stays in the
        // registry, and the re-registration is refused by the orphan under incumbent-wins.
        name: "panelverbs: dispose() withdraws the panel's runtime commands, so a reopened panel can re-register",
        run: async (): Promise<void> => {
            const fx = fixture({});
            const id = `${PACKAGE_ID}.refresh`;
            await call(fx, PANEL_VERB_COMMANDS_REGISTER, {
                commands: [{ id, title: "Refresh", when: "" }],
            });
            assert(fx.registry.get(id) !== undefined, "precondition: the command registered");

            // What `IframePanelRenderer.dispose` does when the panel is closed.
            fx.dispose();
            assertEqual(
                fx.registry.get(id),
                undefined,
                "a closed panel leaves no ghost command in the registry (and none in the palette)",
            );

            // THE PERMANENT-BREAK HALF. A reopened panel builds a FRESH table with an empty
            // `registered` set; without the withdrawal above its re-registration would collide with
            // its own orphan and lose under incumbent-wins — costing it that command for the life of
            // the window, with no way to withdraw an id it no longer knows it holds.
            const reopened = fixture({ registry: fx.registry });
            const reply = (await call(reopened, PANEL_VERB_COMMANDS_REGISTER, {
                commands: [{ id, title: "Refresh", when: "" }],
            })) as { registered: string[]; rejected: PanelCommandRejection[] };
            assertEqual(
                JSON.stringify(reply.registered),
                JSON.stringify([id]),
                "the reopened panel re-registers the same id cleanly",
            );
            assertEqual(reply.rejected.length, 0, "…with nothing refused");
        },
    },

    // `dispose()` is idempotent and safe with no command layer — `PanelHost.dispose` can reach it
    // twice, and a window whose command layer never came up must not throw out of teardown.
    {
        name: "panelverbs: dispose() is idempotent and survives a missing command layer",
        run: async (): Promise<void> => {
            const fx = fixture({});
            await call(fx, PANEL_VERB_COMMANDS_REGISTER, {
                commands: [{ id: `${PACKAGE_ID}.a`, title: "A", when: "" }],
            });
            fx.dispose();
            fx.dispose();
            assertEqual(
                fx.registry.get(`${PACKAGE_ID}.a`),
                undefined,
                "a second dispose is a no-op, not a throw",
            );
            const headless = fixture({ withoutRegistry: true });
            headless.dispose();
        },
    },

    // ------------------------------------------------------------------- M9 e13d — theme + state

    {
        // ⚠ PLANT (a): make the handler return a captured snapshot instead of calling the provider
        //   (`const snapshot = context.themeTokens()` at table-build time) — RED, the second `get`
        //   still reports Dark. That is the whole failure mode "re-tokens on a theme switch" names:
        //   a panel opened under Dark would answer Light-era questions with Dark forever, and every
        //   assertion made at a single point in time would pass.
        name: "panelverbs: bridge.theme.tokens is LATE-BOUND, so a switch changes what it answers",
        run: async (): Promise<void> => {
            const fx = fixture({});
            const dark = (await call(fx, PANEL_VERB_THEME_TOKENS)) as ThemeChangedPayload;
            assertEqual(dark.themeId, "builtin.dark", "the current theme is answered");
            assertEqual(
                dark.variables["--ctx-colors-panel"],
                "#0a0a0a",
                "…with the FULL variable set a panel re-tokens itself from, not just an id",
            );

            fx.setTheme(themePayload("builtin.light"));
            const light = (await call(fx, PANEL_VERB_THEME_TOKENS)) as ThemeChangedPayload;
            assertEqual(light.themeId, "builtin.light", "after a switch the NEW theme is answered");
            assertEqual(
                light.variables["--ctx-colors-panel"],
                "#ffffff",
                "…and the variables moved with it",
            );
        },
    },
    {
        // The honest-refusal arm. A window whose theme engine never came up must NOT hand a panel an
        // empty token set: a panel that styles itself from `{}` renders invisible and blames its own
        // stylesheet, which is strictly worse than a refusal it can report.
        //
        // ⚠ PLANT (b): answer `{...empty payload}` instead of refusing — RED here.
        name: "panelverbs: bridge.theme.tokens REFUSES when no theme has been applied",
        run: async (): Promise<void> => {
            const fx = fixture({ withoutTheme: true });
            const refusal = await refusalFrom(fx, PANEL_VERB_THEME_TOKENS);
            assertEqual(
                refusal.code,
                PANEL_BRIDGE_REFUSALS.verbNotGranted,
                "an absent subsystem refuses like the absent command layer does — no new code spent",
            );
            assert(
                refusal.message.includes("theme"),
                "…and names the subsystem, so a package author is not left guessing",
            );
        },
    },
    {
        // ⚠ PLANT (c): drop `context.state.write(...)` from the set handler (keep the `{stored:true}`
        //   reply) — RED: the reply still claims success, and `get` answers `null`. That is the exact
        //   shape a "state round-trips" gate must not pass vacuously, because the SET call's own
        //   reply is the thing a naive test would assert on.
        // ⚠ PLANT (d): make `get` re-read the raw params instead of the store — RED for the same case.
        name: "panelverbs: bridge.state.set stores, and bridge.state.get reads BACK what was stored",
        run: async (): Promise<void> => {
            const fx = fixture({});
            assertEqual(
                await call(fx, PANEL_VERB_STATE_GET),
                { state: null },
                "a panel that never stored anything reads null, not undefined and not a throw",
            );

            const blob = { filter: "mesh", scroll: 42, open: ["a", "b"] };
            const stored = (await call(fx, PANEL_VERB_STATE_SET, {
                state: blob,
            })) as { stored: boolean; length: number };
            assertEqual(stored.stored, true, "the set is accepted");
            assertEqual(
                stored.length,
                JSON.stringify(blob).length,
                "…and reports the MEASURED serialized length — the number a package sizes its own " +
                    "state against, so any constant would do (a `> 0` assertion accepts `1`)",
            );
            assertEqual(fx.writes.length, 1, "the store really saw one write");

            assertEqual(
                await call(fx, PANEL_VERB_STATE_GET),
                { state: { filter: "mesh", scroll: 42, open: ["a", "b"] } },
                "and get answers with the stored blob, unchanged",
            );
        },
    },
    {
        // THE PROPERTY THAT MAKES A RELOAD HONEST. The value arrives by structured clone, which
        // carries shapes JSON does not; what is PERSISTED is JSON. If the store kept the cloned
        // value, `get` would answer with something a reload could never reproduce and the package
        // would meet the loss tomorrow instead of now.
        //
        // ⚠ PLANT (e): store `value` instead of `verdict.state` (skip the round trip) — RED: the Map
        //   comes back as a Map in-session and would be `{}` after a reload.
        name: "panelverbs: a stored blob is CANONICALIZED to what will persist, not to what was sent",
        run: async (): Promise<void> => {
            const fx = fixture({});
            await call(fx, PANEL_VERB_STATE_SET, {
                state: { when: new Date(0), picked: new Map([["k", "v"]]), keep: 1 },
            });
            const read = (await call(fx, PANEL_VERB_STATE_GET)) as { state: unknown };
            assertEqual(
                JSON.stringify(read.state),
                JSON.stringify({ when: "1970-01-01T00:00:00.000Z", picked: {}, keep: 1 }),
                "the in-session answer is already the post-reload answer — a Date is its ISO string " +
                    "and a Map is the empty object JSON makes of it",
            );
            // ⚠ THE TWO ASSERTIONS BELOW ARE THE DISCRIMINATING ONES, and the one above is not.
            // `JSON.stringify` erases exactly the difference PLANT (e) introduces — a Date and a Map
            // serialize to the ISO string and `{}` whether or not they were round-tripped first — so
            // the comparison above documents the SHAPE while these pin the TYPES.
            assert(
                typeof (read.state as Record<string, unknown>)["when"] === "string",
                "the Date really is a string in memory, not a Date that merely serializes like one",
            );
            assert(
                !(((read.state as Record<string, unknown>)["picked"]) instanceof Map),
                "…so nothing a reload cannot reproduce survives in memory to mislead the package",
            );
        },
    },
    {
        // ⚠ PLANT (f): raise/remove the bound in `sanitizePanelState` — RED (the oversize row is
        //   accepted). ⚠ PLANT (g): truncate instead of refusing — RED, because the case asserts the
        //   store was NOT written; a truncated blob is not a smaller blob, it is a corrupt one.
        name: "panelverbs: bridge.state.set REFUSES an oversize or unserializable blob, storing nothing",
        run: async (): Promise<void> => {
            const fx = fixture({});
            await call(fx, PANEL_VERB_STATE_SET, { state: { keep: "me" } });

            const oversize = await refusalFrom(fx, PANEL_VERB_STATE_SET, {
                state: { blob: "x".repeat(PANEL_STATE_MAX_JSON_LENGTH + 1) },
            });
            assertEqual(
                oversize.code,
                PANEL_BRIDGE_REFUSALS.malformedRequest,
                "an over-budget blob is refused as a malformed REQUEST, not as a host fault",
            );
            assert(
                oversize.message.includes(String(PANEL_STATE_MAX_JSON_LENGTH)),
                "…naming the bound, so a package can size its state against it",
            );

            // A cycle is the reachable non-serializable shape: structured clone supports one.
            const cyclic: Record<string, unknown> = { name: "loop" };
            cyclic["self"] = cyclic;
            const cycle = await refusalFrom(fx, PANEL_VERB_STATE_SET, { state: cyclic });
            assertEqual(
                cycle.code,
                PANEL_BRIDGE_REFUSALS.malformedRequest,
                "so is a cyclic blob — JSON cannot express it, so it could never round-trip",
            );

            assertEqual(
                await call(fx, PANEL_VERB_STATE_GET),
                { state: { keep: "me" } },
                "and NEITHER refusal disturbed the blob already stored",
            );
            assertEqual(fx.writes.length, 1, "the store saw exactly the one accepted write");
        },
    },
    {
        // THE CROSS-PACKAGE PROPERTY THIS FILE'S HEADER RESTS ON, asserted rather than argued: state
        // is private to one panel, and there is NO ARGUMENT by which one package could name another's
        // blob. Both halves are pinned because they fail differently — the first would be a leak
        // through a shared store, the second a leak through an addressable one.
        //
        // ⚠ PLANT: give `bridge.state.get|set` a `panelId` parameter and resolve the store through a
        //   shared map — RED on the second half (the foreign read starts answering).
        name: "panelverbs: one panel's state is UNREACHABLE from another panel's table",
        run: async (): Promise<void> => {
            const mine = fixture({});
            const theirs = fixture({});
            await call(mine, PANEL_VERB_STATE_SET, { state: { secret: "mine" } });

            assertEqual(
                await call(theirs, PANEL_VERB_STATE_GET),
                { state: null },
                "a second panel's table closes over its OWN store, so it sees nothing of the first's",
            );
            assertEqual(
                await call(theirs, PANEL_VERB_STATE_GET, { panelId: PANEL_ID, packageId: PACKAGE_ID }),
                { state: null },
                "…and NAMING the first panel changes nothing: the verb takes no id, so the params " +
                    "are inert — there is no lookup here that could be tricked into resolving one",
            );
            await call(theirs, PANEL_VERB_STATE_SET, {
                state: { secret: "theirs" },
                panelId: PANEL_ID,
            });
            assertEqual(
                await call(mine, PANEL_VERB_STATE_GET),
                { state: { secret: "mine" } },
                "…nor can a WRITE addressed at the first panel reach it — the isolation is not an " +
                    "id check that could be bypassed, it is the absence of an id to check",
            );
            assertEqual(mine.writes.length, 1, "the first panel's store saw only its own write");
        },
    },
    {
        // `set` with no `state` member is how a panel CLEARS its state. Worth pinning because the
        // permissive reading — "no state member ⇒ ignore the call" — is one line away and would
        // leave a package no way to forget anything.
        name: "panelverbs: bridge.state.set with no state member CLEARS the blob",
        run: async (): Promise<void> => {
            const fx = fixture({});
            await call(fx, PANEL_VERB_STATE_SET, { state: { a: 1 } });
            await call(fx, PANEL_VERB_STATE_SET, {});
            assertEqual(
                await call(fx, PANEL_VERB_STATE_GET),
                { state: null },
                "an empty set clears rather than being ignored",
            );
            assertEqual(fx.storedState(), null, "…observed on the store itself, not only in the reply");
        },
    },
    {
        // The sanitizer drives directly too — it is the ONE gate BOTH the verb and the persisted-bytes
        // path (panelhost.ts `seedState`) go through, so its verdicts are worth pinning without a
        // table around them.
        name: "panelverbs: sanitizePanelState is total over every shape a blob can arrive as",
        run: (): void => {
            assertEqual(sanitizePanelState(undefined).diagnostic, "", "undefined is a clear, not a fault");
            assertEqual(sanitizePanelState(undefined).state, null, "…normalized to null");
            assertEqual(sanitizePanelState(null).diagnostic, "", "null is storable");
            assertEqual(sanitizePanelState(7).state, 7, "a bare number is a legal JSON document");
            assertEqual(sanitizePanelState("x").state, "x", "so is a bare string");
            assertEqual(
                JSON.stringify(sanitizePanelState([1, { a: 2 }]).state),
                JSON.stringify([1, { a: 2 }]),
                "an array survives intact",
            );
            const fn = sanitizePanelState(() => 1);
            assert(fn.diagnostic !== "", "a function is refused (JSON.stringify answers undefined)");
            assertEqual(fn.state, null, "…and carries no partial value");
            const big = sanitizePanelState({ b: "y".repeat(PANEL_STATE_MAX_JSON_LENGTH) });
            assert(big.diagnostic !== "", "an oversize document is refused");
            assert(
                big.length > PANEL_STATE_MAX_JSON_LENGTH,
                "…and reports the length it measured, not the bound",
            );

            // THE EDGE ITSELF, both sides. Every oversize row above overshoots by ~10 characters or
            // more, so all of them survive `>` becoming `>=` — i.e. nothing pinned WHICH side of the
            // bound is inclusive, and a package sizing its state to exactly the documented limit
            // would have started being refused with no test noticing.
            // `JSON.stringify({b: "y".repeat(n)})` is `{"b":"…"}`, so its length is n + 8.
            const exact = sanitizePanelState({ b: "y".repeat(PANEL_STATE_MAX_JSON_LENGTH - 8) });
            assertEqual(
                exact.length,
                PANEL_STATE_MAX_JSON_LENGTH,
                "the row is built to serialize to EXACTLY the bound",
            );
            assertEqual(exact.diagnostic, "", "…and a blob AT the bound is ACCEPTED, not refused");
            const overByOne = sanitizePanelState({ b: "y".repeat(PANEL_STATE_MAX_JSON_LENGTH - 7) });
            assertEqual(
                overByOne.length,
                PANEL_STATE_MAX_JSON_LENGTH + 1,
                "…while one character more serializes to one over",
            );
            assert(overByOne.diagnostic !== "", "…and THAT is refused — the bound is inclusive");
        },
    },
];
