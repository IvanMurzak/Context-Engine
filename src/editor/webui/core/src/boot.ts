// editor-core's boot sequence (M9 e05c handshake, extended by e05d1 to bring up the app).
//
// e05c's job was the CHANNEL: proving — inside the real e04 Shell window, over the real
// `context-editor://` scheme, under the real CSP — that the bundle loaded and that the bridge
// round-trips native<->JS. e05d1 adds THE APP on top of that channel: once the handshake completes,
// PanelHost creates the docking root and every Shell-hostable panel mounts and hydrates.
//
// The `editor-cef-smoke-shell` ctest asserts the native side of the handshake, which is what makes
// it an end-to-end proof rather than two halves that were each tested alone:
//
//   1. JS  -> native   `shell.hello`  — "the bundle executed and can reach the Shell"
//   2. native -> JS    the reply       — carries a nonce only the Shell knows
//   3. JS  -> native   `shell.ready`  — echoes that nonce back
//
// Step 3 is the load-bearing one: the native side only accepts a `shell.ready` whose nonce matches
// the one it just minted, so the sequence cannot pass unless a value made the FULL round trip
// through the renderer. A one-way "JS called us" ping would pass with a broken response path.
//
// e05d1 extends that proof one link further: the same smoke now also asserts the Shell's PanelHost
// SERVED `panel.list` and `panel.render`, which can only be true if the sequence below actually ran
// in the live renderer. That is the end-to-end evidence for "Problems hydrates from the live daemon
// via the bridge" — a claim no local test can make, since the local gate cannot link CEF at all.
//
// ORDERING IS LOAD-BEARING: panels are brought up AFTER `shell.ready`, never before. The handshake
// is what proves the response path works; mounting panels first would fire a burst of `panel.*`
// calls down a channel nothing has yet shown to be bidirectional, turning a clean "the bridge is
// broken" diagnosis into a pile of unexplained panel failures.

import { BridgeError, ShellBridge, isRecord } from "./bridge.js";
import {
    ConfigClient,
    EMPTY_CONFIG_SNAPSHOT,
    configuredThemeId,
    startupThemeId,
    type UserConfigSnapshot,
} from "./config.js";
import {
    buildCommandRegistry,
    type CommandOutcome,
    type CommandRegistry,
    type EditorCommandActions,
} from "./commands.js";
import { EditorStateClient, LayoutPersistence } from "./editorstate.js";
import { editorCoreInfo } from "./info.js";
import { Keymap, KeybindingsClient } from "./keymap.js";
import { Palette, PALETTE_TOGGLE_COMMAND_ID, paletteCommands } from "./palette.js";
import { PaletteView } from "./palette_view.js";
import { PanelClient } from "./panels.js";
import { PanelHost, type LocalPanelFactory } from "./panelhost.js";
import {
    SETTINGS_PANEL_ID,
    mountSettings,
    type SettingsPanelMount,
    type ThemeChoice,
} from "./settings.js";
import {
    REDUCED_MOTION_QUERY,
    ThemeController,
    ThemeEngine,
    ThemesClient,
    defaultMediaQueryProbe,
    defaultThemeId,
    parsePinnedThemeId,
    type ThemeRoot,
} from "./theme.js";
import { WELCOME_MODE_WELCOME, WelcomeClient, mountWelcome } from "./welcome.js";
import {
    resolveContext,
    STUB_EDITOR_UI,
    STUB_SESSION_STATE,
    type WhenContext,
} from "./when.js";

/** The element the docking root mounts into. Mirrors `app/index.html`'s `<main id="editor-root">`. */
export const EDITOR_ROOT_ID = "editor-root";

/** What `bootEditorCore` did — returned so a caller (and a test) can assert on it. */
export interface BootReport {
    /** False when the bundle is running outside the Shell (no injected query function). */
    readonly attached: boolean;
    /** True once the full JS -> native -> JS -> native handshake completed. */
    readonly ready: boolean;
    /** How many panels PanelHost mounted. 0 when the app layer did not come up. */
    readonly panelsMounted: number;
    /**
     * Rostered panels this build cannot host (`hosted: false`) — today Scene tree, Inspector and the
     * six panels with no provider yet. REPORTED rather than silently skipped: "a panel is missing"
     * must be an observable fact. e05d3 shrinks this list.
     */
    readonly panelsUnavailable: readonly string[];
    /** Populated when the handshake or the app bring-up failed; empty otherwise. */
    readonly error: string;
}

/** Marks the document so the boot state is inspectable from DevTools and from a DOM scan. */
function markDocument(state: string, detail: string): void {
    if (typeof document === "undefined") {
        return;
    }
    const root = document.documentElement;
    root.setAttribute("data-editor-core", state);
    if (detail !== "") {
        root.setAttribute("data-editor-core-detail", detail);
    }
}

