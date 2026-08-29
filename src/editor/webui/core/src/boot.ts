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
    type Command,
    type CommandOutcome,
    type CommandRegistry,
    type EditorCommandActions,
    type PlayCommandActions,
    type SessionCommandActions,
} from "./commands.js";
import { EditorStateClient, LayoutPersistence } from "./editorstate.js";
import { editorCoreInfo } from "./info.js";
import { Keymap, KeybindingsClient } from "./keymap.js";
import { Palette, PALETTE_TOGGLE_COMMAND_ID, paletteCommands } from "./palette.js";
import { PaletteView } from "./palette_view.js";
import { PanelClient } from "./panels.js";
import { PanelHost, type LocalPanelFactory, type PanelVerbBinding } from "./panelhost.js";
import {
    PANEL_VERB_COMMAND_INVOKE,
    makePanelBridgeVerbs,
    type PanelCapabilityGrants,
    type PanelDaemonCall,
    type PanelDaemonOutcome,
    type PanelUiSubscribeOutcome,
} from "./panelverbs.js";
import { ShellPackageGrants } from "./packagegrants.js";
import { PANEL_UI_DELIVER_VERB, PackageUiFanout } from "./packageui.js";
import {
    WINDOW_APPEARANCE_DARK,
    WINDOW_APPEARANCE_LIGHT,
    WindowClient,
    type ChromeState,
    type WindowSeed,
} from "./window.js";
import {
    EDITOR_PLAYBAR_ID,
    findChromeStripElements,
    startChromeStrips,
    subscribeChromeFacts,
    type ChromeMount,
    type ChromeStrips,
} from "./chrome.js";
import { makePlayActions, mountPlaybar, type PlaybarHolder } from "./playbar.js";
import { DragClient, makeDropZoneHitTest, pumpCrossWindowDrag } from "./drag.js";
import { EditorUiBus, UI_TOPIC_THEME_CHANGED } from "./uibus.js";
import { wireUiMirror, type UiMirrorWiring } from "./uimirror.js";
import {
    PANEL_EVENTS_DELIVER_VERB,
    PackageEventPump,
    type PackageEventBatch,
} from "./packageevents.js";
import { createNotificationHost, type NotificationHost } from "./notifications.js";
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
    type ThemeChangedPayload,
    type ThemeRoot,
} from "./theme.js";
import { WELCOME_MODE_WELCOME, WelcomeClient, mountWelcome } from "./welcome.js";
import { BannerClient, mountBanners, type BannerData } from "./banners.js";
import {
    SessionControlClient,
    SessionFeed,
    defaultSessionScheduler,
    describeSessionRead,
    type SessionReadReport,
} from "./session.js";
import {
    DaemonSessionState,
    resolveContext,
    STUB_EDITOR_UI,
    type WhenContext,
} from "./when.js";

/** The element the docking root mounts into. Mirrors `app/index.html`'s `<main id="editor-root">`. */
export const EDITOR_ROOT_ID = "editor-root";

/**
 * The attribute the BOOT-TIME session read and its resolved when-context are reported onto (M9 e08d).
 *
 * The same diagnosability discipline `markDocument` gives the boot state and `data-editor-theme` /
 * `data-editor-config` / `data-editor-keybindings` give their feeds — AND, unlike those, it is a
 * TEST SURFACE: `boot.test.ts` reads it to prove the live editor's when-context tracks the daemon's
 * play state, because the resolved context is otherwise reachable only from inside the palette.
 *
 * It is a SNAPSHOT of the first read, deliberately NOT kept live by the poll: its job is to report
 * how boot resolved (and to be the anti-stub gate's readout), not to mirror the session. The LIVE
 * value is the provider closure `startSession` returns — which is what the palette actually filters
 * on, and the only thing a consumer should read the current play state from.
 */
export const SESSION_ATTRIBUTE = "data-editor-session";

/** The e14d notification strip's host. Mirrors `app/index.html`'s `<div id="editor-banners">`. */
export const EDITOR_BANNERS_ID = "editor-banners";

/**
 * The attribute the boot-time `chrome.state` read is reported onto (editor-window-chrome a1).
 *
 * The same diagnosability discipline `data-editor-session` follows, and like it a TEST SURFACE: the
 * DOM tier reads it to prove the live boot really fetched the chrome contract (a value only the
 * served state could produce), independent of whether the served document carries the a2 strip
 * elements. A SNAPSHOT of the boot read, deliberately: runtime `maximized` flips arrive as
 * `editor.ui.chrome` facts (uibus.ts `UI_TOPIC_CHROME`), and the a2 strips consume the VALUE this
 * boot hands them (`startChromeStrips` below), not this attribute.
 */
export const CHROME_ATTRIBUTE = "data-editor-chrome";

/** Report the boot-time chrome read (a1) — `mode=<m> inset=<l>,<r> maximized=<b> focused=<b> window=<w>`. */
function reportChrome(state: ChromeState): void {
    if (typeof document === "undefined") {
        return;
    }
    document.documentElement.setAttribute(
        CHROME_ATTRIBUTE,
        `mode=${state.mode} inset=${String(state.controlsInset.left)},${String(
            state.controlsInset.right,
        )} maximized=${String(state.maximized)} focused=${String(state.focused)} window=${state.window}`,
    );
}

/**
 * The a1 chrome-control smoke seam — a NO-OP unless the boot URL carries `?ctx-smoke-chrome`.
 *
 * Under the flag it drives each of the three window-control verbs ONCE, awaited in order so the
 * live smoke's counters are deterministic. The Shell in that smoke binds no handlers, so each
 * answers the honest `accepted:false` — the claim the smoke asserts is the ROUTING (the counters
 * move and `bridge.refused() == 0` holds), not the OS effect, which only a windowed backend has.
 * Inert in the shipping editor: without the flag nothing here fires, and the a2 titlebar controls
 * dispatch these verbs only on a user activation (chrome.ts), so the smoke counters stay exact.
 */
async function runChromeSmoke(client: WindowClient): Promise<void> {
    if (typeof location === "undefined" || !location.search.includes("ctx-smoke-chrome")) {
        return;
    }
    await client.minimize();
    await client.toggleMaximize();
    await client.focus();
}

