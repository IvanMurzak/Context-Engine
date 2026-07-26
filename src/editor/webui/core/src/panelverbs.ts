// The PANEL BRIDGE API VERBS — editor-core's LOCAL half (M9 e13b-2, design 04 §5 / 05 §5-§6).
//
// WHERE THIS SITS. `panelport.ts` is the TRANSPORT: one authenticated `MessagePort` per iframe panel,
// a versioned envelope over it, and a verb TABLE whose lookup MISS is the deny-all refusal. It ships
// zero capability by construction. THIS module is the first thing to put entries in that table — the
// verbs editor-core can answer WITHOUT reaching the daemon:
//
//   * `bridge.commands.*` — a package panel's OWN commands, over the ONE registry (05 §6) the palette
//     and the keymap already read. Nothing new is built here; the delegation IS the design.
//   * `bridge.ui.subscribe` — ships HARD-DENIED, at the ONE named enforcement point below.
//
//   * `bridge.theme.tokens` — the current design tokens, PULLED over the port (M9 e13d). Its PUSH
//     half is `IframeThemeChannel` (theme.ts), wired to real frames by `panelhost.ts`.
//   * `bridge.state.get` / `.set` — THIS panel's own opaque blob, the half that makes a reload
//     preserve panel state (M9 e13d).
//
// WHAT IS DELIBERATELY ABSENT. `bridge.call` / `bridge.events.subscribe` are e13c's (they need a
// per-package scoped DAEMON session, which is the capability model itself). Both are still missing
// from this table, so both still get the e13b-1 deny-all answer — the property `panelport.test.ts`
// pins for the verbs that remain parked.
//
// ⚠ THE CROSS-PACKAGE PROPERTY, FOR e13d's TWO FAMILIES — stated because they are the first verbs
// that move DATA rather than refusals, and because `event.origin` is `"null"` for every package so no
// origin string can be used to establish it (panelport.ts § the file header).
//
//   * THEME is public and IDENTICAL for every panel: one applied theme, one variable set, no
//     per-package content. So a package learning another's theme tokens learns nothing it could not
//     read off its own document, which is why the payload may also ride `IframeThemeChannel`'s
//     UNAUTHENTICATED `targetOrigin:"*"` broadcast (each post still targets ONE `contentWindow`, so
//     it is a fan-out of one public value, not a shared channel).
//
//     ⚠ THAT ARGUMENT COVERS READING, AND ONLY READING — STATED HERE BECAUSE IT DOES NOT COVER
//     WRITING. A `window.postMessage` broadcast is unauthenticated in BOTH directions, and a
//     sandboxed package reaches a SIBLING package's `WindowProxy` through the cross-origin surface
//     (`window.parent.length` / `window.parent[i]` are readable cross-origin, and `postMessage` is
//     callable on the result). So one package CAN post a fabricated `editor.ui.theme-changed` into
//     another's frame. What that buys is bounded — the payload is presentation tokens, so the worst
//     outcome is a panel drawn in the wrong colours; it conveys NO capability, and it is NOT a path
//     to another package's state (which is why STATE is on the port, next bullet). It is still a
//     WRITE the editor does not authenticate, and the honest conclusion is that the PORT is what
//     establishes the cross-package property here and the broadcast is not: a package that must
//     trust what it is handed should take the tokens from `bridge.theme.tokens` over its own port,
//     or check `event.source === window.parent` on the pushed event — the editor's own document has
//     a REAL origin, so a forgery from an opaque-origin sibling is distinguishable, but only by a
//     panel that looks. Recorded rather than assumed away, because "the port alone is sufficient" is
//     exactly the claim this bullet cannot make for the push half.
//   * STATE is a package's PRIVATE data, so it never touches that broadcast. It travels ONLY over the
//     authenticated per-panel port, and — the structural half — `bridge.state.get|set` take NO panel
//     or package id: each panel's table closes over ITS OWN store (`PanelVerbContext.state`, supplied
//     per renderer by panelhost.ts), so there is no argument by which one package could name
//     another's blob and no lookup that could be tricked into returning it. The verbs are not an
//     oracle for which panels exist either, for the same reason.
//
// ⚠ THE `ui_events` GATE IS THE POINT OF THIS TASK, AND ITS SHAPE IS LOAD-BEARING. C-F18's threat is
// "a sandboxed panel observes editor telemetry / builds a cross-package channel via `editor.ui`", and
// the control 08 names is the `ui_events` GRANT. The vocabulary already exists on both sides
// (`kCapabilityUiEvents`, gui/contract/extension.h; `capabilities` on the parsed manifest, panels.ts)
// and NOTHING enforces it, so shipping the verb open would ship the threat. It therefore ships DENIED
// — and the denial comes from a real GRANT LOOKUP (`capabilityDenial`), never from a hardcoded refusal
// and never from simply leaving the verb out:
//
//   * OMITTING it answers `verb_not_granted` by lookup MISS, which is indistinguishable from "not
//     implemented yet", is provable by no plant, and leaves e13c nothing to fill.
//   * A HARDCODED refusal passes every negative test vacuously — panelport.ts says exactly this about
//     the table itself, and the reasoning does not stop being true one layer up.
//
// So a GRANT SOURCE is consulted, and today's source is `DENY_ALL_CAPABILITY_GRANTS`. e13c replaces
// THAT ONE OBJECT with install-consent grants; the DENIED half of the parent DoD clause stays true for
// every package that was not granted `ui_events`. The T1 tier proves the lookup is real by planting a
// GRANTING source and watching the refusal CODE change.
//
// ⚠ A MANIFEST DECLARATION IS NOT A GRANT. `PanelManifest.capabilities` is what a package ASKED for
// (04 §3). Reading it as the gate would let any package grant itself `ui_events` by editing its own
// manifest — the threat wearing the control's clothes. It is carried here for DIAGNOSTICS only, so a
// refusal can distinguish "you declared it and were not granted it" from "you never asked", and it is
// never consulted as the decision.
//
// ⚠ TWO DIRECTIONS, TWO NAMESPACES. `bridge.*` verbs travel PANEL -> HOST and are what this table
// answers. `commands.invoke` travels HOST -> PANEL over `PanelPortBridge.request` (e13b-1 built that
// direction and named e13b-2 as its first production consumer) and is answered by the PACKAGE. The
// missing `bridge.` prefix on it is the whole distinction, so a package author reading one envelope
// can tell which side is being asked.