/**
 * Run the boot handshake.
 *
 * NEVER throws and never rejects: a failure here must leave a diagnosable document rather than an
 * unhandled rejection in a renderer nobody is watching.
 */
export async function bootEditorCore(bridge = ShellBridge.detect()): Promise<BootReport> {
    if (bridge === undefined) {
        // Loaded outside the Shell — a plain browser or a harness. Honest, not fatal.
        markDocument("detached", "no shell bridge on this global");
        return { attached: false, ready: false, panelsMounted: 0, panelsUnavailable: [], error: "" };
    }

    // Marked BEFORE the first await. `index.html` ships the literal `data-editor-core="loading"`, so
    // without this a bundle that never executed and a handshake that hung present the SAME document
    // state — in a renderer whose only diagnostic channel is this attribute. "booting" means the
    // module ran and the bridge was found.
    markDocument("booting", "");

    try {
        const info = editorCoreInfo();
        const hello = await bridge.call("shell.hello", {
            protocolMajor: info.protocolMajor,
            clientSchemaVersion: info.clientSchemaVersion,
            rpcMethodCount: info.rpcMethodCount,
        });

        // The Shell's reply carries a nonce. Echoing it back is what proves the response path, so a
        // missing one is a hard failure rather than something to paper over with a default.
        const nonce = isRecord(hello) ? hello["nonce"] : undefined;
        if (typeof nonce !== "string" || nonce === "") {
            markDocument("error", "shell.hello returned no nonce");
            return {
                attached: true,
                ready: false,
                panelsMounted: 0,
                panelsUnavailable: [],
                error: "shell.hello returned no nonce",
            };
        }

        await bridge.call("shell.ready", { nonce });

        // --- the theme engine (e06b, design 06) ---------------------------------------------------
        // BEFORE the welcome screen and before the panels, deliberately: the tokens are what every
        // surface below is drawn with, so applying them first means the first painted frame is
        // already themed. Bringing panels up first would show one unthemed frame of docking chrome —
        // exactly the flash the 350ms cross-fade exists to avoid — and would make the live smoke's
        // per-pixel background assertion race the theme apply.
        //
        // The BUILT-IN half needs no Shell round trip (the themes ship inside the bundle), so it is
        // synchronous and cannot fail. The WATCHED user themes need one `themes.get`, which a Shell
        // that does not serve it refuses instantly — so loading them here too, rather than after the
        // panels, costs nothing and means a user's own theme is on screen for the first frame as
        // well. Both halves are best-effort: neither can keep the editor from booting.
        // The PERSISTED choice is read FIRST, because it decides which theme the first painted frame
        // carries (06 §4 / C-F22). Best-effort like every other feed: a Shell that does not serve
        // `config.get` yields the empty snapshot and the editor falls back to `prefers-color-scheme`,
        // which is exactly the first-run behaviour.
        const config = await loadUserConfig(bridge);
        const theme = startTheme(config);
        if (theme !== undefined) {
            await startThemeFeed(bridge, theme);
        }

        // --- the welcome screen (e14c, design 07 §4 / D13) ----------------------------------------
        // A BARE launch shows the app's front door (recent projects / "Open project…" / "New from
        // template") instead of the editor. Ask the Shell, and DEFAULT to the editor path when there
        // is no welcome surface: `state()` returns null on an `unknown_method` refusal, which is
        // exactly what the CEF boot smokes (which install no welcome surface) get — so they mount
        // panels unchanged. Only an explicit `mode: "welcome"` diverts to the front door.
        const welcomeState = await new WelcomeClient(bridge).state();
        if (welcomeState !== null && welcomeState.mode === WELCOME_MODE_WELCOME) {
            const container =
                typeof document === "undefined" ? null : document.getElementById(EDITOR_ROOT_ID);
            let recentCount = 0;
            let templateCount = 0;
            if (container !== null) {
                const mount = mountWelcome(bridge, container, welcomeState);
                recentCount = mount.recentCount;
                templateCount = mount.templateCount;
            }
            markDocument("welcome", `recents:${recentCount} templates:${templateCount}`);
            return {
                attached: true,
                ready: true,
                panelsMounted: 0,
                panelsUnavailable: [],
                error: "",
            };
        }

        // --- the app layer (e05d1) ----------------------------------------------------------------
        // The channel is proven; bring up the panels. A failure HERE is reported but does NOT undo
        // `ready`: the bridge genuinely does round-trip, and conflating "the editor has no panels"
        // with "the editor cannot talk to the Shell" would send the next diagnosis in exactly the
        // wrong direction.
        const panels = await startPanels(bridge, theme, config);

        // --- the keymap override feed (e07c) ------------------------------------------------------
        // Load the per-user `~/.context/keybindings.json` override the Shell watches and serves
        // (keybindings_bridge.h). This is the "the Shell publishes the keymap to editor-core" channel:
        // the Shell owns the file (editor-core is a pure wire-client and cannot read it), editor-core
        // schema-validates + merges it here. Best-effort like persistence — a Shell that does not serve
        // the method (an older build) leaves the default keymap standing, and a malformed override is
        // rejected with a diagnostic, so this can NEVER keep the editor from booting. The live input
        // pump consumes the resolved keymap when 03 §6 keyboard routing wires it (a later seam); at boot
        // the job is to LOAD + validate the override and prove the channel end to end (the
        // `editor-cef-smoke-shell` leg asserts `keybindings.get` was served).
        await startKeybindings(bridge);

        markDocument("ready", panels.error);
        return {
            attached: true,
            ready: true,
            panelsMounted: panels.mounted,
            panelsUnavailable: panels.unavailable,
            error: panels.error,
        };
    } catch (error) {
        const detail =
            error instanceof BridgeError
                ? `${error.reason}: ${error.message}`
                : error instanceof Error
                  ? error.message
                  : String(error);
        markDocument("error", detail);
        return {
            attached: true,
            ready: false,
            panelsMounted: 0,
            panelsUnavailable: [],
            error: detail,
        };
    }
}

