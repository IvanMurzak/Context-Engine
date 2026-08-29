// The STATUSBAR strip content (editor-window-chrome d2, target design 02 §8): daemon link state,
// problems count, and active theme + project identity, rendered into the a2 shell
// (`#editor-statusbar`). v1 content is exactly what already has a truthful source — anything else
// waits for its source (02 §8's honesty rule); a field whose source is absent HIDES rather than
// invents.
//
// Three facts are load-bearing:
//
//   1. EVERY FIELD RENDERS AN EXISTING FEED — no new bridge surface. The link field renders the
//      SAME `daemon.linkState` read the banners consume (banners.ts `BannerClient`), seeded from
//      boot's one fetch and kept live by `StatusbarLinkFeed` below (a poll, like every
//      Shell→renderer fact — the e05c bridge accepts no persistent queries; packageevents.ts owns
//      that rationale). The theme field rides the RETAINED `editor.ui.theme-changed` envelope
//      (uibus.ts snapshot-on-subscribe), so subscribing after boot's theme apply is handed the
//      current theme immediately. The project field is the SAME `welcome.state` projectName the
//      titlebar renders, with the same fallback (chrome.ts DEFAULT_TITLE).
//
//   2. THE PROBLEMS COUNT IS DERIVED FROM THE HYDRATED PROBLEMS PANEL, editor-core's only view of
//      the diagnostics feed. The C++ `ProblemsPanel` (problems_panel.cpp) receives the daemon's
//      `diagnostics` topic Shell-side and renders ONE row per diagnostic under grep-stable node ids
//      (`problems.list` / `problems.row.N` — mirrored by the constants below, exactly as chrome.ts
//      mirrors its region ids); hydration mounts that markup, and the model-change refresh driver
//      (panelhost.ts `pollRevisions`) re-renders it whenever a diagnostic lands. Counting those
//      rows is therefore counting the model's diagnostics, one poll tick behind at worst — and when
//      the panel is NOT mounted (closed, or the welcome screen) there is honestly NO count in this
//      window, so the field hides instead of showing a number nothing backs.
//
//   3. THE UNCHANGED SHORT-CIRCUITS ARE LOAD-BEARING, not an optimisation (the playbar's rule): the
//      link and problems fields are `aria-live="polite"` kit badges, re-applied on every poll tick,
//      and a live region whose text node is replaced re-announces even when the text is identical —
//      an unguarded re-render would have a screen reader repeating "Live" for the life of the
//      window.
//
// DOM ONLY, no `innerHTML`, exactly like banners.ts / chrome.ts / playbar.ts: every node is built
// with `createElement` + `textContent`, so a project name or theme name off the wire can never
// inject markup into the trusted zone. Styled in app.css from existing tokens; the live readouts
// are kit components (`createBadge`); no new kit family, no new tokens.

import { createBadge, TONE_ATTRIBUTE, type KitBadge, type SemanticTone } from "../../kit/src/index.js";
import { DAEMON_OWNERSHIP_OWNED, type DaemonLinkState } from "./banners.js";
import { DEFAULT_TITLE } from "./chrome.js";
import { NODE_ID_ATTRIBUTE } from "./hydration.js";
import { defaultSessionScheduler, type SessionScheduler } from "./session.js";
import type { ThemeChangedPayload } from "./theme.js";
import { UI_TOPIC_THEME_CHANGED, type EditorUiBus, type EditorUiSubscription } from "./uibus.js";

// ------------------------------------------------------------------------------- the DOM classes

export const STATUSBAR_LINK_CLASS = "ctx-statusbar__link";
export const STATUSBAR_DOT_CLASS = "ctx-statusbar__dot";
export const STATUSBAR_PROBLEMS_CLASS = "ctx-statusbar__problems";
export const STATUSBAR_FILL_CLASS = "ctx-statusbar__fill";
export const STATUSBAR_THEME_CLASS = "ctx-statusbar__theme";
export const STATUSBAR_PROJECT_CLASS = "ctx-statusbar__project";

/** The `<html>` report of what the strip renders — boot diagnosability, like `data-editor-playbar`. */
export const STATUSBAR_ATTRIBUTE = "data-editor-statusbar";