import type { Command, CommandOutcome, CommandRegistry } from "./commands.js";
import { PANEL_BRIDGE_REFUSALS, PanelVerbRefusal } from "./panelport.js";
import type { PanelBridgeReply, PanelVerbHandler } from "./panelport.js";
// TYPE-ONLY, so this module still imports no theme CODE: `bridge.theme.tokens` hands back whatever
// the injected provider gives it, and the provider is `boot.ts`'s (`IframeThemeChannel.last`). The
// direction is the same one this file already has with `commands.ts` — the shape travels, the
// subsystem does not.
import type { ThemeChangedPayload } from "./theme.js";
import { evaluateWhen } from "./when.js";
import type { WhenContext } from "./when.js";

// ------------------------------------------------------------------------------- the vocabulary

/**
 * The `editor.ui` read grant. MIRRORS C++ `kCapabilityUiEvents`
 * (`src/editor/gui/contract/include/context/editor/gui/contract/extension.h`), which is the closed
 * manifest capability vocabulary's own definition of the token — spelled underscored there and here,
 * because the manifest spelling and the bridge's hyphenated wire scope names are deliberately
 * distinct (registry.cpp owns the one translation between them).
 */
export const CAPABILITY_UI_EVENTS = "ui_events";

/** Read this panel's own commands, with their `when` clauses resolved against the live context. */
export const PANEL_VERB_COMMANDS_LIST = "bridge.commands.list";
/** Register commands at RUNTIME, namespaced under this package (04 §5 `bridge.commands.register`). */
export const PANEL_VERB_COMMANDS_REGISTER = "bridge.commands.register";
/** Withdraw commands this panel registered at runtime. Never a manifest one, never another package's. */
export const PANEL_VERB_COMMANDS_UNREGISTER = "bridge.commands.unregister";
/** Execute one of THIS panel's own commands through the one registry (04 §5 `bridge.commands.execute`). */
export const PANEL_VERB_COMMANDS_EXECUTE = "bridge.commands.execute";
/** Subscribe to `editor.ui` facts — requires the `ui_events` grant (04 §5, C-F18). DENIED here. */
export const PANEL_VERB_UI_SUBSCRIBE = "bridge.ui.subscribe";
/** Read the CURRENT design tokens (04 §5 `bridge.theme.tokens`; 06 §1). The PULL half — M9 e13d. */
export const PANEL_VERB_THEME_TOKENS = "bridge.theme.tokens";
/** Read back this panel's own state blob (04 §5 `bridge.state.get`, "its own blob") — M9 e13d. */
export const PANEL_VERB_STATE_GET = "bridge.state.get";
/** Store this panel's own state blob (04 §5 `bridge.state.set`) — M9 e13d. */
export const PANEL_VERB_STATE_SET = "bridge.state.set";

/**
 * The HOST -> PANEL verb: "run this command of yours".
 *
 * NO `bridge.` PREFIX, deliberately — see the file header's two-directions note. This is what the
 * editor sends DOWN the port when a panel command fires in the palette or off a keybinding, and it is
 * the reason a package panel's command is reachable from the editor's own command surface at all: an
 * iframe panel has no C++ model, so the `panel.command` route every `uitree` panel takes answers
 * `dispatched:false` for it (a latent dead end e13b-2 is the first task able to fix).
 */
export const PANEL_VERB_COMMAND_INVOKE = "commands.invoke";

/**
 * The command-id namespaces a package may NEVER register into.
 *
 * These are the editor's OWN sources — `contract.` (projected contract verbs), `view.` (dock / window
 * / theme), `session.` (undo / redo), `workbench.` (the palette), and `editor.` reserved for the same
 * reason uibus.ts reserves the `editor` package id. Incumbent-wins registration already stops a
 * package from SHADOWING one that exists; this additionally stops it from SQUATTING an id the editor
 * has not shipped yet, which incumbent-wins alone cannot do (the package would simply be first).
 */
export const RESERVED_COMMAND_PREFIXES: readonly string[] = [
    "contract.",
    "view.",
    "session.",
    "workbench.",
    "editor.",
];

/**
 * The longest id / title / when-clause this side accepts from a panel.
 *
 * BOUNDED because an untrusted peer chooses all three and each is stored, echoed in diagnostics, and
 * rendered in the palette — the same amplification `PANEL_BRIDGE_TOKEN_MAX_LENGTH` refuses one layer
 * down.
 */
export const PANEL_COMMAND_FIELD_MAX_LENGTH = 128;