/** What the app-layer bring-up produced. Internal to `bootEditorCore`'s two return paths. */
interface PanelBringUp {
    readonly mounted: number;
    readonly unavailable: readonly string[];
    readonly error: string;
}

/**
 * Create the PanelHost and open every hostable panel.
 *
 * Separated from `bootEditorCore` so the handshake stays legible as the three-leg exchange it is,
 * and so a panel failure has one obvious place to be handled rather than being tangled into the
 * nonce logic. Like its caller it NEVER throws.
 */
async function startPanels(
    bridge: ShellBridge,
    theme: ThemeEngine | undefined,
    config: UserConfigSnapshot,
): Promise<PanelBringUp> {
    if (typeof document === "undefined") {
        return { mounted: 0, unavailable: [], error: "no document to mount into" };
    }
    const container = document.getElementById(EDITOR_ROOT_ID);
    if (container === null) {
        // The served document owns this element; its absence means the HTML and the bundle have
        // drifted apart, which is worth naming precisely rather than failing on a null deref.
        return { mounted: 0, unavailable: [], error: `no #${EDITOR_ROOT_ID} element in the document` };
    }
    try {
        const client = new PanelClient(bridge);
        // The Settings panel is editor-core's own content (e06d): the roster declares it
        // `content.type: "local"` and THIS is the build that knows how to draw it. Nothing else about
        // PanelHost changes — an unregistered local panel is reported unavailable like any other.
        const settings = makeSettingsPanel(bridge, theme, config);
        const host = new PanelHost({ container, client, localPanels: settings.factories });
        const report = await host.start();

        // --- layout persistence + region maps (e05d2) --------------------------------------------
        // The panels are up; now make the ARRANGEMENT durable. Restore the persisted layout + per-
        // panel D6 state OVER the defaults `start` just opened (a fresh project restores nothing and
        // the defaults stand), then start persisting future changes and publishing region maps on
        // every layout change. NEVER fatal: a persistence failure — a stale blob, a full disk — must
        // leave a working editor, so `ready` does NOT depend on it. `restore` and `attach` are called
        // only once the docking root is up (`report.started`); the LayoutPersistence stays reachable
        // through the Dockview + `pagehide` listeners `attach` registers, the same way `host` stays
        // reachable through the mounted DOM.
        if (report.started) {
            try {
                const stateClient = new EditorStateClient(bridge);
                const persistence = new LayoutPersistence({
                    panelHost: host,
                    panelClient: client,
                    stateClient,
                });
                const restoreReport = await persistence.restore();
                // Report the restore OUTCOME to the Shell (e05d4). The restart smoke asserts this is
                // `layoutRestored:false` on a fresh boot and `true` on a boot that reapplied a
                // persisted arrangement — the end-to-end proof that the arrangement round-tripped
                // through the Shell's editor-state store. Best-effort: `reportRestore` swallows a
                // Shell refusal, so it can never keep `attach` from running below.
                await stateClient.reportRestore(restoreReport);
                persistence.attach();
                // --- the command layer + palette (e07d) ----------------------------------------
                // The docking root is up and persistence is live; wire the ONE command registry, the
                // palette over it, and (only under `?ctx-smoke-palette`) drive the T2 command-driven
                // scenario. Placed AFTER `persistence.attach()` so a palette-driven layout change
                // publishes over the live editor.state channel — the observable the T2 smoke asserts.
                startCommandLayer(host, client, theme);
                // e06d settings-smoke seam: only under `?ctx-smoke-settings`, drive a REAL theme
                // switch through the Settings panel so the live leg can assert the Shell persisted it.
                runSettingsSmoke(settings.mount());
                // e05d4 restart-smoke seam: only when the boot URL carries `?ctx-smoke-arrange`,
                // perform ONE deterministic docking change so the arrangement that gets persisted
                // differs from the fresh-boot default — which is what makes the restart proof
                // meaningful. A no-op with no flag, so nothing in the shipping editor is affected.
                applySmokeArrangement(host);
            } catch {
                // Reported by absence of persistence, never fatal — the editor is up and usable.
            }
        }

        return { mounted: report.mounted, unavailable: report.unavailable, error: report.error };
    } catch (error) {
        return {
            mounted: 0,
            unavailable: [],
            error: error instanceof Error ? error.message : String(error),
        };
    }
}