/** The fields' accessible names (the banners worked example's ARIA discipline). */
export const LABEL_LINK = "Daemon link";
export const LABEL_PROBLEMS = "Problems";
export const LABEL_THEME = "Active theme";
export const LABEL_PROJECT = "Project";

// ------------------------------------------------------------------------- the problems node ids
// MIRROR `problems_panel.cpp`'s uitree node ids (`build_panel`): the list container and the
// per-diagnostic row prefix. Grep-stable on BOTH sides — a rename there reds the statusbar tests
// here rather than silently unbinding the count from the feed.

export const PROBLEMS_LIST_NODE_ID = "problems.list";
export const PROBLEMS_ROW_NODE_ID_PREFIX = "problems.row.";

/**
 * The problems count this window can truthfully claim: the number of diagnostic rows the hydrated
 * Problems panel currently renders, or `null` when the panel is not mounted in `doc` (closed, the
 * welcome screen, a bare harness) — in which case there IS no count here, and the caller hides the
 * field rather than inventing one. One row per diagnostic is the C++ model's own rendering rule
 * (problems_panel.cpp `build_panel`), so counting rows IS counting diagnostics.
 */
export function problemsCountFrom(doc: Document): number | null {
    const list = doc.querySelector(`[${NODE_ID_ATTRIBUTE}="${PROBLEMS_LIST_NODE_ID}"]`);
    if (list === null) {
        return null;
    }
    return list.querySelectorAll(`[${NODE_ID_ATTRIBUTE}^="${PROBLEMS_ROW_NODE_ID_PREFIX}"]`).length;
}

/** The problems label — the Problems panel's own status-line vocabulary (problems_panel.cpp). */
export function problemsLabel(count: number): string {
    if (count === 0) {
        return "No problems";
    }
    return `${String(count)}${count === 1 ? " problem" : " problems"}`;
}

// --------------------------------------------------------------------------- the link presentation

/** What the link field renders for one `daemon.linkState` answer — dot tone + label, in one table. */
export interface LinkPresentation {
    readonly tone: SemanticTone;
    readonly label: string;
}

/**
 * The ONE presentation record per link state (the playbar's PRESENTATION rule: one table, not
 * parallel switches). Derived from the SAME fields the daemon-lost banner reads, with the same
 * semantics: `readOnly: false` is the live read-write link; a retrying ladder is `wait` (something
 * is actively happening — "reconnecting" for a daemon this editor owns and will respawn, "waiting"
 * for a shared one it can only wait for, banners.ts's ownership distinction); a read-only link with
 * no retries yet is `warn` (the state a user must know about — the banner's own tone for it).
 */
export function linkPresentation(link: DaemonLinkState): LinkPresentation {
    if (!link.readOnly) {
        return { tone: "good", label: "Live" };
    }
    if (link.reconnectAttempts > 0) {
        const verb = link.ownership === DAEMON_OWNERSHIP_OWNED ? "Reconnecting" : "Waiting";
        return { tone: "wait", label: `${verb} (${String(link.reconnectAttempts)})` };
    }
    return { tone: "warn", label: "Read-only" };
}

// ------------------------------------------------------------------------------- the strip mount

export interface MountStatusbarOptions {
    /** The project's display name; `""` renders the product name (the titlebar's exact rule). */
    readonly projectName: string;
}

/** What `mountStatusbar` produced — the handle boot keeps and re-renders through. */
export interface StatusbarMount {
    /** Render a `daemon.linkState` answer. `null` (no surface) hides the field. Idempotent. */
    applyLink(link: DaemonLinkState | null): void;
    /** Render a problems count. `null` (no mounted Problems panel) hides the field. Idempotent. */
    applyProblems(count: number | null): void;
    /** Render the active theme's display name. `""` (no theme engine) hides the field. Idempotent. */
    applyTheme(name: string): void;
    /** Re-derive the problems count from the document the strip lives in (fact 2 above). */
    refreshProblems(): void;
}

/**
 * Render the statusbar's content into the a2 shell. Replaces the shell's children wholesale (a
 * re-mount is a re-render, the `mountChrome` rule) and seeds every field EMPTY/hidden — the caller
 * applies each feed's truth as it lands (boot seeds the link + theme immediately; the problems
 * count arrives with the first panel refresh). The strip renders in BOTH welcome and project modes:
 * it is part of the frame, and each field's honesty rule decides what shows.
 */