/**
 * How many commands ONE panel may hold registered at a time.
 *
 * Without a cap, `bridge.commands.register` is an unbounded allocation reachable from untrusted code
 * in a process that stays up for days — and every entry is also a palette row, so the cost is not only
 * memory. 64 is far above any plausible panel and far below anything that matters.
 */
export const PANEL_COMMAND_REGISTRATION_LIMIT = 64;

// ---------------------------------------------------------------------------- the capability gate

/**
 * The capability GRANT source — what a package was actually granted, as opposed to what its manifest
 * declared.
 *
 * A one-method interface with no transport in it, exactly like `UiMirrorSink` (uibus.ts): the
 * implementation that knows about install consent and the daemon dispatcher is e13c's, and this module
 * must stay ignorant of it. Replacing `DENY_ALL_CAPABILITY_GRANTS` at the one seam that consumes it is
 * the whole of e13c's editor-core-side change.
 */
export interface PanelCapabilityGrants {
    /** Has `packageId` been granted `capability`? */
    granted(packageId: string, capability: string): boolean;
}

/**
 * THE deny-all grant source — this build's only one.
 *
 * Frozen so it cannot be mutated into a grant by anything holding a reference to it. e13c replaces the
 * VALUE passed to `makePanelBridgeVerbs`; it does not weaken this object.
 */
export const DENY_ALL_CAPABILITY_GRANTS: PanelCapabilityGrants = Object.freeze({
    granted: (): boolean => false,
});

/**
 * THE `ui_events` ENFORCEMENT POINT'S PREDICATE (C-F18). `""` when granted; the diagnostic otherwise.
 *
 * Pure and total, so the T1 tier drives it directly rather than only through a port round trip. The
 * two distinct diagnostics matter: a package that DECLARED the capability and was refused is an
 * install-consent question, while one that never declared it is a manifest bug, and a single message
 * would make a package author debug the wrong thing.
 */
export function capabilityDenial(
    grants: PanelCapabilityGrants,
    packageId: string,
    capability: string,
    declared: readonly string[],
): string {
    if (grants.granted(packageId, capability)) {
        return "";
    }
    return declared.includes(capability)
        ? `the package "${packageId}" declared the "${capability}" capability but has not been ` +
              "granted it (install consent is not wired in this build)"
        : `the package "${packageId}" holds no "${capability}" grant and its manifest does not ` +
              "declare one";
}

/**
 * THE ONE NAMED ENFORCEMENT POINT the parent DoD clause "`ui_events` gates `bridge.ui.subscribe`"
 * lands on. Returns normally when granted; throws the panel-facing refusal otherwise.
 *
 * e13c FILLS THIS POINT — by passing a real grant source, not by editing this function. Everything
 * about the shape of the refusal (its code, its wording, the fact that it is a reply rather than a
 * dropped message) is settled here so the granted half is purely additive.
 */
export function requireCapability(
    grants: PanelCapabilityGrants,
    packageId: string,
    capability: string,
    declared: readonly string[],
): void {
    const denial = capabilityDenial(grants, packageId, capability, declared);
    if (denial !== "") {
        throw new PanelVerbRefusal(PANEL_BRIDGE_REFUSALS.capabilityNotGranted, denial);
    }
}

// ------------------------------------------------------------------------ the command-id grammar

/**
 * May `packageId` register `commandId` at RUNTIME? `""` when yes, the diagnostic otherwise.
 *
 * NAMESPACED UNDER THE PACKAGE, exactly as `validatePackageTopic` (uibus.ts) constrains a package's
 * bus topics, and against the same threat: with incumbent-wins registration an unnamespaced runtime id
 * is a SQUATTING vector — package A loads first, takes `b.open`, and package B's own command is
 * refused for the life of the window. The prefix makes that structurally impossible rather than a
 * race, and it also makes a palette row attributable to a package by reading its id.
 *
 * ⚠ THE MANIFEST PATH IS DELIBERATELY NOT RETRO-CONSTRAINED **FOR REGISTRATION**.
 * `projectPanelCommands` (commands.ts) projects a panel's manifest `commands` as authored; those ids
 * are reviewed at install and validated C++-side (`registry.cpp` refuses an id declared twice within
 * one contribution). Tightening them here would silently drop commands from manifests that already
 * ship, and the collision they can still produce is exactly what the non-fatal registration rule now
 * handles — one command refused, attributably, instead of the whole palette.
 *
 * ⚠ BUT REGISTERING AN ID AND OWNING ONE ARE DIFFERENT QUESTIONS, and only the first is relaxed.
 * `makePanelBridgeVerbs` drops reserved-namespace ids from the set a panel OWNS
 * (`isReservedCommandId`), because ownership is what `execute` checks. See the filter's own comment
 * for the escalation that closes.
 */
/**
 * Is `commandId` inside one of the editor's OWN namespaces (`RESERVED_COMMAND_PREFIXES`)?
 *
 * Shared by the two places that must agree on the answer: the runtime grammar below, which refuses
 * to REGISTER such an id, and the manifest-ownership filter in `makePanelBridgeVerbs`, which refuses
 * to let one confer OWNERSHIP. Written once so the two can never drift into different notions of
 * "the editor's own namespace" — a drift that would reopen the escalation the filter exists to close.
 */
export function isReservedCommandId(commandId: string): boolean {
    return RESERVED_COMMAND_PREFIXES.some((prefix) => commandId.startsWith(prefix));
}