/**
 * Load the per-user keybindings override at boot (e07c) — best-effort, never fatal.
 *
 * Fetches the Shell's `keybindings.get` snapshot and applies it to a fresh keymap: an absent file
 * leaves the defaults, a valid override merges over them, and a malformed one is rejected with a
 * diagnostic (the defaults stand). The outcome is written onto `<html data-editor-keybindings>` so the
 * `--dump-dom` local repro and DevTools can read it — the same diagnosability discipline `markDocument`
 * gives the boot state. NEVER throws: a bridge/parse failure degrades to "defaults", it does not fail
 * boot.
 */
async function startKeybindings(bridge: ShellBridge): Promise<void> {
    let detail = "defaults";
    try {
        const keymap = new Keymap();
        const snapshot = await new KeybindingsClient(bridge).get();
        const apply = keymap.applyUserOverride(snapshot.present ? snapshot.text : null);
        detail = !snapshot.present
            ? "no override; defaults"
            : apply.applied
              ? `override applied (${apply.userBindingCount} bindings, gen ${snapshot.generation})`
              : `override REJECTED: ${apply.diagnostic}`;
    } catch (error) {
        detail = `keybindings feed unavailable: ${error instanceof Error ? error.message : String(error)}`;
    }
    if (typeof document !== "undefined") {
        document.documentElement.setAttribute("data-editor-keybindings", detail);
    }
}

/**
 * Bring up the theme engine and apply the first-run theme (e06b, design 06 §1-§4).
 *
 * Returns `undefined` only when there is no document to theme (a harness, a documentless host) —
 * everything else is best-effort and NEVER throws: an editor that cannot theme itself must still
 * boot, and the honest signal for that is the `data-editor-theme` attribute reading `unavailable`
 * rather than an unhandled rejection in a renderer nobody watches.
 *
 * The first-run choice follows `prefers-color-scheme`, Dark when undetectable (06 §4 / C-F22).
 * PERSISTING an explicit choice is e06d's (`~/.context/config.json`, single writer: the Shell), so
 * today the choice lives for the session — stated plainly rather than half-implemented here.
 *
 * The live `prefers-reduced-motion` listener is registered when the environment supports it, so a
 * user turning the OS setting on mid-session gets the static fallback WITHOUT a restart — the same
 * "unconditionally honoured" rule applied over time, not just at boot.
 */
function startTheme(config: UserConfigSnapshot): ThemeEngine | undefined {
    if (typeof document === "undefined") {
        return undefined;
    }
    try {
        const probe = defaultMediaQueryProbe();
        // `document.documentElement` satisfies ThemeRoot structurally; naming the interface here is
        // what keeps the engine testable against a recording root instead of a live DOM.
        const root: ThemeRoot = document.documentElement;
        const engine = new ThemeEngine({ root, probe });
        // The `?ctx-smoke-theme=<id>` pin when the boot URL carries one, else the
        // `prefers-color-scheme` default. The pin is what makes the live CEF smokes' per-pixel
        // background assertion independent of whether the HOST prefers dark — see THEME_PIN_FLAG.
        const search = typeof location === "undefined" ? "" : location.search;
        const pin = parsePinnedThemeId(search);
        // e06d: the PERSISTED choice now sits between the pin and the `prefers-color-scheme` default
        // (config.ts `startupThemeId` is the single expression of that order). A persisted id the
        // registry cannot resolve — a user theme whose file was deleted — falls back rather than
        // leaving the window unstyled.
        const persisted = configuredThemeId(config);
        const themeId = startupThemeId(persisted, search, probe, (id: string) =>
            engine.registry.has(id),
        );
        // Reported so a boot that came up in the "wrong" theme names WHY: a persisted choice, a pin,
        // or the host preference. Diagnosing that from pixels alone cost e06b two CI rounds.
        const sourceNote =
            persisted === ""
                ? ", first-run (prefers-color-scheme)"
                : persisted === themeId
                  ? ", from config"
                  : `, config "${persisted}" UNRESOLVED`;
        // Reported so a red smoke names WHY it saw the colours it saw: "pinned" means the boot URL
        // chose the theme, its absence means the host's `prefers-color-scheme` did.
        const pinNote = pin === "" ? "" : pin === themeId ? ", pinned" : `, pin "${pin}" UNKNOWN`;
        const report = engine.apply(themeId);
        document.documentElement.setAttribute(
            "data-editor-theme",
            report.applied
                ? `${report.themeId} (${report.variableCount} tokens, fade ${report.fadeDurationMs}ms` +
                  `${report.reducedMotion ? ", reduced-motion" : ""}${pinNote}${sourceNote})`
                : `unavailable: ${report.diagnostic}`,
        );
        watchReducedMotion(engine);
        return engine;
    } catch (error) {
        document.documentElement.setAttribute(
            "data-editor-theme",
            `unavailable: ${error instanceof Error ? error.message : String(error)}`,
        );
        return undefined;
    }
}