export function mountStatusbar(slot: HTMLElement, options: MountStatusbarOptions): StatusbarMount {
    const doc = slot.ownerDocument;
    slot.replaceChildren();

    const el = (tag: string, className: string): HTMLElement => {
        const node = doc.createElement(tag);
        node.className = className;
        return node;
    };

    // --- the daemon-link field: dot + live badge -------------------------------------------------
    // A labelled GROUP (the banners worked example's ARIA discipline): "Live" alone is ambiguous to
    // a screen reader browsing the strip; the group says what the value belongs to. The dot is
    // DECORATION (aria-hidden, styled off `data-tone` in app.css); the badge is the live kit
    // primitive — a link transition is a legitimate, rare announcement.
    const link = el("span", STATUSBAR_LINK_CLASS);
    link.setAttribute("role", "group");
    link.setAttribute("aria-label", LABEL_LINK);
    link.hidden = true;
    const dot = el("span", STATUSBAR_DOT_CLASS);
    dot.setAttribute("aria-hidden", "true");
    const linkBadge: KitBadge = createBadge({ label: "", tone: "idle", live: true });
    link.append(dot, linkBadge.element);
    slot.append(link);

    // --- the problems field: a live badge --------------------------------------------------------
    // The kit's own worked example for `createBadge({ live: true })` (status.ts's module header
    // names the Problems count) — a derivation state whose changes are worth a polite announcement.
    const problems = el("span", STATUSBAR_PROBLEMS_CLASS);
    problems.setAttribute("role", "group");
    problems.setAttribute("aria-label", LABEL_PROBLEMS);
    problems.hidden = true;
    const problemsBadge: KitBadge = createBadge({ label: "", tone: "neutral", live: true });
    problems.append(problemsBadge.element);
    slot.append(problems);

    slot.append(el("span", STATUSBAR_FILL_CLASS));

    // --- the theme + project identity: plain text readouts (the playbar timer's pattern) ---------
    // Not controls, so not kit components — a `title` for the pointer hover, and an aria-label that
    // carries the field name so assistive tech hears "Active theme: Dark", not a bare word.
    const themeField = el("span", STATUSBAR_THEME_CLASS);
    themeField.title = LABEL_THEME;
    themeField.hidden = true;
    slot.append(themeField);

    const projectField = el("span", STATUSBAR_PROJECT_CLASS);
    projectField.title = LABEL_PROJECT;
    // The titlebar's exact fallback rule (chrome.ts): the product name when no project is known —
    // the welcome screen's state, and an older Shell with no welcome surface.
    const projectText = options.projectName !== "" ? options.projectName : DEFAULT_TITLE;
    projectField.textContent = projectText;
    projectField.setAttribute("aria-label", `${LABEL_PROJECT}: ${projectText}`);
    slot.append(projectField);

    // --- the render state + report ---------------------------------------------------------------
    let renderedLink = "";
    let renderedProblems: number | null = null;
    let renderedTheme = "";
    const report = (): void => {
        doc.documentElement.setAttribute(
            STATUSBAR_ATTRIBUTE,
            `link ${renderedLink === "" ? "none" : renderedLink}; problems ${
                renderedProblems === null ? "none" : String(renderedProblems)
            }; theme ${renderedTheme === "" ? "none" : renderedTheme}; project ${projectText}`,
        );
    };
    report();

    const mount: StatusbarMount = {
        applyLink: (state: DaemonLinkState | null): void => {
            // Compared on the RENDERED label (tone derives from the same table row), so a poll
            // tick that read the same state repaints nothing and the live badge stays silent.
            const next = state === null ? null : linkPresentation(state);
            const key = next === null ? "" : next.label;
            if (key === renderedLink) {
                return;
            }
            renderedLink = key;
            if (next === null) {
                link.hidden = true;
            } else {
                link.hidden = false;
                dot.setAttribute(TONE_ATTRIBUTE, next.tone);
                linkBadge.setLabel(next.label);
                linkBadge.setTone(next.tone);
            }
            report();
        },
        applyProblems: (count: number | null): void => {
            if (count === renderedProblems) {
                return;
            }
            renderedProblems = count;
            if (count === null) {
                problems.hidden = true;
            } else {
                problems.hidden = false;
                problemsBadge.setLabel(problemsLabel(count));
            }
            report();
        },
        applyTheme: (name: string): void => {
            if (name === renderedTheme) {
                return;
            }
            renderedTheme = name;
            if (name === "") {
                themeField.hidden = true;
            } else {
                themeField.hidden = false;
                themeField.textContent = name;
                themeField.setAttribute("aria-label", `${LABEL_THEME}: ${name}`);
            }
            report();
        },
        refreshProblems: (): void => {
            mount.applyProblems(problemsCountFrom(doc));
        },
    };
    return mount;
}