export function validatePackageCommandId(packageId: string, commandId: string): string {
    if (commandId.length === 0 || commandId.length > PANEL_COMMAND_FIELD_MAX_LENGTH) {
        return `"${commandId}" is not a usable command id (1..${String(
            PANEL_COMMAND_FIELD_MAX_LENGTH,
        )} characters)`;
    }
    if (isReservedCommandId(commandId)) {
        // Re-derived only for the DIAGNOSTIC; the decision above is the shared one, so the two paths
        // cannot answer differently about what "reserved" means.
        const prefix = RESERVED_COMMAND_PREFIXES.find((entry) => commandId.startsWith(entry)) ?? "";
        return `"${commandId}" is inside the reserved editor namespace "${prefix}"`;
    }
    if (packageId.length === 0) {
        return `"${commandId}" cannot be namespaced: this panel has no package id`;
    }
    if (!commandId.startsWith(`${packageId}.`) || commandId.length <= packageId.length + 1) {
        return `"${commandId}" is not namespaced under its own package "${packageId}"`;
    }
    return "";
}

// ------------------------------------------------------------------------- the panel state blob

/**
 * The longest JSON text ONE panel's state blob may serialize to (M9 e13d).
 *
 * BOUNDED FOR A SHARPER REASON THAN THE FIELDS ABOVE. A registered command id is echoed; a state blob
 * is WRITTEN TO DISK — `LayoutPersistence` folds it into `.editor/editor-state.json`, which the Shell
 * is the single writer of and rewrites on every layout change. Unbounded, `bridge.state.set` is a
 * disk-fill primitive reachable from sandboxed third-party code at whatever rate it likes, in a
 * process designed to stay up for days. 64 KiB is far above any plausible panel's UI state (a
 * selection, a scroll offset, a filter string) and far below anything that matters on disk.
 *
 * Measured in UTF-16 code units of the SERIALIZED text, not bytes: the value that matters is the one
 * that leaves this process, and counting the string the serializer produced needs no `TextEncoder`
 * and therefore no environment branch this tier could not reach.
 */
export const PANEL_STATE_MAX_JSON_LENGTH = 64 * 1024;

/**
 * ONE panel's state blob, as the verb table reads and writes it.
 *
 * A two-method store with no persistence in it, exactly like `PanelCapabilityGrants` above: WHERE the
 * blob ends up (an in-memory field on the renderer, folded into the editor-state document by
 * `LayoutPersistence`) is panelhost.ts's and editorstate.ts's business, and this module must stay
 * ignorant of it. What matters here is that the store is supplied PER PANEL — see the file header on
 * why that, and not a lookup by id, is the cross-package boundary.
 */
export interface PanelStateStore {
    /** The blob this panel last stored, or what a persisted blob seeded. `null` when it has none. */
    read(): unknown;
    /**
     * Store an ALREADY-SANITIZED blob — a `PanelStateVerdict.state`, never a raw panel value.
     *
     * The bound and the JSON round trip are `sanitizePanelState`'s and are applied by the CALLER
     * (the `bridge.state.set` handler below, and panelhost.ts's `seedState` for persisted bytes).
     * Said here rather than enforced here because sanitizing again inside the store would re-serialize
     * every write to re-derive a verdict the caller already holds; the cost of that choice is that a
     * host supplying its OWN `panelVerbs` factory (`PanelHostOptions`) owns this obligation too.
     */
    write(value: unknown): void;
}

/** The verdict of `sanitizePanelState`. `diagnostic` is `""` exactly when the value is storable. */
export interface PanelStateVerdict {
    /** The value AS IT WILL PERSIST — a JSON round trip of the input. `null` when refused. */
    readonly state: unknown;
    /** The serialized length that was measured against `PANEL_STATE_MAX_JSON_LENGTH`. */
    readonly length: number;
    readonly diagnostic: string;
}

/**
 * Bound and CANONICALIZE one panel-supplied state blob. Pure and total, so the T1 tier drives it
 * directly rather than only through a port round trip.
 *
 * ⚠ THE JSON ROUND TRIP IS THE POINT, not a formality. The value arrives by STRUCTURED CLONE, which
 * carries shapes JSON does not: a cycle (JSON throws), a `Map` or `Set` (JSON silently yields `{}`),
 * a `Date` (JSON yields a string). What is persisted is JSON, so a store that kept the cloned value
 * would answer `bridge.state.get` with something a RELOAD could never reproduce — the panel would see
 * its `Map` survive all session and find it empty tomorrow, which is the worst possible place to
 * discover the conversion. Storing the round-tripped value instead makes the in-session answer and
 * the post-reload answer THE SAME VALUE, so a package meets any loss immediately.
 *
 * `undefined` is normalized to `null` and accepted: a `set` carrying no value is how a panel CLEARS
 * its state, and refusing it would leave no way to do so. A function or a symbol is refused rather
 * than ignored — NOT because either production caller can produce one (structured clone cannot carry
 * them over the port, and the persisted path arrives as JSON, which cannot either), but because this
 * function is EXPORTED and takes `unknown`: the T1 tier drives it directly, and a total function over
 * `unknown` is cheaper to guarantee than a partial one is to argue safe every time a caller is added.
 */