/** Re-apply the active theme whenever the OS reduced-motion preference flips. Best-effort. */
function watchReducedMotion(engine: ThemeEngine): void {
    const scope = globalThis as { matchMedia?: (query: string) => unknown };
    if (typeof scope.matchMedia !== "function") {
        return;
    }
    const list = scope.matchMedia(REDUCED_MOTION_QUERY) as {
        addEventListener?: (type: string, listener: () => void) => void;
    };
    if (typeof list.addEventListener !== "function") {
        return; // an older engine with only the deprecated addListener — not worth a shim
    }
    list.addEventListener("change", () => {
        engine.reapply();
    });
}

/**
 * Load the watched user themes (and any package contributions) over the Shell feed (e06b).
 *
 * editor-core cannot read `~/.context/themes/*.theme.json` itself — it is a pure wire-client — so the
 * Shell watches them and publishes the bytes with a GENERATION counter (themes_bridge.h). This does
 * the first pull; the `ThemeController` re-registers only when that counter moves, which is what
 * makes the hot reload a counter compare on this side and one stat per owner-loop tick on the
 * Shell's.
 *
 * NEVER FATAL, exactly like the keybindings feed: a Shell that does not serve `themes.get` (an older
 * build, or a smoke's minimal router) leaves the built-in themes standing, and a malformed user theme
 * is rejected with a diagnostic rather than applied. The outcome is written onto
 * `<html data-editor-themes>` for the `--dump-dom` local repro and DevTools.
 */
async function startThemeFeed(bridge: ShellBridge, engine: ThemeEngine): Promise<void> {
    let detail = "built-ins only";
    try {
        const controller = new ThemeController(engine, new ThemesClient(bridge));
        const result = await controller.refresh();
        const accepted = result.registration?.accepted.length ?? 0;
        const rejected = result.registration?.rejected ?? [];
        detail =
            accepted === 0 && rejected.length === 0
                ? "no watched themes; built-ins only"
                : `${accepted} loaded, ${rejected.length} REJECTED` +
                  (rejected.length === 0
                      ? ""
                      : ` (${rejected.map((entry) => `${entry.id}: ${entry.diagnostic}`).join(" | ")})`);
    } catch (error) {
        detail = `theme feed unavailable: ${error instanceof Error ? error.message : String(error)}`;
    }
    if (typeof document !== "undefined") {
        document.documentElement.setAttribute("data-editor-themes", detail);
    }
}

/**
 * Wire the D8 command layer (e07d): build the ONE registry from all three sources, create the palette
 * over it, register the palette-open command, mount the palette overlay, and — only under the
 * `?ctx-smoke-palette` boot flag — drive the T2 command-driven scenario.
 *
 * NEVER THROWS: like the rest of boot, a failure here degrades to "no command layer" rather than an
 * unhandled rejection in a renderer nobody watches. Placed after `PanelHost.start()` so the roster is
 * known and after `LayoutPersistence.attach()` so a palette-driven layout change actually publishes.
 */