/** What `bootEditorCore` did — returned so a caller (and a test) can assert on it. */
export interface BootReport {
    /** False when the bundle is running outside the Shell (no injected query function). */
    readonly attached: boolean;
    /** True once the full JS -> native -> JS -> native handshake completed. */
    readonly ready: boolean;
    /** How many panels PanelHost mounted. 0 when the app layer did not come up. */
    readonly panelsMounted: number;
    /**
     * Rostered panels this build cannot host (`hosted: false`) — whatever the HOST reports, not a
     * list maintained here: this comment has already drifted twice (e05d3 hosted Scene tree and
     * Inspector, e08b the playbar, e09c the session-undo panel). REPORTED rather than silently
     * skipped: "a panel is missing" must be an observable fact.
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

        // --- the e14d notification banners (design 07 §4 / 08) -----------------------------------
        // Asked for ONCE here, and handed to whichever surface comes up. Both calls degrade to null
        // on an `unknown_method` refusal, so a Shell with no banner bridge (the pre-e14d smokes)
        // renders exactly what it did before. NOTE: editor-core makes NO network call of its own —
        // the version check is the Shell's, for the privacy reason banners.ts documents.
        const banners = new BannerClient(bridge);
        const bannerData: BannerData = {
            update: await banners.updateState(),
            link: await banners.daemonLinkState(),
            handlers: {
                onOpenDownloads: (): void => {
                    void banners.openDownloads();
                },
                onDismiss: (): void => {
                    void banners.dismissUpdate();
                },
            },
        };

        // --- the chrome contract (editor-window-chrome a1, target design 02 §1) -------------------
        // Fetched at boot BESIDE `welcome.state` — i.e. before the welcome branch below — because
        // the strips (a2) render in BOTH welcome and project modes; the fetch must not depend on
        // which path boot takes. Refusal-tolerant like every boot feed: an older Shell that does not
        // route `chrome.state` yields the honest `system`/inset-0 default and the editor renders
        // exactly what a stock OS frame implies. Reported on <html data-editor-chrome> (the
        // renderer's out-of-band diagnostic channel + the DOM tier's test surface); runtime
        // `maximized` flips arrive separately, as `editor.ui.chrome` facts over the mirror relay.
        //
        // The welcome read (consumed by the branch below) is ISSUED CONCURRENTLY: the two reads are
        // independent by design — that independence is exactly why the chrome fetch sits before the
        // welcome branch — so boot pays one bridge round-trip of latency here, not two.
        const chromeClient = new WindowClient(bridge);
        const [chromeState, welcomeState] = await Promise.all([
            chromeClient.chromeState(),
            new WelcomeClient(bridge).state(),
        ]);
        reportChrome(chromeState);
        // The `?ctx-smoke-chrome` seam (a NO-OP without the flag): drive the three control verbs so
        // the live boot smoke can assert their routing end to end. Awaited so the smoke's counters
        // are settled before boot reports ready.
        await runChromeSmoke(chromeClient);

        // --- the appearance report (editor-window-chrome b1, target design 02 §3) ----------------
        // The Shell's OS window chrome — Windows' DWM dark-mode edge tint / drop shadow — follows
        // the ACTIVE theme's appearance. ONE subscription serves boot AND every later switch: the
        // theme bus RETAINS the last `theme-changed` envelope (snapshot-on-subscribe, uibus.ts), so
        // subscribing here — after startTheme's boot apply above — is handed the current appearance
        // immediately, and a live Dark → Light toggle re-reports through the same listener.
        // Fire-and-forget by design: the client is refusal-tolerant, and a frame tint must never
        // gate the boot. The ternary below is NOT a redundant re-derivation of the payload's
        // already-closed "dark" | "light" type: it is boot's only PRODUCTION value-reference to
        // the two token constants, which is what keeps them in the shipped bundle for
        // check_webui_assets' cross-check — a test-only reference gets tree-shaken out (the
        // WELCOME_MODE_PROJECT precedent in that gate's own comments).
        if (theme !== undefined) {
            theme.bus.subscribe<ThemeChangedPayload>(UI_TOPIC_THEME_CHANGED, (event): void => {
                void chromeClient.setAppearance(
                    event.payload.appearance === WINDOW_APPEARANCE_LIGHT
                        ? WINDOW_APPEARANCE_LIGHT
                        : WINDOW_APPEARANCE_DARK,
                );
            });
        }

        // --- the chrome strips (editor-window-chrome a2, target design 02 §2 / §6) ---------------
        // Mounted BEFORE the welcome branch, deliberately: the mockup's frame is the app's frame,
        // so the strips render in BOTH welcome and project modes (the play-bar slot hides on the
        // welcome screen — no session to control). The titlebar's palette button dispatches through
        // the LATE-BOUND registry holder (`liveRegistry.current` is filled once startCommandLayer
        // runs, and stays undefined on the welcome path — the dispatch is then an honest no-op,
        // never a throw). The regionProvider + publisher this wires are the a2 deliverable: the
        // SAME provider is handed to LayoutPersistence below, so the layout-change and resize/DPI
        // publish paths can never disagree about the region set. NEVER fatal, like every boot feed:
        // a document without the strip elements (an older served page, a bare harness) renders
        // exactly what it did before a2.
        const isWelcome = welcomeState !== null && welcomeState.mode === WELCOME_MODE_WELCOME;
        const liveRegistry = { current: undefined as CommandRegistry | undefined };
        let chromeStrips: ChromeStrips | undefined;
        try {
            const stripElements =
                typeof document === "undefined" ? null : findChromeStripElements(document);
            if (stripElements !== null) {
                const chromeStateClient = new EditorStateClient(bridge);
                chromeStrips = await startChromeStrips(stripElements, {
                    state: chromeState,
                    projectName: welcomeState?.projectName ?? "",
                    welcome: isWelcome,
                    controls: chromeClient,
                    executeCommand: (commandId: string): void => {
                        void liveRegistry.current?.execute(commandId);
                    },
                    publishRegions: (regions): Promise<boolean> =>
                        chromeStateClient.publishRegions(regions),
                });
            }
        } catch {
            // Reported by the absence of `data-editor-strips`; the editor is up and usable.
        }

        // --- the welcome screen (e14c, design 07 §4 / D13) ----------------------------------------
        // A BARE launch shows the app's front door (recent projects / "Open project…" / "New from
        // template") instead of the editor. Ask the Shell, and DEFAULT to the editor path when there
        // is no welcome surface: `welcomeState` (fetched above, beside the chrome read) is null on
        // an `unknown_method` refusal, which is exactly what the CEF boot smokes (which install no
        // welcome surface) get — so they mount panels unchanged. Only an explicit `mode: "welcome"`
        // diverts to the front door.
        if (isWelcome) {
            const container =
                typeof document === "undefined" ? null : document.getElementById(EDITOR_ROOT_ID);
            let recentCount = 0;
            let templateCount = 0;
            if (container !== null) {
                const mount = mountWelcome(bridge, container, welcomeState, bannerData);
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

        // --- the e14d notification strip on the EDITOR path ---------------------------------------
        // The daemon-lost banner's real home: this is the only surface where losing the daemon
        // changes what the next keystroke does. Mounted BEFORE the panels so a read-only editor says
        // so from its first frame rather than after the dock finishes materialising. A document with
        // no `#editor-banners` element (an older shell, a bare harness) simply gets no strip.
        const bannerHost =
            typeof document === "undefined" ? null : document.getElementById(EDITOR_BANNERS_ID);
        if (bannerHost !== null) {
            mountBanners(bannerHost, bannerData);
        }

        // --- the when-context sources (e08d, design 05 §4 / §6) -----------------------------------
        // Built BEFORE the panels because the command layer they bring up filters on it. This is the
        // ONE construction site of the editor's when-context: the closure returned here is what the
        // palette is handed, and what the `data-editor-session` report below is computed from — so a
        // regression that re-froze the session source could not show a live value in the report while
        // serving a frozen one to the palette.
        //
        // d1: the play-bar strip is FED from this same feed — the holder is filled by startPlaybar
        // below, and every session read from then on re-renders the strip. One sink, one poll, two
        // projections (the when-contexts and the strip), so they can never disagree.
        const playbarHolder: PlaybarHolder = { current: undefined };
        const session = await startSession(bridge, (report: SessionReadReport): void => {
            playbarHolder.current?.applySession(report.playState, report.simTick);
        });
        const whenContext = session.whenContext;

        // --- the play-bar strip (editor-window-chrome d1, target design 02 §7) --------------------
        // Mounted on the EDITOR path only — the welcome branch returned above, and a2 already hides
        // the slot there (no session to control). The returned actions are the `play.*` command
        // handlers startPanels threads into the registry: strip buttons, palette and the d3 menu
        // all dispatch ONE implementation over `session.control`.
        const playActions = startPlaybar(bridge, session.session, playbarHolder, liveRegistry);

        // --- the app layer (e05d1) ----------------------------------------------------------------
        // The channel is proven; bring up the panels. A failure HERE is reported but does NOT undo
        // `ready`: the bridge genuinely does round-trip, and conflating "the editor has no panels"
        // with "the editor cannot talk to the Shell" would send the next diagnosis in exactly the
        // wrong direction.
        const panels = await startPanels(
            bridge,
            theme,
            config,
            whenContext,
            chromeStrips,
            liveRegistry,
            playActions,
        );

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
    whenContext: () => WhenContext,
    // a2: the mounted chrome strips (undefined when the document carries none). Two duties here:
    // the strip's regionProvider feeds LayoutPersistence (so a Dockview layout change republishes
    // the SAME region set the resize/DPI publisher does), and the mount is threaded into the
    // window mechanism so the `editor.ui.chrome` maximized fact can flip the titlebar glyph.
    chrome: ChromeStrips | undefined,
    // a2: THE one late-bound command-registry holder, created by bootEditorCore. Filled here once
    // startCommandLayer runs; read at call time by BOTH the panel verb tables below and the
    // titlebar's palette dispatch (which mounted BEFORE the registry existed) — one holder, so the
    // two consumers can never see different registries.
    liveRegistry: { current: CommandRegistry | undefined },
    // d1: the real `play.*` command handlers (startPlaybar builds them over `session.control`),
    // threaded into buildCommandRegistry so the palette's transports write the same path the strip
    // buttons do.
    playActions: PlayCommandActions,
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
        const windowClient = new WindowClient(bridge);
        // e10c: the cross-window DRAG probe/answer for THIS window (any window can be a drop TARGET).
        const dragClient = new DragClient(bridge);
        // e10b: is this window a TEAR-OUT TARGET? A boot seed means "a panel was moved here — open
        // ONLY it, restored from its D6 state", never the fresh default arrangement. An ordinary
        // window (and every CEF smoke that installs the surface unseeded) reports `seeded:false` and
        // takes the default path unchanged.
        const boot = await windowClient.seed();
        const seed: WindowSeed | null = boot.seeded ? boot.seed : null;
        // The Settings panel is editor-core's own content (e06d): the roster declares it
        // `content.type: "local"` and THIS is the build that knows how to draw it. Nothing else about
        // PanelHost changes — an unregistered local panel is reported unavailable like any other.
        const settings = makeSettingsPanel(bridge, theme, config);
        // M9 e13c-4 — THE REAL GRANT SOURCE, read ONCE, before any panel mounts.
        //
        // AWAITED HERE rather than late-bound like the registry, deliberately: a package panel's very
        // FIRST verb must meet the operator's real answer, and a grant source that were still loading
        // would have to answer something in the meantime — which could only be "deny", i.e. a
        // package's first call would be refused for a grant it actually holds. `load` never rejects
        // (packagegrants.ts): a Shell with no route yields the deny-all floor, so this cannot delay or
        // fail a boot.
        const packageGrants: PanelCapabilityGrants = await ShellPackageGrants.load(bridge);
        // ...and the `editor.ui` fan-out the GRANTED half of `bridge.ui.subscribe` delivers over.
        // LATE-BOUND through a holder for the same reason the registry is: the bus is constructed
        // further down (it needs this window's id for the mirror origin), while the verb tables are
        // built here. `undefined` means this window has no bus — the welcome-screen boot, where
        // `panel.list` is unavailable — and a subscribe then refuses honestly rather than pretending.
        let packageUi: PackageUiFanout | undefined;
        // The ONE command registry, LATE-BOUND into the panel verb tables (M9 e13b-2) through the
        // `liveRegistry` holder parameter above. It is built by `startCommandLayer` further down —
        // after `host.start()` has already created every renderer and every port — so the tables
        // close over the holder rather than over a value. A package panel's first verb can only
        // arrive once its document has loaded and handshaken, which is strictly after that, so the
        // `undefined` window is not reachable by a real panel; the tables refuse honestly if it
        // ever were.
        const host = new PanelHost({
            container,
            client,
            localPanels: settings.factories,
            // THE THEME PUSH CHANNEL (M9 e13d) — `ThemeEngine` already broadcasts every apply into
            // it (theme.ts, the constructor's bus subscription); this is the wiring that finally
            // gives it real frames to push to. A window with no theme engine OMITS the option (the
            // spread, required by `exactOptionalPropertyTypes`) and package panels are simply never
            // re-tokened, which is the honest degraded state.
            ...(theme === undefined ? {} : { themeChannel: theme.iframes }),
            panelVerbs: (binding: PanelVerbBinding) =>
                makePanelBridgeVerbs({
                    panelId: binding.panelId,
                    packageId: binding.packageId,
                    declaredCapabilities: binding.declaredCapabilities,
                    manifestCommandIds: binding.manifestCommandIds,
                    registry: () => liveRegistry.current,
                    whenContext,
                    // THE GRANT SOURCE — the operator's persisted install consent since M9 e13c-4.
                    // This ONE ARGUMENT is the whole editor-core-side change the gate ever needed
                    // (panelverbs.ts § the capability gate); `DENY_ALL_CAPABILITY_GRANTS` is still
                    // what `ShellPackageGrants.load` falls back to on every failure path.
                    grants: packageGrants,
                    // THE e13c-4 `editor.ui` FAN-OUT, with THIS panel's package closed over — the
                    // same structural property as `daemonCall` below, so `bridge.ui.subscribe` has no
                    // argument by which one package could subscribe another's panels. Reached ONLY
                    // after `requireCapability` has passed (panelverbs.ts § the enforcement point).
                    uiSubscribe: (topics: readonly string[]): PanelUiSubscribeOutcome =>
                        packageUi === undefined
                            ? {
                                  topics: [],
                                  // HONEST, not silently accepted: a window with no `editor.ui` bus
                                  // can never deliver a fact, and answering `{topics}` would leave the
                                  // package waiting forever for events that structurally cannot come.
                                  diagnostic: "this window has no editor.ui bus to subscribe to",
                              }
                            : packageUi.subscribe(binding.packageId, topics),
                    // THE PULL half of theme delivery, read from the SAME field the push replays
                    // from — so `bridge.theme.tokens` cannot answer with a theme the channel is not
                    // pushing (theme.ts § `IframeThemeChannel.last`). LATE-BOUND through the closure
                    // because the active theme changes under a mounted panel.
                    themeTokens: () => theme?.iframes.last?.payload,
                    state: binding.state,
                    // THE e13c-1 DAEMON FAN-IN, with THIS panel's package closed over. Built here
                    // per binding — never handed a package id at call time — so `bridge.call` has no
                    // argument by which one package could ride another's baseline session.
                    daemonCall: makePackageDaemonCall(bridge, binding.packageId),
                    request: binding.request,
                }),
        });
        const report = await host.start(seed !== null ? { only: seed.panelId } : {});
        // Restore the moved panel's D6 state onto the freshly-opened panel — the SAME recreate path
        // (open + panel.state.set + refresh) window-close rehome uses, which is the whole point of D6:
        // one mechanism, exercised here at boot and by the rehome poll at runtime. Asserted on the
        // RENDERED output (a value a fresh panel could not have) by the live tear-out smoke.
        if (seed !== null && report.started) {
            await client.setState(seed.panelId, seed.state);
            await host.refreshAll();
        }

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
                // A seeded (torn-out) window is a FRESH target: it restores no persisted arrangement
                // (cross-window layout persistence is e10d) — it shows exactly the moved panel. An
                // ordinary window restores its layout + per-panel D6 state as before.
                if (seed === null) {
                    const stateClient = new EditorStateClient(bridge);
                    const persistence = new LayoutPersistence({
                        panelHost: host,
                        panelClient: client,
                        stateClient,
                        // a2: the FIRST real regionProvider (02 §6) — the titlebar's caption +
                        // control rects. The spread keeps the option absent (the empty default)
                        // when no strips mounted, per exactOptionalPropertyTypes.
                        ...(chrome === undefined ? {} : { regionProvider: chrome.provider }),
                    });
                    const restoreReport = await persistence.restore();
                    // Report the restore OUTCOME to the Shell (e05d4). The restart smoke asserts this
                    // is `layoutRestored:false` on a fresh boot and `true` on a boot that reapplied a
                    // persisted arrangement — the end-to-end proof that the arrangement round-tripped
                    // through the Shell's editor-state store. Best-effort: `reportRestore` swallows a
                    // Shell refusal, so it can never keep `attach` from running below.
                    await stateClient.reportRestore(restoreReport);
                    persistence.attach();
                }
                // --- the cross-window move machinery (e10b) + editor.ui mirror (e10d-drill2) ----
                // Wire it in EVERY window: the rehome poll (this window opens panels moved INTO it,
                // from a "move to window N" or a peer's window-close rehome) and, for a non-primary
                // window, the pagehide rehome-to-window-0 ("close a window with panels ⇒ they rehome,
                // never lost"). Both use the SAME recreate path as the seed-open above (D6). It also
                // brings up the cross-window `editor.ui` MIRROR on a per-window-origin bus (e10d-drill2).
                await startWindowMechanism(
                    windowClient,
                    dragClient,
                    host,
                    client,
                    bridge,
                    // ⚠ IT SHARES THE PUSH PATH WITH e13c-2, deliberately: `deliverToPackage` is the
                    // same method `PackageEventPump` pushes daemon batches through, so a package's two
                    // streams arrive over ONE port with one lifetime and one failure mode. Only the
                    // VERB differs.
                    (bus: EditorUiBus): void => {
                        packageUi = new PackageUiFanout(bus, {
                            deliver: (packageId: string, event): number =>
                                host.deliverToPackage(packageId, PANEL_UI_DELIVER_VERB, event)
                                    .delivered,
                        });
                    },
                    chrome?.mount,
                );
                // --- the command layer + palette (e07d) ----------------------------------------
                // The docking root is up and persistence is live; wire the ONE command registry, the
                // palette over it, and (only under `?ctx-smoke-palette`) drive the T2 command-driven
                // scenario. Placed AFTER `persistence.attach()` so a palette-driven layout change
                // publishes over the live editor.state channel — the observable the T2 smoke asserts.
                // Filling the holder ALSO resolves the titlebar palette button's late-bound
                // dispatch (a2 — see the strips block in bootEditorCore): from here on the button
                // opens the real palette.
                liveRegistry.current = startCommandLayer(
                    host,
                    client,
                    windowClient,
                    theme,
                    whenContext,
                    playActions,
                );
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
 * Bring up the daemon session feed and return THE editor's when-context provider (e08d).
 *
 * THIS IS THE FIX FOR THE FROZEN `playState`. Before e08d, `boot.ts` resolved the when-context from
 * e08b's `STUB_SESSION_STATE` boot baseline, so the live editor's `playState` was `edit` for the
 * whole session and every `playState == playing` clause was wrong — while `DaemonSessionState`, the
 * real source, was reachable from the tests only. Here that source is constructed, fed over the
 * Shell's `session.state` relay (session.ts), and handed to `resolveContext` as the `session` half.
 *
 * ONE PROVIDER, ONE SESSION OBJECT. The returned closure is what the palette is given AND what the
 * `data-editor-session` report is computed from, so the report cannot show a live value while the
 * palette filters on a frozen one. That is what makes `boot.test.ts`'s assertion a real anti-stub
 * gate rather than a second, independently-correct code path.
 *
 * NEVER FATAL and never throws, like every other boot feed: a Shell that does not serve the relay
 * (an older build) leaves the sink on its `edit` boot baseline — which is exactly what such a build
 * can honestly know — and the editor comes up filtering on it. The outcome is mirrored onto
 * `<html data-editor-session>` so a `--dump-dom` repro and DevTools can read WHY.
 */
async function startSession(
    bridge: ShellBridge,
    // d1: the strip's update channel — handed to the feed so every completed read (the boot read
    // and every poll tick) re-renders the play bar from the same sink the when-contexts read.
    onRead?: (report: SessionReadReport) => void,
): Promise<{ whenContext: () => WhenContext; session: DaemonSessionState }> {
    const session = new DaemonSessionState();
    // The ONE construction site of the editor's when-context. `editorUi` is still the baseline (the
    // real bus is e08c's; wiring it across windows is e10's), so ONLY the session half moved here.
    const whenContext = (): WhenContext =>
        resolveContext({ editorUi: STUB_EDITOR_UI, session });
    const report = (detail: string): void => {
        if (typeof document !== "undefined") {
            document.documentElement.setAttribute(SESSION_ATTRIBUTE, detail);
        }
    };
    try {
        const feed = new SessionFeed(bridge, session, defaultSessionScheduler(), onRead);
        const first = await feed.refresh();
        // Reported from the PROVIDER, not from the read: what a `when` clause would actually see.
        report(`${describeSessionRead(first)}; when playState "${String(whenContext().playState)}"`);
        if (first.served) {
            // Only poll a Shell that answers. An older one refuses identically every tick, and a
            // refusal per tick for the life of the window is a cost with no possible payoff.
            feed.start();
        }
    } catch (error) {
        report(
            `session feed unavailable: ${error instanceof Error ? error.message : String(error)}`,
        );
    }
    return { whenContext, session };
}

/**
 * Mount the play-bar strip into the a2 slot and build the `play.*` command actions (d1).
 *
 * NEVER fatal, like every boot feed: a document without the slot (an older served page, a bare
 * harness) mounts nothing — reported by the ABSENCE of `data-editor-playbar` — and the returned
 * actions still work, because the commands write over `session.control` regardless of whether the
 * strip painted (the palette must not lose its transport because a strip did).
 */
function startPlaybar(
    bridge: ShellBridge,
    session: DaemonSessionState,
    holder: PlaybarHolder,
    liveRegistry: { current: CommandRegistry | undefined },
): PlayCommandActions {
    const actions = makePlayActions(new SessionControlClient(bridge), session, holder);
    try {
        const slot =
            typeof document === "undefined" ? null : document.getElementById(EDITOR_PLAYBAR_ID);
        if (slot !== null) {
            holder.current = mountPlaybar(slot, {
                // The same late-bound dispatch the titlebar's palette button uses (a2): before the
                // command layer is up the press is an honest no-op, never a throw.
                executeCommand: (commandId: string): void => {
                    void liveRegistry.current?.execute(commandId);
                },
            });
            // Seed from the sink's current truth (the boot read already landed in startSession).
            holder.current.applySession(session.playState, session.simTick);
        }
    } catch {
        // Reported by the absence of `data-editor-playbar`; the editor is up and usable.
    }
    return actions;
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

/** The <html> attribute the e10b window mechanism reports its state on (for --dump-dom + the smoke). */
export const WINDOW_ATTRIBUTE = "data-editor-window";

/** The <html> attribute the e10d-drill2 cross-window mirror reports its state on (for --dump-dom). */
export const UI_MIRROR_ATTRIBUTE = "data-editor-uimirror";

/**
 * The <html> attribute the e09b-3 notification host reports on (for --dump-dom + a live smoke).
 *
 * It exists for the same reason every other `data-editor-*` boot report does: the renderer's only
 * out-of-band diagnostic channel is the document. Here it also answers the one question a screenshot
 * cannot — a toast the human dismissed leaves no pixels, but `shown` is cumulative, so "the editor
 * DID tell them" stays observable after the evidence is off screen.
 */
export const NOTICES_ATTRIBUTE = "data-editor-notices";

/**
 * The e10d-drill2 mirror-smoke boot flag — a NO-OP unless the boot URL carries it. Under it, window 0
 * (the publisher) re-publishes an `editor.ui` fact on every poll tick, so once the SECOND window is up
 * and polling the broadcast reaches it. Every other window just drains + applies + reports, exactly as
 * the shipping poll path does. Inert in the shipping editor (no flag ⇒ nothing publishes on the bus).
 */
const UI_MIRROR_SMOKE_FLAG = "ctx-smoke-uimirror";

/**
 * The Shell-hosted session-undo panel (M9 e09c) — the id `session.undo` / `session.redo` dispatch to.
 *
 * MIRRORS `UndoJournal::kContributionId` (undo_journal.h) and the `builtin.session.undo` roster entry
 * (builtin_roster.cpp), exactly as `SETTINGS_PANEL_ID` mirrors its own. Exported so the T1 tier pins
 * the spelling: a drift here does not fail any build — it silently makes Ctrl+Z dispatch to a panel
 * that does not exist, and the only symptom is an undo that quietly answers `not dispatched`.
 */
export const SESSION_UNDO_PANEL_ID = "builtin.session.undo";

/** How often a window polls for panels moved INTO it (move-to-N + a peer's window-close rehome). */
const REHOME_POLL_MS = 500;

/** Mirror the window mechanism's state onto <html> for a --dump-dom repro and the live smoke. */
function reportWindow(detail: string): void {
    if (typeof document !== "undefined") {
        document.documentElement.setAttribute(WINDOW_ATTRIBUTE, detail);
    }
}

/**
 * A FIRE-ON-CHANGE `<html>` attribute reporter, for the poll-driven diagnostics.
 *
 * The runtime poll calls these every tick, but their detail strings only move when the thing they
 * describe does — so an idle shipping window writes each attribute once and never touches the DOM
 * again. Written as a factory rather than twice over because the memo has to be per-attribute state,
 * and two copies of it meant two module-level `let`s drifting beside two identical functions.
 *
 * NOTE the honest scope of "fire-on-change": the DOM WRITE is gated, the caller's string CONCATENATION
 * is not — the detail is built at the call site before the comparison can happen. At 2 Hz that is
 * microseconds and not worth restructuring the call sites for, but it is not literally "no per-tick
 * work", and claiming so would mislead the next person profiling this loop.
 */
function makeChangeReporter(attribute: string): (detail: string) => void {
    let last: string | undefined;
    return (detail: string): void => {
        if (typeof document !== "undefined" && detail !== last) {
            last = detail;
            document.documentElement.setAttribute(attribute, detail);
        }
    };
}

/** The e10d-drill2 cross-window mirror's convergence, for a --dump-dom repro and the live smoke. */
const reportUiMirror = makeChangeReporter(UI_MIRROR_ATTRIBUTE);

/** The e09b-3 notification host's cumulative tally — "the editor DID tell them", after the pixels. */
const reportNotices = makeChangeReporter(NOTICES_ATTRIBUTE);

/**
 * Open + restore every panel that has rehomed INTO this window (M9 e10b) — the RUNTIME half of the
 * D6 recreate path (the boot seed is the boot half). Each seed is `openById` + `panel.state.set` +
 * `refreshAll`, exactly the seed-open in `startPanels`, so rehome and tear-out demonstrably use the
 * SAME mechanism (a DoD line). Best-effort and total: a panel this build cannot host is skipped.
 */
async function applyRehomedPanels(
    host: PanelHost,
    client: PanelClient,
    seeds: readonly WindowSeed[],
): Promise<number> {
    let applied = 0;
    for (const seed of seeds) {
        if (host.openById(seed.panelId)) {
            await client.setState(seed.panelId, seed.state);
            applied += 1;
        }
    }
    if (applied > 0) {
        await host.refreshAll();
    }
    return applied;
}

/**
 * Wire the cross-window MOVE machinery for THIS window (M9 e10b). Returns this window's id.
 *
 * Two channels, both over the SAME D6 relay as tear-out:
 *   * the REHOME POLL — a light interval draining `window.rehomed`, so a panel moved to this window
 *     (a "move to window N", or a closing peer's rehome) opens here at runtime;
 *   * the PAGEHIDE REHOME — a NON-PRIMARY window, on close, moves every open panel to window 0, so
 *     "close a window with panels ⇒ they rehome, never silently lost". The primary never does this:
 *     it hosts the app menu / welcome screen (D13) and is the rehome TARGET, not a source.
 *
 * NEVER throws: a Shell with no window surface (an older build, or a smoke that did not install it)
 * leaves the window on the default single-window behaviour, reported on `<html data-editor-window>`.
 */
async function startWindowMechanism(
    windowClient: WindowClient,
    dragClient: DragClient,
    host: PanelHost,
    client: PanelClient,
    bridge: ShellBridge,
    // M9 e13c-4: called with this window's `editor.ui` bus once it exists, so `startPanels` — which
    // built the verb tables BEFORE the bus did — can fill the fan-out holder they closed over. A
    // CALLBACK rather than a returned value, because the bus is created inside a `try` that may never
    // reach the end, and a fan-out attached only on the happy path is one a caller cannot reason about.
    attachUiFanout: (bus: EditorUiBus) => void,
    // a2: the mounted titlebar (undefined when the document has no strips). Subscribed here — not
    // at mount — because the `editor.ui.chrome` maximized fact arrives over THIS window's bus,
    // which only exists once this mechanism is up, and because the subscription filter needs the
    // window id `window.list` reports. On the welcome path (no window mechanism) the glyph simply
    // keeps the boot snapshot, which is the honest state of a window nothing is relaying to.
    chromeMount?: ChromeMount,
): Promise<number> {
    let windowId = 0;
    let detail = "single window";
    // e10c: a cross-window drag targeting THIS window drops anywhere in its viewport (a rehome via the
    // e10b move path — NOT Dockview's own drop, so in-window docking is untouched). `<html>` bounds the
    // window; a null document (a non-DOM smoke) answers "no zone" honestly.
    const dropZoneHitTest = makeDropZoneHitTest(() =>
        typeof document !== "undefined" ? document.documentElement : null,
    );
    try {
        const list = await windowClient.list();
        windowId = list?.windowId ?? 0;
        // Drain anything already queued (a move that landed before this window finished booting).
        const applied = await applyRehomedPanels(host, client, await windowClient.rehomed());
        // e10d-drill2: bring up the cross-window `editor.ui` MIRROR. THREAD THIS WINDOW'S ID INTO THE
        // BUS ORIGIN — that is what lets a mirrored envelope be told apart from a locally-published one,
        // so `receiveMirrored` drops this window's OWN fact when the Shell's broadcast delivers it back
        // (the echo-suppression branch a broadcasting transport needs). `wireUiMirror` attaches the
        // ShellUiMirrorSink (outbound) and returns the inbound poller. Nothing publishes on the bus in
        // the shipping editor yet (the focus/layout/palette publishers are later seams), so the
        // transport is live and ready but idle — the mirror smoke drives the one fact that exercises it.
        let uiMirror: UiMirrorWiring | null = null;
        let uiBus: EditorUiBus | null = null;
        let notifications: NotificationHost | null = null;
        if (list !== null) {
            uiBus = new EditorUiBus({ origin: String(windowId) });
            uiMirror = wireUiMirror(bridge, uiBus);
            // M9 e13c-4 — THE `editor.ui` FAN-OUT TO GRANTED PACKAGE PANELS. Filled into the holder
            // the verb tables closed over above; until this line every `bridge.ui.subscribe` refuses
            // with "this window has no editor.ui bus", which is the honest welcome-screen state and
            // is also why the holder is `undefined` rather than a no-op object.
            //
            // ⚠ IT SHARES THE PUSH PATH WITH e13c-2, deliberately: `deliverToPackage` is the same
            // method `PackageEventPump` pushes daemon batches through, so a package's two streams
            // arrive over ONE port with one lifetime and one failure mode. Only the VERB differs.
            attachUiFanout(uiBus);
            // a2: flip the titlebar's max/restore glyph on the `editor.ui.chrome` maximized fact
            // (02 §1) — the Shell's placement poll observes the flip and unicasts the envelope to
            // the affected window over the ui.mirror relay; the poll below drains it onto this bus.
            if (chromeMount !== undefined) {
                subscribeChromeFacts(uiBus, windowId, chromeMount);
            }
            // M9 e09b-3 — THE LOUD SURFACE. The editor's ONE notification host, attached to this
            // window's bus. Every refused write the Shell publishes (an L-30 drop, a write-path
            // refusal) arrives over the mirror poll below, lands on the bus, and is rendered here as
            // a wait/bad-hued, assertively-announced toast (design 05 §8 / 10 § UX invariants).
            //
            // ATTACHED BEFORE THE POLL LOOP STARTS, deliberately: a notice already queued Shell-side
            // when this window booted is drained by the FIRST tick, and a host created after that tick
            // would miss it. (The bus's snapshot-on-subscribe would still deliver a retained one, but
            // relying on that ordering to be safe is how it stops being safe.)
            //
            // Mounted on <body>, not into #editor-root: `.ctx-toast-region` is a fixed-position
            // overlay (components.css), so it costs the docking root no layout — the same reasoning
            // index.html records for the banner strip, which must not change the box Dockview
            // measures or the live smokes' per-pixel background floor.
            //
            // The mount is handed to the HOST rather than appended here, because it must happen before
            // the host subscribes (see createNotificationHost); and it is CONSTRUCTED inside the
            // document guard rather than beside it, because building a toast region touches
            // `document.createElement` — a throw there is swallowed by this function's outer catch and
            // would silently take the rehome poll, the cross-window drag pump and the whole ui.mirror
            // poll down with it, reporting nothing but a string on data-editor-window.
            if (typeof document !== "undefined") {
                notifications = createNotificationHost(uiBus, { mount: document.body });
            }
        }
        // M9 e13c-2 — THE DAEMON EVENT FAN-OUT. Drains each mounted package's BOUNDED Shell-side
        // buffer and pushes the batch into that package's frames over the e13b-1 ports. Built here
        // (rather than inside the tick) so its running totals survive across ticks and can be read by
        // a smoke; the host is asked for its packages EVERY tick, so a panel opened later is picked up
        // with no re-wiring.
        const packageEvents = new PackageEventPump(bridge, {
            packages: (): readonly string[] => host.packagesWithPorts(),
            deliver: (packageId: string, batch: PackageEventBatch): number =>
                host.deliverToPackage(packageId, PANEL_EVENTS_DELIVER_VERB, batch).delivered,
        });
        const uiMirrorSmoke =
            typeof location !== "undefined" && location.search.includes(UI_MIRROR_SMOKE_FLAG);
        // The runtime poll: cheap, and started ONLY when the Shell actually serves the surface. It
        // drains the rehome queue (e10b), the cross-window drag probe (e10c) — the drag pump does
        // nothing unless a drag from another window is currently over this one — and the `editor.ui`
        // mirror (e10d-drill2), whose poll drains a peer's chrome facts and reports convergence.
        if (list !== null && typeof setInterval === "function") {
            setInterval((): void => {
                // M9 e09e-3 — THE MODEL-CHANGE REFRESH DRIVER, and design 05 §8's tail on the only
                // surface a human looks at. Every other re-render in this build is driven by a LOCAL
                // interaction (mount, becoming visible, a command this window sent), so a fact
                // arriving from the DAEMON moved the C++ panel model and changed NOTHING on screen
                // until the user next clicked — worst in a secondary window, which sends no commands
                // at all. `pollRevisions` costs ONE `panel.list` round trip per tick (the revision
                // rides the roster; nothing is built Shell-side) and issues a `panel.render` only for
                // a panel whose model actually moved — see `PanelHost.pollRevisions` on why the
                // unconditional `refreshAll` must NOT be what a tick calls.
                void host.pollRevisions();
                void windowClient.rehomed().then((seeds) => {
                    if (seeds.length > 0) {
                        void applyRehomedPanels(host, client, seeds);
                    }
                });
                void pumpCrossWindowDrag(dragClient, dropZoneHitTest);
                // e13c-2: drain each mounted package's daemon-event buffer and push it into its
                // frames. COSTS NOTHING when no package panel is mounted — the pump returns before
                // it calls the Shell at all, so the shipping editor's tick is unchanged until a
                // third-party panel is actually open.
                void packageEvents.poll();
                // Under the mirror-smoke flag, WINDOW 0 (the publisher) re-publishes on every tick so
                // the broadcast reaches the second window once it is up and polling; every window then
                // drains + applies + reports. Inert without the flag — the shipping editor publishes
                // nothing here, so the poll is an empty round trip and no report is ever sent.
                if (uiBus !== null && uiMirrorSmoke && windowId === 0) {
                    uiBus.publish(UI_TOPIC_THEME_CHANGED, { variant: "dark" });
                }
                if (uiMirror !== null) {
                    const poller = uiMirror.poller;
                    const notices = notifications;
                    void poller.poll().then(() => {
                        reportUiMirror(
                            `window ${windowId} (origin "${windowId}"): applied ${poller.applied}, ` +
                                `suppressed ${poller.suppressed}`,
                        );
                        // e09b-3: the drained batch has already been applied to the bus and, for a
                        // write notice, already rendered — the subscription is synchronous. So this
                        // reports a settled tally, never a pending one.
                        if (notices !== null) {
                            reportNotices(
                                `shown ${notices.shown}; on screen ${notices.onScreen}` +
                                    (notices.last === null ? "" : `; last ${notices.last.kind}`),
                            );
                        }
                        return poller.report();
                    });
                }
            }, REHOME_POLL_MS);
        }
        // A non-primary window rehomes its panels to window 0 when it closes, so no PANEL is lost —
        // and, since M9 e13d, no longer "nothing": a package panel's port blob does not travel this
        // relay (see `tearOutActivePanel` for why), so it rehomes with its state cleared.
        if (list !== null && windowId !== 0 && typeof window !== "undefined") {
            window.addEventListener("pagehide", (): void => {
                for (const id of [...host.mounted]) {
                    void client.getState(id).then((state) => {
                        void windowClient.moveTo(id, state, 0);
                    });
                }
            });
        }
        detail =
            windowId === 0
                ? `window 0 (primary); ${applied} rehomed at boot`
                : `window ${windowId} (secondary); rehome-on-close armed; ${applied} rehomed at boot`;
    } catch (error) {
        detail = `window mechanism unavailable: ${error instanceof Error ? error.message : String(error)}`;
    }
    reportWindow(detail);
    return windowId;
}

/**
 * Wire the D8 command layer (e07d): build the ONE registry from all three sources, create the palette
 * over it, register the palette-open command, mount the palette overlay, and — only under the
 * `?ctx-smoke-palette` boot flag — drive the T2 command-driven scenario.
 *
 * NEVER THROWS: like the rest of boot, a failure here degrades to "no command layer" rather than an
 * unhandled rejection in a renderer nobody watches. Placed after `PanelHost.start()` so the roster is
 * known and after `LayoutPersistence.attach()` so a palette-driven layout change actually publishes.
 *
 * ⚠ THE "NEVER THROWS" CONTRACT IS NOT WHAT MADE A DUPLICATE COMMAND ID FATAL — the throw was, and it
 * has been fixed UPSTREAM of this catch (commands.ts § buildCommandRegistry). Degrading a genuine
 * boot failure to "no command layer" is right; degrading ONE colliding id from ONE package to it was
 * not, and deleting the catch would have traded a silent palette outage for an unhandled rejection.
 * The refusals are reported below instead.
 *
 * RETURNS THE REGISTRY (M9 e13b-2) so `startPanels` can hand it to the package-panel verb tables —
 * `undefined` when the layer did not come up, which those tables refuse honestly on.
 */
function startCommandLayer(
    host: PanelHost,
    client: PanelClient,
    windowClient: WindowClient,
    theme: ThemeEngine | undefined,
    whenContext: () => WhenContext,
    playActions: PlayCommandActions,
): CommandRegistry | undefined {
    if (typeof document === "undefined") {
        return undefined;
    }
    const roster = host.roster;
    if (roster === null) {
        return undefined;
    }
    const dispatchPanelCommand = makePanelDispatch(client, host);
    try {
        const registry = buildCommandRegistry({
            // The daemon RPC fan-in (D19) is a later seam; contract verbs are PROJECTED into the
            // palette (with their introspected docs) now, and executing one is an honest refusal until
            // the client fan-in lands — it never touches the bridge, so no boot-time refusal.
            contractDispatch: (method): CommandOutcome => ({
                ok: false,
                note: `daemon RPC fan-in not wired yet (D19): ${method}`,
            }),
            editorActions: makeEditorActions(host, client, windowClient, theme),
            // M9 e09c — session undo/redo are REAL now (see `makeSessionActions` below for what
            // e07c left behind and why the wiring is a named factory). Two facts are local to THIS
            // call site:
            //
            // NOT a duplicate registration: `panel.list` projects a panel's MANIFEST commands, which
            // are empty for every built-in uitree panel (panel_host.cpp § list) — the journal's
            // `session.undo` / `session.redo` ride its RENDER instead. So `projectPanelCommands`
            // contributes nothing here and these two ids stay the registry's only ones. Since e13b-2
            // that is a tidiness fact, no longer a load-bearing one: `buildCommandRegistry` NO LONGER
            // THROWS on a collision (commands.ts § buildCommandRegistry), and the session source is
            // registered BEFORE the panel source, so a manifest colliding on either id would now cost
            // exactly that manifest command and leave the palette whole.
            //
            // And `dispatched:false` is the honest outcome for all three of nothing-to-undo, a loud
            // drop, and a refused replay — in every one the file was left exactly as it was.
            sessionActions: makeSessionActions(dispatchPanelCommand),
            // d1: the four `play.*` transports, writing over `session.control` (startPlaybar built
            // them) — registered BEFORE the panel source, so incumbent-wins protects the ids.
            playActions,
            roster,
            // A panel-manifest command dispatches to its panel over the real `panel.command` bridge.
            panelDispatch: dispatchPanelCommand,
        });

        const palette = new Palette(registry);
        const view = new PaletteView({
            host: document.body,
            palette,
            // The when-context the palette filters on. Its SESSION half is live daemon truth since
            // e08d (`startSession`); its `editor.ui` half is still the "nothing focused" baseline —
            // the real bus is e08c's, and wiring it is e10's cross-window seam. One provider, built
            // once in `bootEditorCore`, so there is exactly one place either half can be swapped.
            contextProvider: whenContext,
        });
        view.mount();
        // Register the palette-open command AFTER the view exists, so its handler can reflect the model
        // into the overlay. It is bound to Ctrl+Shift+P in the default keymap (keymap.ts).
        //
        // ⚠ EVICT ANY SQUATTER FIRST — this is the ONE built-in that registers AFTER the panel source,
        // so it is the ONE the assembly order does not already protect. Every other editor id is
        // registered inside `buildCommandRegistry` BEFORE `projectPanelCommands`, where incumbent-wins
        // makes the editor the incumbent; this one runs after it, so a package manifest declaring
        // `workbench.palette.toggle` would be the incumbent and OUR registration would be the one
        // refused. That is not "costing us this one command": Ctrl+Shift+P resolves through the same
        // registry (keymap.ts), and the palette has no other opener — the chord would dispatch into
        // the package and the editor's universal keyboard surface (R-A11Y-001) would be gone.
        // `unregister` is the withdrawal primitive e13b-2 added, and this is its second real caller.
        const squatter = claimPaletteToggle(
            registry,
            paletteCommands({
                toggle: (): CommandOutcome => {
                    palette.toggle();
                    view.sync();
                    return { ok: true, note: palette.isOpen ? "palette opened" : "palette closed" };
                },
            }),
        );

        // ATTRIBUTION, mirrored onto <html> like every other boot state. A refused registration is
        // now survivable, which is exactly why it must be VISIBLE: the old failure took the whole
        // palette down and said "duplicate command id", and the new one silently drops one command
        // unless the collision is reported with the id and both of its sources.
        // The eviction above is reported for the same reason a refusal is: a package that tried to
        // take the palette's own id is exactly the event an operator must be able to see, and an
        // eviction that only ever showed up as "the package's command is missing" would be the silent
        // failure this attribution channel exists to end.
        const evicted =
            squatter === undefined
                ? ""
                : `; EVICTED a squatter on "${PALETTE_TOGGLE_COMMAND_ID}" (${squatter.title})`;
        reportCommands(
            registry.rejections.length === 0
                ? `ready; ${String(registry.size)} commands${evicted}`
                : `ready; ${String(registry.size)} commands${evicted}; ` +
                      `${String(registry.rejections.length)} REFUSED: ` +
                      registry.rejections.map((rejection) => rejection.diagnostic).join(" | "),
        );

        void runPaletteSmoke(registry, palette, host, view);
        return registry;
    } catch (error) {
        // Honest degradation, mirrored onto <html> like the other boot states.
        reportCommands(
            `command layer unavailable: ${error instanceof Error ? error.message : String(error)}`,
        );
        return undefined;
    }
}

/**
 * Register the palette's own commands, EVICTING anyone already holding the toggle id.
 * Returns the evicted command, or `undefined` when nobody held it (the normal case).
 *
 * ⚠ THE ONE BUILT-IN THAT THE ASSEMBLY ORDER DOES NOT PROTECT. Every other editor command is
 * registered inside `buildCommandRegistry` BEFORE `projectPanelCommands`, so incumbent-wins makes the
 * editor the incumbent and a colliding manifest id loses. The palette's toggle is registered AFTER
 * the whole registry is built (its handler needs the view, which needs the registry), so the order
 * runs the other way: a package manifest declaring `workbench.palette.toggle` would be the incumbent
 * and the EDITOR's registration would be the one refused.
 *
 * That is not "costing us one command". Ctrl+Shift+P resolves through this same registry (keymap.ts)
 * and the palette has no other opener, so the chord would dispatch into the package and the editor's
 * universal keyboard surface (R-A11Y-001) would be gone for the life of the window. Evicting first
 * makes the editor's claim unconditional.
 *
 * Extracted and exported so the invariant is assertable at T1: `startCommandLayer` needs a live DOM,
 * a host and a client, and an invariant only provable through a full boot is one a regression can
 * quietly take away.
 */
export function claimPaletteToggle(
    registry: CommandRegistry,
    commands: readonly Command[],
): Command | undefined {
    const squatter = registry.get(PALETTE_TOGGLE_COMMAND_ID);
    if (squatter !== undefined) {
        registry.unregister(PALETTE_TOGGLE_COMMAND_ID);
    }
    registry.tryRegisterAll(commands);
    return squatter;
}

/** Mirror the command layer's outcome onto `<html data-editor-commands>`. */
function reportCommands(detail: string): void {
    if (typeof document !== "undefined") {
        document.documentElement.setAttribute("data-editor-commands", detail);
    }
}

/**
 * The ONE dispatcher for every command that resolves to a panel — the manifest-projected ones AND
 * (since M9 e09c) the session undo/redo pair.
 *
 * Shared rather than written twice so the two cannot drift into different notions of "dispatched":
 * a panel command that reported success on a `dispatched:false` reply would make an undo that did
 * nothing look like one that worked.
 *
 * ⚠ TWO ROUTES SINCE M9 e13b-2, AND THE SECOND ONE FIXES A DEAD END. A `uitree` panel's command goes
 * over `panel.command` to its C++ model, as it always has. A THIRD-PARTY (`iframe`) panel has no C++
 * model at all, so that route answered `dispatched:false` for every manifest command a package
 * declared — the command appeared in the palette, was bound by the keymap, and could never do
 * anything. When `host` is supplied and the panel has a live port, the command is delivered to the
 * PACKAGE instead (`commands.invoke`, panelverbs.ts), which is the direction e13b-1 built
 * `PanelPortBridge.request` for and named e13b-2 as the first consumer of.
 *
 * The fallback is deliberate rather than defensive, and it covers EXACTLY TWO cases: no `host`, or a
 * panel that is not a mounted `iframe` renderer (`uitree` / `local` / unavailable). Those resolve back
 * to the `panel.command` route, whose honest `dispatched:false` is the same answer as before.
 *
 * A mounted package panel with NO live port does NOT fall back — `PanelHost.portRequest` returns a
 * closure for any `IframePanelRenderer` regardless of port state, and the renderer answers
 * `bridge.port_unavailable` / `bridge.port_revoked` itself. That is the honest outcome, not a
 * degradation: `panel.command` has no C++ model to answer for an iframe panel either, so re-routing
 * there would only relabel the same failure.
 *
 * Exported so the T1 tier can drive THIS function rather than a copy of it (`commands.test.ts`).
 */
export function makePanelDispatch(
    client: PanelClient,
    host?: PanelHost,
): (panelId: string, commandId: string) => Promise<CommandOutcome> {
    return async (panelId: string, commandId: string): Promise<CommandOutcome> => {
        const port = host?.portRequest(panelId);
        if (port !== undefined) {
            const reply = await port(PANEL_VERB_COMMAND_INVOKE, { id: commandId });
            return reply.ok
                ? { ok: true, note: `${panelId}/${commandId}` }
                : {
                      ok: false,
                      note: `${panelId}/${commandId} not dispatched (${
                          reply.error?.code ?? "no reply"
                      })`,
                  };
        }
        const result = await client.command(panelId, commandId, "");
        return result !== null && result.dispatched
            ? { ok: true, note: `${panelId}/${commandId}` }
            : { ok: false, note: `${panelId}/${commandId} not dispatched` };
    };
}

/**
 * The Shell method the package daemon fan-in lands on (M9 e13c-1).
 *
 * MIRRORS C++ `kPanelDaemonCallMethod` (package_sessions.h). Kept beside its one caller rather than
 * in `panelverbs.js`, which is transport-free by construction and must stay so.
 */
export const PANEL_DAEMON_CALL_METHOD = "panel.daemon.call";

/**
 * Bind ONE package's daemon fan-in (M9 e13c-1, design 04 §5 / 08 §2).
 *
 * ⚠ `packageId` IS AN ARGUMENT OF THIS FACTORY, NOT OF THE RETURNED FUNCTION. That single fact is the
 * cross-package boundary at this layer: the returned `PanelDaemonCall` takes only a method and its
 * params, so `bridge.call`'s handler has nothing to pass that could name another package, and a
 * `packageId` in a panel's request payload reaches no code that reads one. Same shape as the
 * per-panel `PanelStateStore` (panelverbs.ts § the file header), and for the same reason: every
 * package reports `event.origin === "null"`, so a closure is the only thing that can carry identity.
 *
 * ⚠ NO ALLOWLIST HERE. The panel-callable method set lives at the SHELL, which holds the session
 * (package_sessions.h § control 2). This function forwards the method verbatim and relays the answer.
 *
 * TOTAL — it never rejects, because `bridge.call`'s handler must be able to turn every outcome into a
 * `PanelVerbRefusal`; an escaping rejection would reach `PanelPortBridge.#invoke`'s generic host-fault
 * path and tell the package nothing, `scope.denied` least of all. A `BridgeError` carries the Shell's
 * machine-readable `reason` (the handler's own `error_code`, e.g. `scope.denied`), which is exactly
 * what the panel-facing refusal relays; anything else is a transport fault with no code to relay.
 */
export function makePackageDaemonCall(bridge: ShellBridge, packageId: string): PanelDaemonCall {
    return async (method: string, params: unknown): Promise<PanelDaemonOutcome> => {
        try {
            const result = await bridge.call(PANEL_DAEMON_CALL_METHOD, {
                packageId,
                method,
                params,
            });
            return { ok: true, result };
        } catch (error) {
            if (error instanceof BridgeError) {
                return { ok: false, code: error.reason, message: error.message };
            }
            // Not a refusal the Shell authored — a client-side parse/shape fault. Its message is
            // renderer state, so it is NOT echoed (the discipline `PanelPortBridge.#invoke` applies
            // to an ordinary throw); the code says what class it is and nothing more.
            return { ok: false, code: "bridge.transport", message: "the Shell could not be reached" };
        }
    };
}

/**
 * The M9 e09c session actions: undo/redo dispatched at the Shell's session-undo host.
 *
 * A named factory rather than an object literal at the call site, mirroring `makeEditorActions`
 * below — and for a reason beyond symmetry. e07c left these an honest refusal ("the wire replay
 * lands in e09") and e09c replaced it; a test that builds its OWN actions object cannot tell the
 * two apart, so reverting to the stub would leave every TS test green. Driving this function is
 * what makes that revert fail.
 */
export function makeSessionActions(
    dispatch: (panelId: string, commandId: string) => Promise<CommandOutcome>,
): SessionCommandActions {
    return {
        undo: (): Promise<CommandOutcome> => dispatch(SESSION_UNDO_PANEL_ID, "session.undo"),
        redo: (): Promise<CommandOutcome> => dispatch(SESSION_UNDO_PANEL_ID, "session.redo"),
    };
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
function makeEditorActions(
    host: PanelHost,
    client: PanelClient,
    windowClient: WindowClient,
    theme: ThemeEngine | undefined,
): EditorCommandActions {
    // The "active" panel is the last-mounted one, mirroring `closeActivePanel` above — real focus
    // tracking is the 03 §6 input-pump seam; until it lands this is the deterministic stand-in the
    // tear-out / move commands share with close.
    const activePanel = (): string | undefined => {
        const mounted = host.mounted;
        return mounted.length > 0 ? mounted[mounted.length - 1] : undefined;
    };
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
        // e10b: tear the active panel out into a NEW window over the D6 relay (serialize -> create +
        // seed -> the new window's editor-core recreates + restores). On success the panel is REMOVED
        // here (the D6 destroy step). On a create FAILURE it degrades LOUDLY (03 §7) — the panel is
        // floated (a visible floating group) and the reason is reported, never a silent no-op.
        tearOutActivePanel: async (): Promise<CommandOutcome> => {
            const active = activePanel();
            if (active === undefined) {
                return { ok: false, note: "no active panel to tear out" };
            }
            // ⚠ THE C++ ROUTE ONLY, AND THEREFORE NOT A PACKAGE PANEL'S BLOB (M9 e13d). An `iframe`
            // panel has no model, so `panel.state.get` answers `null` for it and the panel is relayed
            // with empty state — a real loss, silent today. e13d wired the PORT store
            // (`host.portState` / `host.seedPortState`) into `LayoutPersistence` ONLY, which is the
            // RELOAD round trip it owns; the cross-window relay is e10b's mechanism.
            //
            // ⚠ AND THE REASON IS SCOPE, NOT IMPOSSIBILITY — said plainly, because "it could not be
            // done here" would be false and would stop the next reader from doing it. All FIVE sites
            // could take the port route: the two CAPTURE sites (here and `movePanelToPrimary`) run in
            // the window where the panel is mounted, and the two APPLY sites (`applyRehomedPanels`
            // and the boot seed, both above) already `openById`/`start` BEFORE they `setState`, so
            // the target's store exists by the time they would seed it. What is missing is a drill
            // that proves a blob survives a real two-window relay — which is e10's kind of test, not
            // this task's — plus the `pagehide` rehome handler, whose payload would then have to be
            // gathered while the window is closing. Left as ONE coherent piece of follow-up rather
            // than three-fifths wired: a half-wired relay loses state on whichever path was skipped
            // and looks like it works on the others.
            const state = await client.getState(active);
            const result = await windowClient.tearOut(active, state);
            if (result.created) {
                host.close(active); // the panel now lives in the new window — recreate + destroy, D6
                reportWindow(`torn out ${active} to window ${result.windowId}`);
                return { ok: true, note: `torn out ${active} to window ${result.windowId}` };
            }
            const floated = host.floatPanel(active);
            reportWindow(
                `tear-out FAILED (${result.outcome}: ${result.error}); ` +
                    `${floated ? "floated" : "kept"} ${active} in this window`,
            );
            return {
                ok: false,
                note: `tear-out failed (${result.outcome}); ${floated ? "floated" : "kept"} ${active} loudly`,
            };
        },
        // e10b: move the active panel to the MAIN window (window 0) over the SAME relay. The target
        // opens it on its rehome poll; on success the panel is removed here.
        movePanelToPrimary: async (): Promise<CommandOutcome> => {
            const active = activePanel();
            if (active === undefined) {
                return { ok: false, note: "no active panel to move" };
            }
            // Same C++-route-only limit as `tearOutActivePanel` above: a package panel's port blob
            // does not travel this relay (M9 e13d).
            const state = await client.getState(active);
            const result = await windowClient.moveTo(active, state, 0);
            if (result.moved) {
                host.close(active);
                return { ok: true, note: `moved ${active} to window 0` };
            }
            return { ok: false, note: `move to window 0 failed: ${result.error}` };
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
    // e14d: the panel's Updates tab. Its own client rather than a threaded-in snapshot, because the
    // panel can materialise long after boot (Dockview creates it lazily) and the check completes on a
    // Shell worker thread — so the tab asks when it MOUNTS, and reports the answer into the live mount.
    const bannerClient = new BannerClient(bridge);
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
            onOpenDownloads: (): void => {
                void bannerClient.openDownloads();
            },
            onDismissUpdate: (): void => {
                void bannerClient.dismissUpdate();
            },
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
        // Ask the Shell what it knows about updates and fill the tab in when the answer lands. A
        // Shell with no banner surface answers `null`, which renders the honest "no update channel"
        // empty state — the same thing the tab showed before e14d.
        void bannerClient.updateState().then((state) => {
            mount.reportUpdate(state);
        });
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