export function sanitizePanelState(value: unknown): PanelStateVerdict {
    // ONE `try` OVER THE WHOLE ROUND TRIP, so this function's TOTALITY is STRUCTURAL rather than
    // argued. BOTH halves can throw: `JSON.stringify` on a cycle or a throwing `toJSON`, and
    // `JSON.parse` on a structure too deep to recurse back into. Wrapping only the first would leave
    // the claim above resting on "anything stringify produced can always be re-parsed", which is not
    // something the platform promises — and this is the gate a FILE re-enters through (panelhost.ts
    // `seedState`), whose caller loop is documented to degrade, never abort.
    try {
        const text = JSON.stringify(value === undefined ? null : value) as string | undefined;
        if (text === undefined) {
            // `JSON.stringify` answers `undefined` — not a string — for a function or a symbol.
            return {
                state: null,
                length: 0,
                diagnostic: "the state value is not JSON-serializable (a function or a symbol)",
            };
        }
        if (text.length > PANEL_STATE_MAX_JSON_LENGTH) {
            // REFUSED, never truncated: half a blob is not a smaller blob, it is a corrupt one, and a
            // panel told "stored" for something it could not read back is worse than a refusal it can
            // act on. The measured length is reported so a package can size its state against the
            // bound. Checked BEFORE the parse, so an over-budget blob is never re-materialized.
            return {
                state: null,
                length: text.length,
                diagnostic:
                    `the state blob serializes to ${String(text.length)} characters, over the ` +
                    `${String(PANEL_STATE_MAX_JSON_LENGTH)}-character limit`,
            };
        }
        return { state: JSON.parse(text), length: text.length, diagnostic: "" };
    } catch {
        // The thrown error is deliberately NOT echoed: it is produced by the host's serializer and is
        // therefore host state, exactly as `PanelPortBridge.#invoke` treats an ordinary throw.
        return {
            state: null,
            length: 0,
            diagnostic:
                "the state value did not survive a JSON round trip (a cycle, a value whose toJSON " +
                "threw, or a structure too deeply nested to re-parse)",
        };
    }
}

// -------------------------------------------------------------------------------- the verb table

/** What one panel's verb table is built over. Everything it may touch, and nothing else. */
export interface PanelVerbContext {
    /** The rostered panel id this port belongs to. */
    readonly panelId: string;
    /** The `context-ext://<packageId>` authority that owns the panel (extpanel.ts parsed it). */
    readonly packageId: string;
    /** The manifest's `capabilities` — a DECLARATION, never a grant. Diagnostics only. */
    readonly declaredCapabilities: readonly string[];
    /** The ids this panel's MANIFEST declared (`projectPanelCommands` already registered them). */
    readonly manifestCommandIds: readonly string[];
    /**
     * The ONE command registry — LATE-BOUND, because it is built after `PanelHost.start()` has
     * already created this panel's renderer and port. `undefined` means the command layer is not up
     * (or failed), which is an honest refusal rather than a reason to hold a stale registry.
     */
    readonly registry: () => CommandRegistry | undefined;
    /** The live when-context the palette filters on — the same provider, so the two cannot disagree. */
    readonly whenContext: () => WhenContext;
    /** The capability GRANT source. `DENY_ALL_CAPABILITY_GRANTS` until e13c. */
    readonly grants: PanelCapabilityGrants;
    /**
     * The CURRENT theme payload (M9 e13d) — LATE-BOUND for the same reason `registry` is: the theme
     * can change under a mounted panel, and a value captured when the table was built would answer
     * `bridge.theme.tokens` with the theme that was active when the panel OPENED. `undefined` means
     * no theme has been applied in this window, which is an honest refusal rather than a reason to
     * invent an empty token set (a panel handed `{}` would style itself invisible and blame itself).
     *
     * `boot.ts` binds it to `IframeThemeChannel.last` — the SAME field the push replays from, so the
     * pull and the push cannot disagree (theme.ts § `last`).
     */
    readonly themeTokens: () => ThemeChangedPayload | undefined;
    /**
     * THIS PANEL's state store (M9 e13d) — supplied per renderer by `panelhost.ts`.
     *
     * A store, not a lookup: see the file header. `bridge.state.get|set` take no id precisely because
     * this object IS the scope, so no request can name another package's blob.
     */
    readonly state: PanelStateStore;
    /** Issue a request DOWN this panel's port (the `commands.invoke` direction). */
    readonly request: (verb: string, params?: unknown) => Promise<PanelBridgeReply>;
}

/**
 * One panel's verb table, plus the teardown that withdraws what it registered.
 *
 * `dispose` is not optional bookkeeping: the registry outlives every panel, so without it a closed
 * panel's runtime commands stay in the palette and the keymap, dispatching over a dead port — and,
 * worse, the panel can never re-register them, because a reopened panel gets a FRESH table (with an
 * empty `registered` set) whose registrations then collide with its own orphans and lose under
 * incumbent-wins. `CommandRegistry.unregister` exists for exactly this caller.
 */
export interface PanelVerbTable {
    readonly verbs: ReadonlyMap<string, PanelVerbHandler>;
    /** Withdraw every command this table registered. Idempotent. */
    readonly dispose: () => void;
}

/** One command as `bridge.commands.list` reports it back to its own panel. */
export interface PanelCommandView {
    readonly id: string;
    readonly title: string;
    readonly when: string;
    /** Is it invocable RIGHT NOW — i.e. does its `when` hold against the live context? */
    readonly active: boolean;
}