function startCommandLayer(
    host: PanelHost,
    client: PanelClient,
    theme: ThemeEngine | undefined,
): void {
    if (typeof document === "undefined") {
        return;
    }
    const roster = host.roster;
    if (roster === null) {
        return;
    }
    try {
        const registry = buildCommandRegistry({
            // The daemon RPC fan-in (D19) is a later seam; contract verbs are PROJECTED into the
            // palette (with their introspected docs) now, and executing one is an honest refusal until
            // the client fan-in lands — it never touches the bridge, so no boot-time refusal.
            contractDispatch: (method): CommandOutcome => ({
                ok: false,
                note: `daemon RPC fan-in not wired yet (D19): ${method}`,
            }),
            editorActions: makeEditorActions(host, theme),
            // Session undo/redo binding + dispatch land here (e07c); the wire REPLAY of the journal is
            // e09 (undo_journal.h). Executing them is an honest refusal until then.
            sessionActions: {
                undo: (): CommandOutcome => ({
                    ok: false,
                    note: "session undo replay lands in e09 (undo_journal.h)",
                }),
                redo: (): CommandOutcome => ({
                    ok: false,
                    note: "session redo replay lands in e09 (undo_journal.h)",
                }),
            },
            roster,
            // A panel-manifest command dispatches to its panel over the real `panel.command` bridge.
            panelDispatch: async (panelId, commandId): Promise<CommandOutcome> => {
                const result = await client.command(panelId, commandId, "");
                return result !== null && result.dispatched
                    ? { ok: true, note: `${panelId}/${commandId}` }
                    : { ok: false, note: `${panelId}/${commandId} not dispatched` };
            },
        });

        const palette = new Palette(registry);
        const view = new PaletteView({
            host: document.body,
            palette,
            // The real `editor.ui` bus (05 §5) is a later seam; filter by the stubbed "nothing focused"
            // context today and swap ONLY this provider when it lands.
            contextProvider: (): WhenContext =>
                resolveContext({ editorUi: STUB_EDITOR_UI, session: STUB_SESSION_STATE }),
        });
        view.mount();
        // Register the palette-open command AFTER the view exists, so its handler can reflect the model
        // into the overlay. It is bound to Ctrl+Shift+P in the default keymap (keymap.ts).
        registry.registerAll(
            paletteCommands({
                toggle: (): CommandOutcome => {
                    palette.toggle();
                    view.sync();
                    return { ok: true, note: palette.isOpen ? "palette opened" : "palette closed" };
                },
            }),
        );

        void runPaletteSmoke(registry, palette, host, view);
    } catch (error) {
        // Honest degradation, mirrored onto <html> like the other boot states.
        document.documentElement.setAttribute(
            "data-editor-commands",
            `command layer unavailable: ${error instanceof Error ? error.message : String(error)}`,
        );
    }
}

/**
 * The built-in editor actions the e07b editor commands dispatch to.
 *
 * `closeActivePanel` is the one that is FULLY wired in e07d — it is the observable the T2 palette smoke
 * drives (a palette-executed close → a Dockview layout change → an `editor.state.publish`). Panel
 * navigation and dock-move reach their real implementations with the 03 §6 input-pump / interaction
 * seam; until then they are honest refusals, which keeps the command REACHABLE (it is in the registry,
 * the palette, and the keymap) without faking an effect.
 *
 * `toggleTheme` is no longer one of them: e06b's theme engine makes it REAL — a live Dark<->Light swap
 * with no restart, preserving high contrast. It refuses only when there is no engine at all (a
 * documentless host), which is an honest "there is nothing to theme".
 */
function makeEditorActions(host: PanelHost, theme: ThemeEngine | undefined): EditorCommandActions {
    return {
        focusNextPanel: (): CommandOutcome => ({
            ok: false,
            note: "panel focus navigation arrives with the 03 §6 input-pump seam",
        }),
        focusPreviousPanel: (): CommandOutcome => ({
            ok: false,
            note: "panel focus navigation arrives with the 03 §6 input-pump seam",
        }),
        moveActivePanel: (direction): CommandOutcome => ({
            ok: false,
            note: `dock move (${direction}) arrives with the interaction seam`,
        }),
        closeActivePanel: (): CommandOutcome => {
            const mounted = host.mounted;
            // Never empty the layout — close only when more than one panel is mounted, mirroring the
            // e05d4 `applySmokeArrangement` guard.
            if (mounted.length <= 1) {
                return { ok: false, note: "no closable panel (would empty the layout)" };
            }
            const last = mounted[mounted.length - 1];
            const closed = last !== undefined && host.close(last);
            return closed
                ? { ok: true, note: `closed ${last}` }
                : { ok: false, note: "the docking root refused the close" };
        },
        toggleTheme: (): CommandOutcome => {
            if (theme === undefined) {
                return { ok: false, note: "no theme engine on this host (no document to theme)" };
            }
            const report = theme.toggleAppearance();
            return report.applied
                ? { ok: true, note: `theme switched to ${report.themeId}` }
                : { ok: false, note: report.diagnostic };
        },
    };
}

/**
 * The M9 e07d T2 palette-smoke seam — a NO-OP unless the boot URL carries `?ctx-smoke-palette`.
 *
 * Drives a scenario PURELY through the command layer, exactly as an agent or a T2 test would (10
 * "the palette surface ≡ the scriptable surface"): OPEN the palette via its command, FILTER by a
 * query, then EXECUTE the top match. The chosen command is `view.panel.close`, whose effect — a
 * Dockview layout change → an `editor.state.publish` over the live bridge — is the OBSERVABLE the C++
 * `editor-cef-smoke-shell-palette` leg asserts (`states_published() >= 1`). The outcome is mirrored
 * onto `<html data-editor-palette>` for the `--dump-dom` local repro, the same diagnosability
 * discipline `markDocument` gives the boot state.
 *
 * Guarded so it is inert in the shipping editor: it requires the explicit flag AND more than one
 * mounted panel (so it never empties the layout), and it is total — any failure just records a
 * diagnostic and leaves the editor usable, which the smoke would then catch as a missing publish.
 *
 * Drives the palette by mutating the MODEL and reflecting each mutation into the VIEW with
 * `view.sync()` — the same "mutate the model, then sync the view" step the palette-toggle command and
 * the view's own listeners perform (the model is passive; the view reflects it). Syncing after EXECUTE
 * is load-bearing for correct UX: `palette.execute` closes only the model (palette.ts), so without the
 * reflect the overlay would linger visually over the composited frame instead of dismissing the way a
 * real Enter/click activation does (PaletteView.#activateSelected).
 */