/**
 * Wire the theme field to the `editor.ui.theme-changed` topic (fact 1 above). The bus RETAINS the
 * last envelope (snapshot-on-subscribe, uibus.ts), so subscribing after boot's theme apply hands
 * the current theme to the field immediately, and every later switch re-renders through the same
 * listener. Extracted (and exported) so the DOM tier drives THIS function against a real bus rather
 * than a copy of it — the `subscribeChromeFacts` rule.
 */
export function subscribeStatusbarTheme(
    bus: EditorUiBus,
    mount: StatusbarMount,
): EditorUiSubscription {
    return bus.subscribe<ThemeChangedPayload>(UI_TOPIC_THEME_CHANGED, (event): void => {
        mount.applyTheme(event.payload.name);
    });
}

// ------------------------------------------------------------------------------- the link feed

/** How often the strip re-reads `daemon.linkState`. A local Shell read — no daemon round trip. */
export const STATUSBAR_LINK_POLL_INTERVAL_MS = 1_000;

/**
 * The link field's update channel: re-read `daemon.linkState` on a tick and apply the answer.
 *
 * A POLL, deliberately — the bridge accepts no persistent queries (fact 1 above), so link
 * transitions (a daemon lost mid-session, the reconnect ladder counting up, the link coming back)
 * can only be observed by asking again. SELF-STOPPING on a refusal, the session feed's rule: `null`
 * means the Shell does not serve the surface (an older build — the method cannot be withdrawn
 * mid-session by a Shell that has it), so a refusal per tick for the life of the window is a cost
 * with no possible payoff; the field is hidden by that same `null` and stays honest.
 */
export class StatusbarLinkFeed {
    readonly #read: () => Promise<DaemonLinkState | null>;
    readonly #apply: (link: DaemonLinkState | null) => void;
    readonly #scheduler: SessionScheduler | undefined;
    #handle: number | null = null;
    #inFlight = false;

    constructor(
        read: () => Promise<DaemonLinkState | null>,
        apply: (link: DaemonLinkState | null) => void,
        scheduler: SessionScheduler | undefined = defaultSessionScheduler(),
    ) {
        this.#read = read;
        this.#apply = apply;
        this.#scheduler = scheduler;
    }

    /** Is the interval armed? (The T1 tier's self-stop observable.) */
    get polling(): boolean {
        return this.#handle !== null;
    }

    /** Read once and apply. A `null` answer stops the poll (see the class doc). Never throws. */
    async refresh(): Promise<void> {
        if (this.#inFlight) {
            return;
        }
        this.#inFlight = true;
        try {
            const link = await this.#read();
            this.#apply(link);
            if (link === null) {
                this.stop();
            }
        } finally {
            this.#inFlight = false;
        }
    }

    /** Arm the poll. A scheduler-less host (no timers) never ticks — the boot seed still rendered. */
    start(intervalMs: number = STATUSBAR_LINK_POLL_INTERVAL_MS): void {
        if (this.#scheduler === undefined || this.#handle !== null) {
            return;
        }
        this.#handle = this.#scheduler.setInterval((): void => {
            void this.refresh();
        }, intervalMs);
    }

    /** Disarm the poll. Idempotent. */
    stop(): void {
        if (this.#scheduler !== undefined && this.#handle !== null) {
            this.#scheduler.clearInterval(this.#handle);
            this.#handle = null;
        }
    }
}