/**
 * Build the verb table for ONE third-party panel's port.
 *
 * EVERY VERB IS SCOPED TO THIS PANEL. `list`, `execute` and `unregister` all resolve against the ids
 * this panel owns (its manifest's plus the ones it registered), so a package can neither enumerate the
 * editor's whole command surface nor drive a command belonging to the editor or to another package.
 * That scoping is the capability boundary at this layer: e13b-2 grants a panel authority over ITSELF
 * and nothing more, which is what keeps it an "editor-core-LOCAL" slice with e13c still owning every
 * grant that reaches the daemon.
 *
 * NEVER THROWS except through `PanelVerbRefusal` (the sanctioned panel-facing refusal). An ordinary
 * throw would reach `PanelPortBridge.#invoke`'s generic host-fault path and tell the panel nothing.
 */
export function makePanelBridgeVerbs(context: PanelVerbContext): PanelVerbTable {
    // The ids this panel registered at RUNTIME, in registration order. Held here rather than derived
    // from the registry because the registry cannot say WHO registered a command — and a scoping rule
    // that cannot name its subject is not a scoping rule.
    const registered = new Set<string>();

    // ⚠ A MANIFEST DECLARATION IS NOT A GRANT — the rule this file's header states for
    // `capabilities`, applied to commands, where it was missing.
    //
    // `projectPanelCommands` projects manifest ids AS AUTHORED (see `validatePackageCommandId`), so a
    // package may DECLARE anything — including `session.undo` or `view.window.tearOut`. Incumbent-wins
    // then refuses that registration and the id keeps pointing at the EDITOR's command. That is
    // precisely what made reading the raw manifest list as ownership an escalation: `owns()` said yes
    // (the package declared it), `registry.get()` handed back the editor's entry, and `execute` ran
    // the editor's command — replaying an undo, tearing out a window — from a sandboxed panel, with
    // no capability anywhere in the path.
    //
    // Ownership is therefore filtered to ids OUTSIDE the editor's own namespaces. The commands stay
    // projected and their palette rows are unaffected; a package simply never becomes the owner of an
    // id the editor reserves. The runtime path already refuses these at registration, so this makes
    // the two paths agree on the one question `execute` asks.
    const manifestOwned: readonly string[] = context.manifestCommandIds.filter(
        (id) => !isReservedCommandId(id),
    );

    const ownIds = (): readonly string[] => [...manifestOwned, ...registered];

    const owns = (id: string): boolean => registered.has(id) || manifestOwned.includes(id);

    /** The registry, or a panel-facing refusal when the command layer never came up. */
    const requireRegistry = (): CommandRegistry => {
        const registry = context.registry();
        if (registry === undefined) {
            throw new PanelVerbRefusal(
                PANEL_BRIDGE_REFUSALS.verbNotGranted,
                "the editor command layer is not available in this window",
            );
        }
        return registry;
    };

    /** Ask the PANEL to run one of its own commands, over its port. */
    const invokeInPanel = async (commandId: string): Promise<CommandOutcome> => {
        const reply = await context.request(PANEL_VERB_COMMAND_INVOKE, { id: commandId });
        return reply.ok
            ? { ok: true, note: `${context.panelId}/${commandId}` }
            : {
                  ok: false,
                  note: `${context.panelId}/${commandId} not dispatched (${
                      reply.error?.code ?? "no reply"
                  })`,
              };
    };

    /** Register ONE panel-supplied command. `null` on success; the refusal otherwise. */
    const registerOne = (
        registry: CommandRegistry,
        spec: CommandSpec,
    ): PanelCommandRejection | null => {
        const grammar = validatePackageCommandId(context.packageId, spec.id);
        if (grammar !== "") {
            return { id: spec.id, diagnostic: grammar };
        }
        if (registered.size >= PANEL_COMMAND_REGISTRATION_LIMIT) {
            return {
                id: spec.id,
                diagnostic: `this panel already holds the maximum of ${String(
                    PANEL_COMMAND_REGISTRATION_LIMIT,
                )} registered commands`,
            };
        }
        const command: Command = {
            id: spec.id,
            title: spec.title,
            category: "panel",
            when: spec.when,
            docs: {
                summary: `${spec.title} — from the ${context.panelId} panel`,
                detail:
                    `package command; package=${context.packageId}; panel=${context.panelId}; ` +
                    `registered over ${PANEL_VERB_COMMANDS_REGISTER}` +
                    (spec.when === "" ? "" : `; when: ${spec.when}`),
            },
            // Dispatch goes back DOWN this panel's port — see PANEL_VERB_COMMAND_INVOKE.
            handler: (): Promise<CommandOutcome> => invokeInPanel(spec.id),
        };
        // NON-FATAL by construction (commands.ts § tryRegister): a collision costs this one command,
        // and the incumbent — an editor command, or another package that got there first — keeps the
        // id.
        const collision = registry.tryRegister(command);
        if (collision !== null) {
            return { id: spec.id, diagnostic: collision.diagnostic };
        }
        registered.add(spec.id);
        return null;
    };

    /**
     * Withdraw everything this panel registered — the renderer's teardown calls it.
     *
     * Uses the LATE-BOUND registry like every verb: a window whose command layer never came up has
     * nothing to withdraw, which is a no-op rather than a reason to throw out of `dispose()`.
     */
    const dispose = (): void => {
        const registry = context.registry();
        for (const id of registered) {
            registry?.unregister(id);
        }
        registered.clear();
    };

    const verbs = new Map<string, PanelVerbHandler>([
        [
            PANEL_VERB_COMMANDS_LIST,
            (): PanelCommandView[] => {
                const registry = requireRegistry();
                const resolved = context.whenContext();
                const views: PanelCommandView[] = [];
                for (const id of ownIds()) {
                    const command = registry.get(id);
                    // An id that is NOT in the registry is omitted rather than reported inactive: a
                    // manifest command refused for colliding with a built-in genuinely does not exist,
                    // and listing it would promise the panel an `execute` that must then fail.
                    if (command !== undefined) {
                        views.push({
                            id: command.id,
                            title: command.title,
                            when: command.when,
                            active: command.when === "" || evaluateWhen(command.when, resolved),
                        });
                    }
                }
                return views;
            },
        ],
        [
            PANEL_VERB_COMMANDS_REGISTER,
            (params: unknown): { registered: string[]; rejected: PanelCommandRejection[] } => {
                const registry = requireRegistry();
                const accepted: string[] = [];
                const rejected: PanelCommandRejection[] = [];
                for (const spec of readCommandSpecs(params)) {
                    const rejection = registerOne(registry, spec);
                    if (rejection === null) {
                        accepted.push(spec.id);
                    } else {
                        rejected.push(rejection);
                    }
                }
                return { registered: accepted, rejected };
            },
        ],
        [
            PANEL_VERB_COMMANDS_UNREGISTER,
            (params: unknown): { unregistered: string[]; refused: PanelCommandRejection[] } => {
                const registry = requireRegistry();
                const unregistered: string[] = [];
                const refused: PanelCommandRejection[] = [];
                for (const id of readIdList(params)) {
                    if (!registered.has(id)) {
                        // Deliberately NOT "unknown": a manifest id and another package's id are both
                        // real commands this panel simply may not withdraw, and saying so is what
                        // stops a package probing the registry for what exists.
                        refused.push({
                            id,
                            diagnostic: `"${id}" was not registered by this panel over ${PANEL_VERB_COMMANDS_REGISTER}`,
                        });
                        continue;
                    }
                    registry.unregister(id);
                    registered.delete(id);
                    unregistered.push(id);
                }
                return { unregistered, refused };
            },
        ],
        [
            PANEL_VERB_COMMANDS_EXECUTE,
            async (params: unknown): Promise<CommandOutcome> => {
                const registry = requireRegistry();
                const id = readId(params);
                if (!owns(id)) {
                    // THE ESCALATION REFUSAL. A package may drive ITS OWN commands through the one
                    // registry; `view.window.tearOut` and every projected contract verb are not its
                    // to run, and the refusal is deliberately identical whether the id exists or not
                    // so this is not an oracle for the editor's command surface.
                    throw new PanelVerbRefusal(
                        PANEL_BRIDGE_REFUSALS.capabilityNotGranted,
                        `"${id}" is not a command of the ${context.panelId} panel`,
                    );
                }
                const command = registry.get(id);
                if (command === undefined) {
                    // Owned but absent — its registration was refused (a collision). An ordinary
                    // outcome, exactly as `CommandRegistry.execute`'s own unknown-id answer is.
                    return { ok: false, note: `unknown command: ${id}` };
                }
                if (command.when !== "" && !evaluateWhen(command.when, context.whenContext())) {
                    // THE `when`-EVAL DELEGATION (05 §6). A panel executing its own command still gets
                    // the applicability guard the palette and the keymap apply, so one command cannot
                    // have two different notions of when it is invocable.
                    return { ok: false, note: `${id} is not applicable in the current context` };
                }
                return await registry.execute(id);
            },
        ],
        [
            PANEL_VERB_UI_SUBSCRIBE,
            (): never => {
                // ⚠ THE ONE `ui_events` ENFORCEMENT POINT (C-F18) — see the file header. Everything
                // this verb will ever do sits BEHIND this call, so e13c cannot wire delivery without
                // passing through it.
                requireCapability(
                    context.grants,
                    context.packageId,
                    CAPABILITY_UI_EVENTS,
                    context.declaredCapabilities,
                );
                // THE GRANTED HALF IS e13c's. Reaching here means a grant source said yes — which no
                // build does today. It must still not deliver `editor.ui` facts, because the
                // subscription plumbing (and the cross-frame envelope it would ride) does not exist:
                // answering anything else would be inventing a capability out of a grant. The refusal
                // is therefore the ordinary "this build has no such verb", and the CODE is what proves
                // to a T1 plant that the grant lookup above really ran.
                throw new PanelVerbRefusal(
                    PANEL_BRIDGE_REFUSALS.verbNotGranted,
                    "`ui_events` is granted, but editor.ui delivery to package panels is not wired " +
                        "in this build",
                );
            },
        ],
        [
            PANEL_VERB_THEME_TOKENS,
            (): ThemeChangedPayload => {
                const tokens = context.themeTokens();
                if (tokens === undefined) {
                    // The same shape `requireRegistry` uses for an absent subsystem, and the same
                    // code: `verb_not_granted` means "this build cannot answer that", which is
                    // exactly true of a window whose theme engine never came up. Inventing a code
                    // here would spend a cross-task-visible decision (panelport.ts § the refusal set)
                    // on a state no shipped window reaches — `boot.ts` applies a theme before it
                    // starts panels at all.
                    throw new PanelVerbRefusal(
                        PANEL_BRIDGE_REFUSALS.verbNotGranted,
                        "no theme has been applied in this window",
                    );
                }
                // RETURNED WHOLE, and deliberately the same object the PUSH carries as its
                // `payload`: a package writes ONE applier and feeds it from the pull at startup and
                // from every pushed `editor.ui.theme-changed` afterwards. Nothing is filtered out —
                // every member is public presentation data (06 §1), and a subset here would be a
                // second token vocabulary drifting against the pushed one.
                return tokens;
            },
        ],
        [
            PANEL_VERB_STATE_GET,
            (): { state: unknown } => ({
                // NO PARAMETERS, so there is nothing to validate and nothing to scope: this table
                // belongs to one panel and closes over one store. Wrapped in an object rather than
                // returned bare so `null` state and a future sibling field are both expressible —
                // the reply's own `result` is already the unwrapping layer.
                state: context.state.read(),
            }),
        ],
        [
            PANEL_VERB_STATE_SET,
            (params: unknown): { stored: boolean; length: number } => {
                const verdict = sanitizePanelState(readStateParam(params));
                if (verdict.diagnostic !== "") {
                    // A REFUSAL the panel can act on (shrink the blob, drop the cycle), so it is a
                    // `PanelVerbRefusal` and not an ordinary throw — the latter would reach the
                    // generic host-fault path and tell the package nothing (panelport.ts
                    // § `PanelVerbRefusal`). `malformed_request` because the fault is in the request
                    // itself; no new code is spent on it.
                    throw new PanelVerbRefusal(
                        PANEL_BRIDGE_REFUSALS.malformedRequest,
                        verdict.diagnostic,
                    );
                }
                context.state.write(verdict.state);
                // The measured length is reported so a package can watch its own headroom against
                // `PANEL_STATE_MAX_JSON_LENGTH` instead of discovering the bound by hitting it.
                return { stored: true, length: verdict.length };
            },
        ],
    ]);

    return { verbs, dispose };
}