async function runPaletteSmoke(
    registry: CommandRegistry,
    palette: Palette,
    host: PanelHost,
    view: PaletteView,
): Promise<void> {
    if (typeof location === "undefined" || !location.search.includes("ctx-smoke-palette")) {
        return;
    }
    let detail = "palette smoke: nothing executed";
    try {
        const mounted = host.mounted;
        const focus = mounted.length > 0 ? (mounted[mounted.length - 1] ?? "") : "";
        // The scenario supplies its OWN when-context (a focused panel), so the palette surfaces the
        // panel-focus-guarded `view.panel.close` deterministically regardless of the stubbed editor.ui.
        const context: WhenContext = { panelFocus: focus, textInputFocus: false };
        // OPEN the palette through the command layer (its own registered command), not a private call.
        // The toggle command's handler already syncs the view, so the overlay is now visible.
        await registry.execute(PALETTE_TOGGLE_COMMAND_ID);
        // FILTER by a fuzzy query — proves the palette's filter runs over the live registry — and
        // reflect it into the overlay, exactly as the view's own input listener does.
        palette.setQuery("close panel");
        view.sync();
        const results = palette.results(context);
        const target =
            results.find((entry) => entry.command.id === "view.panel.close") ?? results[0];
        if (target === undefined) {
            detail = "palette smoke: no command matched 'close panel'";
        } else {
            // EXECUTE through the palette (→ the ONE registry), the SAME path a real activation drives.
            const outcome = await palette.execute(target.command.id);
            // Reflect the model's close into the overlay — the view step a real activation performs
            // (PaletteView.#activateSelected) but a direct model.execute() does not, so the overlay is
            // actually dismissed rather than left lingering over the composited frame.
            view.sync();
            detail = `palette smoke: executed ${target.command.id} -> ${
                outcome.ok ? "ok" : "refused"
            } (${outcome.note})`;
        }
    } catch (error) {
        detail = `palette smoke error: ${error instanceof Error ? error.message : String(error)}`;
    }
    if (typeof document !== "undefined") {
        document.documentElement.setAttribute("data-editor-palette", detail);
    }
}

/**
 * The M9 e05d4 restart-smoke seam — a NO-OP unless the boot URL carries `?ctx-smoke-arrange`.
 *
 * The restart smoke needs the FIRST boot to persist a REAL, non-default arrangement so its restart
 * proof is not indistinguishable from a fresh boot. There is no command registry yet to drive a dock
 * change from the Shell (design 09 §1 makes T2 command-driven; that arrives with e06), so a URL flag
 * is the v1 seam. It closes the LAST docked panel — a deterministic dock-arrangement change Dockview
 * serialises distinctly and restores cleanly — which fires `onDidLayoutChange`, so LayoutPersistence
 * publishes the new arrangement (debounced) with no further prompting.
 *
 * Guarded three ways so it is inert in the shipping editor: it requires the explicit flag, it needs
 * more than one panel (never empties the layout), and it is total — any failure just leaves the
 * default arrangement, which the smoke would then catch as a missing non-default publish.
 */
function applySmokeArrangement(host: PanelHost): void {
    if (typeof location === "undefined" || !location.search.includes("ctx-smoke-arrange")) {
        return;
    }
    const mounted = host.mounted;
    const last = mounted.length > 1 ? mounted[mounted.length - 1] : undefined;
    if (last !== undefined) {
        host.close(last);
    }
}

/**
 * Read the per-user config at boot (e06d) — best-effort, never fatal.
 *
 * The document decides the startup theme, so it is fetched BEFORE anything is painted. A Shell that
 * does not serve `config.get` (an older build, or a smoke's minimal router) yields the empty snapshot,
 * which is indistinguishable from a genuine first run — the correct degrade, since in both cases
 * nothing has been remembered. The outcome is mirrored onto `<html data-editor-config>` for the
 * `--dump-dom` local repro, the same diagnosability discipline `markDocument` gives the boot state.
 */
async function loadUserConfig(bridge: ShellBridge): Promise<UserConfigSnapshot> {
    let snapshot = EMPTY_CONFIG_SNAPSHOT;
    let detail = "unavailable";
    try {
        snapshot = await new ConfigClient(bridge).get();
        const theme = configuredThemeId(snapshot);
        detail =
            `gen ${snapshot.generation}, ${snapshot.writable ? "writable" : "READ-ONLY (no home)"}` +
            (theme === "" ? ", no theme recorded" : `, theme "${theme}"`);
    } catch (error) {
        detail = `config feed unavailable: ${error instanceof Error ? error.message : String(error)}`;
    }
    if (typeof document !== "undefined") {
        document.documentElement.setAttribute("data-editor-config", detail);
    }
    return snapshot;
}