/** One refused registration / withdrawal, as reported back to the panel. */
export interface PanelCommandRejection {
    readonly id: string;
    readonly diagnostic: string;
}

// ------------------------------------------------------------------------------------- internals
//
// TOTAL PARSERS over attacker-chosen params, in the discipline panels.ts establishes: a member that is
// not the shape we need is DROPPED, never coerced. A coerced value here becomes a palette row, so
// "best effort" reading is how a panel ends up with a command called `[object Object]`.

interface CommandSpec {
    readonly id: string;
    readonly title: string;
    readonly when: string;
}

function isRecordValue(value: unknown): value is Record<string, unknown> {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}

/** A bounded, non-empty string, or `undefined`. The one shape every panel-supplied field must take. */
function boundedString(value: unknown): string | undefined {
    return typeof value === "string" &&
        value.length > 0 &&
        value.length <= PANEL_COMMAND_FIELD_MAX_LENGTH
        ? value
        : undefined;
}

/**
 * Read `{ commands: [{id, title?, when?}] }`.
 *
 * An entry with no usable `id` is dropped SILENTLY rather than reported: there is nothing to name it
 * by in the reply, and inventing a placeholder id would put an attacker-chosen shape into a
 * diagnostic. Entries are capped at the registration limit so a single call cannot make this loop
 * unbounded work.
 */
function readCommandSpecs(params: unknown): readonly CommandSpec[] {
    if (!isRecordValue(params) || !Array.isArray(params["commands"])) {
        return [];
    }
    const specs: CommandSpec[] = [];
    for (const entry of params["commands"]) {
        if (specs.length >= PANEL_COMMAND_REGISTRATION_LIMIT) {
            break;
        }
        if (!isRecordValue(entry)) {
            continue;
        }
        const id = boundedString(entry["id"]);
        if (id === undefined) {
            continue;
        }
        specs.push({
            id,
            title: boundedString(entry["title"]) ?? id,
            // An unusable `when` reads as `""` = ALWAYS APPLICABLE, which is the permissive default —
            // safe only because a `when` clause is an applicability filter, never an authorization:
            // what a package command may DO is decided by `owns()` and by its handler, not by this.
            when: boundedString(entry["when"]) ?? "",
        });
    }
    return specs;
}

/** Read `{ ids: [...] }`, bounded and deduplicated by the caller's own `registered` set. */
function readIdList(params: unknown): readonly string[] {
    if (!isRecordValue(params) || !Array.isArray(params["ids"])) {
        return [];
    }
    const ids: string[] = [];
    for (const entry of params["ids"]) {
        if (ids.length >= PANEL_COMMAND_REGISTRATION_LIMIT) {
            break;
        }
        const id = boundedString(entry);
        if (id !== undefined) {
            ids.push(id);
        }
    }
    return ids;
}

/** Read `{ id }`. `""` when absent — which `owns("")` refuses, so there is no unguarded path. */
function readId(params: unknown): string {
    return isRecordValue(params) ? (boundedString(params["id"]) ?? "") : "";
}

/**
 * Read `{ state }` off a `bridge.state.set` request (M9 e13d).
 *
 * DELIBERATELY UNTYPED AND UNBOUNDED HERE — the blob is OPAQUE to the editor by design ("its own
 * blob", 04 §5), so there is no shape to validate against. Every constraint that does apply (JSON
 * serializability, the length bound) is `sanitizePanelState`'s, in one place, shared with the
 * persisted-bytes path. A request that is not an object, or carries no `state` member, reads as
 * `undefined` and the sanitizer normalizes that to `null` — which is how a panel CLEARS its state.
 */
function readStateParam(params: unknown): unknown {
    return isRecordValue(params) ? params["state"] : undefined;
}