/** The local-panel factories this build registers, plus a handle on the mounted Settings panel. */
interface SettingsBringUp {
    readonly factories: ReadonlyMap<string, LocalPanelFactory>;
    /** The live mount, or undefined until Dockview has materialised the panel. */
    mount(): SettingsPanelMount | undefined;
}

/**
 * Build the `builtin.settings` local-panel factory (e06d) and the wiring that makes it real.
 *
 * THE TWO HALVES OF A THEME PICK MEET HERE, and nowhere else: the panel is handed one callback, which
 * APPLIES the theme through the engine (instant, local, always) and REQUESTS the write through the
 * config client (durable, remote, allowed to fail). Keeping both out of settings.ts is what lets that
 * panel be proven in a browser tier with no bridge and no ThemeEngine; keeping the request behind the
 * typed client is what keeps the write path to one door (config.ts's own gate).
 */
function makeSettingsPanel(
    bridge: ShellBridge,
    theme: ThemeEngine | undefined,
    config: UserConfigSnapshot,
): SettingsBringUp {
    let mounted: SettingsPanelMount | undefined;
    const client = new ConfigClient(bridge);
    const factories = new Map<string, LocalPanelFactory>();

    factories.set(SETTINGS_PANEL_ID, (container: HTMLElement): (() => void) => {
        const choices: readonly ThemeChoice[] =
            theme === undefined
                ? []
                : theme.registry.list().map((entry) => ({
                      id: entry.id,
                      name: entry.name,
                      source: entry.source,
                      highContrast: entry.highContrast,
                  }));
        const mount = mountSettings(container, {
            themes: choices,
            activeThemeId: theme?.activeId ?? "",
            keybindingsPath: config.keybindingsPath,
            writable: config.writable,
            systemThemeId: (): string => (theme === undefined ? "" : defaultThemeId(theme.probe)),
            onSelectTheme: (themeId: string): void => {
                // APPLY first: the switch is what the user asked for and must not wait on IO.
                const report = theme?.apply(themeId);
                if (report !== undefined && !report.applied) {
                    mount.reportSave({ stored: false, diagnostic: report.diagnostic });
                    return;
                }
                // Then REQUEST the write. The Shell is the single writer (C-F14); its verdict comes
                // back to the panel so a failed save is visible rather than implied.
                void client.setTheme(themeId).then((result) => {
                    mount.reportSave({ stored: result.stored, diagnostic: result.diagnostic });
                });
            },
        });
        mounted = mount;
        return (): void => {
            mounted = undefined;
        };
    });

    return { factories, mount: (): SettingsPanelMount | undefined => mounted };
}

/**
 * The M9 e06d T2 settings-smoke seam — a NO-OP unless the boot URL carries `?ctx-smoke-settings`.
 *
 * Drives a REAL theme change through the REAL Settings panel: pick the first offered theme that is not
 * the active one and select it exactly as a user's `<select>` change would. The observable the live
 * `editor-cef-smoke-shell-settings` leg asserts is on the SHELL side — `UserConfigStore::writes() >= 1`
 * plus the chosen theme id actually present in the config file on disk — which can only be true if this
 * panel rendered, its picker was operable, the apply succeeded, and `config.set` round-tripped. A fresh
 * boot with no interaction writes nothing, so that assertion is not satisfiable by accident.
 *
 * Guarded so it is inert in the shipping editor (explicit flag; total; a no-op with fewer than two
 * themes), and mirrored onto `<html data-editor-settings>` for the `--dump-dom` repro.
 */
function runSettingsSmoke(mount: SettingsPanelMount | undefined): void {
    if (typeof location === "undefined" || !location.search.includes("ctx-smoke-settings")) {
        return;
    }
    let detail = "settings smoke: the Settings panel did not mount";
    try {
        if (mount !== undefined) {
            const options = Array.from(
                mount.element.querySelectorAll<HTMLOptionElement>("option"),
            );
            const target = options.find((option) => option.value !== mount.selectedThemeId);
            if (target === undefined) {
                detail = `settings smoke: no alternative theme among ${mount.themeCount}`;
            } else {
                mount.selectTheme(target.value);
                detail = `settings smoke: selected ${mount.selectedThemeId} of ${mount.themeCount}`;
            }
        }
    } catch (error) {
        detail = `settings smoke error: ${error instanceof Error ? error.message : String(error)}`;
    }
    if (typeof document !== "undefined") {
        document.documentElement.setAttribute("data-editor-settings", detail);
    }
}
