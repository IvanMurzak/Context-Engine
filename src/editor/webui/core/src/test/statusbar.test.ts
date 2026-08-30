// T1 for the statusbar content (editor-window-chrome d2, target design 02 §8).
//
// This tier owns the STRIP — anatomy and the ARIA discipline, the link-state presentation table and
// its transitions, the problems count derived from the hydrated Problems panel's node ids (added,
// removed, and the SOURCE disappearing — the honesty rule's three shapes), the theme field over a
// REAL `editor.ui` bus (retained snapshot + a live flip), the project identity's welcome-vs-project
// rule, and the `StatusbarLinkFeed`'s poll/self-stop/overlap behavior over a scripted reader. The
// end-to-end wiring (a real boot mounting the content from the boot feeds) is boot.test.ts's, and
// the C++ halves (the `daemon.linkState` surface, the Problems model + diagnostics feed) are pinned
// by editor-shell-test_banners / gui-panels-test_problems.

import { assert, assertEqual, type TestCase } from "./harness.js";
import {
    LABEL_LINK,
    LABEL_PROBLEMS,
    LABEL_PROJECT,
    LABEL_THEME,
    PROBLEMS_LIST_NODE_ID,
    PROBLEMS_ROW_NODE_ID_PREFIX,
    STATUSBAR_ATTRIBUTE,
    STATUSBAR_DOT_CLASS,
    STATUSBAR_LINK_CLASS,
    STATUSBAR_LINK_COUNT_CLASS,
    STATUSBAR_LINK_MAX_BACKOFF_TICKS,
    STATUSBAR_PROBLEMS_CLASS,
    STATUSBAR_PROJECT_CLASS,
    STATUSBAR_THEME_CLASS,
    StatusbarLinkFeed,
    linkPresentation,
    linkText,
    mountStatusbar,
    problemsCountFrom,
    problemsLabel,
    subscribeStatusbarTheme,
    type StatusbarMount,
} from "../statusbar.js";
import type { DaemonLinkState } from "../banners.js";
import { DEFAULT_TITLE } from "../chrome.js";
import { NODE_ID_ATTRIBUTE } from "../hydration.js";
import { EditorUiBus, UI_TOPIC_THEME_CHANGED } from "../uibus.js";
import { ManualScheduler } from "./session.test.js";

// --------------------------------------------------------------------------- the DOM harness

interface Harness {
    readonly slot: HTMLElement;
    readonly mount: StatusbarMount;
    dispose(): void;
}

function mountHarness(projectName = ""): Harness {
    const slot = document.createElement("footer");
    slot.id = "statusbar-test-slot";
    document.body.append(slot);
    const mount = mountStatusbar(slot, { projectName });
    return {
        slot,
        mount,
        dispose: (): void => {
            slot.remove();
            document.documentElement.removeAttribute(STATUSBAR_ATTRIBUTE);
        },
    };
}

function field(slot: HTMLElement, className: string): HTMLElement {
    const element = slot.querySelector<HTMLElement>(`.${className}`);
    if (element === null) {
        throw new Error(`no .${className} field`);
    }
    return element;
}

/** The live kit badge inside a field (`createBadge({ live: true })` renders an `<output>`). */
function badge(fieldElement: HTMLElement): HTMLElement {
    const element = fieldElement.querySelector<HTMLElement>("output");
    if (element === null) {
        throw new Error("no live badge in the field");
    }
    return element;
}

function link(overrides: Partial<DaemonLinkState> = {}): DaemonLinkState {
    return {
        readOnly: false,
        reconnectAttempts: 0,
        ownership: "owned",
        lastError: "",
        ...overrides,
    };
}

/** A fake hydrated Problems panel — the node-id shapes `uitree::render_html` emits for the model. */
function problemsFixture(rows: number): HTMLElement {
    const list = document.createElement("ul");
    list.setAttribute(NODE_ID_ATTRIBUTE, PROBLEMS_LIST_NODE_ID);
    for (let i = 0; i < rows; i += 1) {
        const row = document.createElement("li");
        row.setAttribute(NODE_ID_ATTRIBUTE, `${PROBLEMS_ROW_NODE_ID_PREFIX}${String(i)}`);
        list.append(row);
    }
    document.body.append(list);
    return list;
}

export const statusbarTests: TestCase[] = [
    {
        name: "d2 statusbar: anatomy — every field mounted, sourceless ones hidden, ARIA per the house pattern",
        run: () => {
            const h = mountHarness();
            try {
                const linkField = field(h.slot, STATUSBAR_LINK_CLASS);
                assert(linkField.hidden === true, "the link field waits for its source");
                assertEqual(linkField.getAttribute("role"), "group", "a labelled group");
                assertEqual(linkField.getAttribute("aria-label"), LABEL_LINK, "…naming the field");
                const dot = linkField.querySelector(`.${STATUSBAR_DOT_CLASS}`);
                assertEqual(dot?.getAttribute("aria-hidden"), "true", "the dot is decoration");
                assertEqual(
                    badge(linkField).getAttribute("aria-live"),
                    "polite",
                    "a link transition announces politely, never interrupts",
                );

                const problemsField = field(h.slot, STATUSBAR_PROBLEMS_CLASS);
                assert(problemsField.hidden === true, "the problems field waits for its source");
                assertEqual(problemsField.getAttribute("aria-label"), LABEL_PROBLEMS, "named");
                assertEqual(
                    badge(problemsField).getAttribute("aria-live"),
                    "polite",
                    "the count is the kit's own live-badge worked example",
                );

                const theme = field(h.slot, STATUSBAR_THEME_CLASS);
                assert(theme.hidden === true, "theme waits for its source");
                assertEqual(
                    theme.getAttribute("role"),
                    "group",
                    "a labelled group — an aria-label on a role-less span is ignored by AT",
                );
                const project = field(h.slot, STATUSBAR_PROJECT_CLASS);
                assert(!project.hidden, "the project identity always renders");
                assertEqual(
                    project.getAttribute("role"),
                    "group",
                    "…the same labelled-group treatment as every named field",
                );
                assertEqual(
                    document.documentElement.getAttribute(STATUSBAR_ATTRIBUTE),
                    `link none; problems none; theme none; project ${DEFAULT_TITLE}`,
                    "the <html> report names every field's state",
                );
            } finally {
                h.dispose();
            }
        },
    },
    {
        name: "d2 statusbar: the link presentation table — live / reconnecting / waiting / read-only",
        run: () => {
            assertEqual(
                linkPresentation(link()),
                { tone: "good", label: "Live", count: "" },
                "a read-write link is good",
            );
            assertEqual(
                linkPresentation(link({ readOnly: true, reconnectAttempts: 3 })),
                { tone: "wait", label: "Reconnecting", count: "(3)" },
                "an OWNED daemon mid-ladder is actively reconnecting — the count is SEPARATE",
            );
            assertEqual(
                linkPresentation(
                    link({ readOnly: true, reconnectAttempts: 2, ownership: "external" }),
                ),
                { tone: "wait", label: "Waiting", count: "(2)" },
                "a shared daemon can only be waited for (banners.ts's ownership distinction)",
            );
            assertEqual(
                linkPresentation(link({ readOnly: true })),
                { tone: "warn", label: "Read-only", count: "" },
                "read-only with no retries yet is the warn state the banner uses",
            );
            assertEqual(
                linkText(linkPresentation(link({ readOnly: true, reconnectAttempts: 3 }))),
                "Reconnecting (3)",
                "the report / sighted reading joins the two",
            );
            assertEqual(linkText(linkPresentation(link())), "Live", "…with no count, no gap");
        },
    },
    {
        name: "d2 statusbar: link-state transitions render dot tone + label, and null hides the field",
        run: () => {
            const h = mountHarness();
            try {
                const linkField = field(h.slot, STATUSBAR_LINK_CLASS);
                const dot = field(h.slot, STATUSBAR_DOT_CLASS);

                h.mount.applyLink(link());
                assert(!linkField.hidden, "a served link shows the field");
                assertEqual(dot.getAttribute("data-tone"), "good", "live is the good hue");
                assertEqual(badge(linkField).textContent, "Live", "…said plainly");

                const count = field(h.slot, STATUSBAR_LINK_COUNT_CLASS);
                assert(count.hidden === true, "no attempts, no count");

                h.mount.applyLink(link({ readOnly: true, reconnectAttempts: 2 }));
                assertEqual(dot.getAttribute("data-tone"), "wait", "the ladder is wait");
                assertEqual(badge(linkField).textContent, "Reconnecting", "the STATE word is live");
                assert(!count.hidden, "the attempt count shows beside it");
                assertEqual(count.textContent, "(2)", "…outside the live region (3b)");
                assert(
                    count.getAttribute("aria-live") === null && count.tagName !== "OUTPUT",
                    "the count is a plain span — a ladder step must never be an announcement",
                );
                assertEqual(
                    linkField.textContent,
                    "Reconnecting(2)",
                    "the group still reads both to a screen reader browsing the strip",
                );

                h.mount.applyLink(link({ readOnly: true }));
                assertEqual(dot.getAttribute("data-tone"), "warn", "read-only is warn");
                assertEqual(badge(linkField).textContent, "Read-only", "the user-visible fact");
                assert(count.hidden === true, "a state with no ladder hides the count again");
                assert(
                    (document.documentElement.getAttribute(STATUSBAR_ATTRIBUTE) ?? "").includes(
                        "link Read-only",
                    ),
                    "the report tracks the transition",
                );

                h.mount.applyLink(null);
                assert(
                    linkField.hidden === true,
                    "no surface hides the field — honesty, not an error",
                );
            } finally {
                h.dispose();
            }
        },
    },
    {
        name: "d2 statusbar: an unchanged link repaints nothing (the live region must not re-announce)",
        run: () => {
            const h = mountHarness();
            try {
                h.mount.applyLink(link());
                // A MutationObserver over the badge's TEXT, not text-node identity: Blink's
                // single-text-child `textContent` fast path reuses the node (same data, same
                // identity), so an identity probe scores GREEN with the short-circuit deleted —
                // a plant proved it. A characterData/childList record is queued for every
                // `textContent` write, identical value or not, so ZERO records is the observable
                // that actually discriminates.
                const observer = new MutationObserver(() => undefined);
                observer.observe(badge(field(h.slot, STATUSBAR_LINK_CLASS)), {
                    childList: true,
                    characterData: true,
                    subtree: true,
                });
                try {
                    // The SAME state again — a poll tick reading an unchanged answer.
                    h.mount.applyLink(link());
                    assertEqual(
                        observer.takeRecords().length,
                        0,
                        "the short-circuit left the live region's text untouched",
                    );
                    // The positive half, so this probe cannot rot into one that can never fail:
                    // a REAL transition must touch the very text the observer watches.
                    h.mount.applyLink(link({ readOnly: true }));
                    assert(
                        observer.takeRecords().length > 0,
                        "a real transition does rewrite the live region (the observer can see)",
                    );
                } finally {
                    observer.disconnect();
                }
            } finally {
                h.dispose();
            }
        },
    },
    {
        name: "d2 statusbar: a reconnect ladder counting up re-announces NOTHING — only the state word is live",
        run: () => {
            // d2 review 3b. The ladder's backoff steps (200 ms → 5 s) each bump `reconnectAttempts`;
            // with the count inside the live badge every step was an announcement. Now the count
            // lives in its own span: the live region's text is written once, on the transition
            // INTO "Reconnecting", and stays untouched while the count climbs.
            const h = mountHarness();
            try {
                h.mount.applyLink(link({ readOnly: true, reconnectAttempts: 1 }));
                const linkField = field(h.slot, STATUSBAR_LINK_CLASS);
                const count = field(h.slot, STATUSBAR_LINK_COUNT_CLASS);
                const observer = new MutationObserver(() => undefined);
                observer.observe(badge(linkField), {
                    childList: true,
                    characterData: true,
                    subtree: true,
                });
                try {
                    for (const attempts of [2, 3, 4, 5]) {
                        h.mount.applyLink(link({ readOnly: true, reconnectAttempts: attempts }));
                        assertEqual(count.textContent, `(${String(attempts)})`, "the count moved");
                    }
                    assertEqual(
                        observer.takeRecords().length,
                        0,
                        "four ladder steps left the live region's text untouched",
                    );
                    // …and the report still carries the full reading a test (boot.test.ts) or a
                    // sighted user reads.
                    assert(
                        (document.documentElement.getAttribute(STATUSBAR_ATTRIBUTE) ?? "").includes(
                            "link Reconnecting (5)",
                        ),
                        "the report joins state and count",
                    );
                    // The positive half: the ladder ENDING is a transition, and it IS announced.
                    h.mount.applyLink(link());
                    assert(
                        observer.takeRecords().length > 0,
                        "the link coming back rewrote the live region (the observer can see)",
                    );
                    assertEqual(badge(linkField).textContent, "Live", "…to the new state word");
                    assert(count.hidden === true, "…and the count went away with the ladder");
                } finally {
                    observer.disconnect();
                }
            } finally {
                h.dispose();
            }
        },
    },
    {
        name: "d2 statusbar: problemsCountFrom counts hydrated rows, and null means no mounted source",
        run: () => {
            assertEqual(
                problemsCountFrom(document),
                null,
                "no Problems panel in the document ⇒ no count to claim",
            );
            const list = problemsFixture(2);
            try {
                assertEqual(problemsCountFrom(document), 2, "one row per diagnostic");
            } finally {
                list.remove();
            }
            assertEqual(problemsLabel(0), "No problems", "the panel's own vocabulary");
            assertEqual(problemsLabel(1), "1 problem", "singular");
            assertEqual(problemsLabel(3), "3 problems", "plural");
        },
    },
    {
        name: "d2 statusbar: the problems count follows the feed — grows, empties, and hides with its source",
        run: () => {
            const h = mountHarness();
            const list = problemsFixture(2);
            try {
                const problemsField = field(h.slot, STATUSBAR_PROBLEMS_CLASS);
                h.mount.refreshProblems();
                assert(!problemsField.hidden, "a mounted Problems panel shows the field");
                assertEqual(badge(problemsField).textContent, "2 problems", "the row count");

                // A diagnostic lands: the refresh driver re-rendered the panel with one more row.
                const row = document.createElement("li");
                row.setAttribute(NODE_ID_ATTRIBUTE, `${PROBLEMS_ROW_NODE_ID_PREFIX}2`);
                list.append(row);
                h.mount.refreshProblems();
                assertEqual(badge(problemsField).textContent, "3 problems", "the count moved");

                // The diagnostics clear: zero is a COUNT, not an absence — the field stays.
                list.replaceChildren();
                h.mount.refreshProblems();
                assert(!problemsField.hidden, "an empty set still has a truthful count");
                assertEqual(badge(problemsField).textContent, "No problems", "…said plainly");

                // The panel closes: the SOURCE is gone, so the field hides rather than going stale.
                list.remove();
                h.mount.refreshProblems();
                assert(problemsField.hidden === true, "no source ⇒ no number nothing backs");
            } finally {
                list.remove();
                h.dispose();
            }
        },
    },
    {
        name: "d2 statusbar: the theme field rides the retained theme-changed envelope and follows a flip",
        run: () => {
            const h = mountHarness();
            try {
                const bus = new EditorUiBus();
                // Published BEFORE the subscription — boot's theme apply happens first, and the
                // retained snapshot (uibus.ts snapshot-on-subscribe) is what seeds the field.
                bus.publish(UI_TOPIC_THEME_CHANGED, {
                    themeId: "builtin.dark",
                    name: "Dark",
                    appearance: "dark",
                });
                subscribeStatusbarTheme(bus, h.mount);
                const themeField = field(h.slot, STATUSBAR_THEME_CLASS);
                assert(!themeField.hidden, "the retained envelope seeded the field");
                assertEqual(themeField.textContent, "Dark", "with the theme's display name");
                assertEqual(
                    themeField.getAttribute("aria-label"),
                    `${LABEL_THEME}: Dark`,
                    "assistive tech hears the field name, not a bare word",
                );

                bus.publish(UI_TOPIC_THEME_CHANGED, {
                    themeId: "builtin.light",
                    name: "Light",
                    appearance: "light",
                });
                assertEqual(themeField.textContent, "Light", "a live flip re-renders");
                assert(
                    (document.documentElement.getAttribute(STATUSBAR_ATTRIBUTE) ?? "").includes(
                        "theme Light",
                    ),
                    "the report tracks the flip",
                );
            } finally {
                h.dispose();
            }
        },
    },
    {
        name: "d2 statusbar: project identity — the project's name, or the product name on the welcome screen",
        run: () => {
            const welcome = mountHarness("");
            try {
                assertEqual(
                    field(welcome.slot, STATUSBAR_PROJECT_CLASS).textContent,
                    DEFAULT_TITLE,
                    "no project name ⇒ the titlebar's exact fallback",
                );
            } finally {
                welcome.dispose();
            }
            const project = mountHarness("Asteroids");
            try {
                const projectField = field(project.slot, STATUSBAR_PROJECT_CLASS);
                assertEqual(projectField.textContent, "Asteroids", "the project's display name");
                assertEqual(
                    projectField.getAttribute("aria-label"),
                    `${LABEL_PROJECT}: Asteroids`,
                    "named for assistive tech",
                );
            } finally {
                project.dispose();
            }
        },
    },
    {
        name: "d2 statusbar: the link feed polls, applies, and SELF-STOPS when the surface refuses",
        run: async () => {
            const scheduler = new ManualScheduler();
            const answers: (DaemonLinkState | null)[] = [
                link({ readOnly: true, reconnectAttempts: 1 }),
                null,
            ];
            const applied: (DaemonLinkState | null)[] = [];
            const feed = new StatusbarLinkFeed(
                (): Promise<DaemonLinkState | null> => Promise.resolve(answers.shift() ?? null),
                (state): void => {
                    applied.push(state);
                },
                scheduler,
            );
            feed.start();
            assert(feed.polling, "start arms the interval");

            await feed.refresh();
            assertEqual(applied.length, 1, "a read applies its answer");
            assertEqual(applied[0]?.reconnectAttempts, 1, "…the answer it read");
            assert(feed.polling, "a served read keeps polling");

            scheduler.fire();
            // The tick's refresh is fire-and-forget; one microtask turn settles the resolved read.
            await Promise.resolve();
            await Promise.resolve();
            assertEqual(applied.length, 2, "the tick read too");
            assertEqual(applied[1], null, "…and applied the refusal (the field hides on it)");
            assert(!feed.polling, "a refusal self-stops the poll — no payoff in asking again");
            assertEqual(scheduler.cleared, [1], "the interval was actually cleared");
        },
    },
    {
        name: "d2 statusbar: a FAULTING read keeps the last state, backs off, and recovers — only a refusal stops",
        run: async () => {
            // d2 review 3a. A throw is a transient fault (a lost bridge query while the C++ link
            // machine reconnects underneath), NOT "no surface": the field keeps the last state it
            // could vouch for, the poll continues on a 1, 2, 4 … STATUSBAR_LINK_MAX_BACKOFF_TICKS
            // ladder, and the next good read resets it. Before this split one throw hid the
            // indicator for the life of the window.
            const scheduler = new ManualScheduler();
            const script: (DaemonLinkState | null | Error)[] = [
                link({ readOnly: true, reconnectAttempts: 1 }),
                new Error("bridge.transport"),
                new Error("bridge.transport"),
                new Error("bridge.transport"),
                new Error("bridge.transport"),
                link(),
                null,
            ];
            let reads = 0;
            const applied: (DaemonLinkState | null)[] = [];
            const feed = new StatusbarLinkFeed(
                (): Promise<DaemonLinkState | null> => {
                    reads += 1;
                    const next = script.shift() ?? null;
                    return next instanceof Error ? Promise.reject(next) : Promise.resolve(next);
                },
                (state): void => {
                    applied.push(state);
                },
                scheduler,
            );
            feed.start();
            const tick = async (): Promise<void> => {
                scheduler.fire();
                await Promise.resolve();
                await Promise.resolve();
            };

            await tick(); // the good read
            assertEqual(applied.length, 1, "the first read applied");
            assertEqual(feed.faults, 0, "…and counts no fault");

            await tick(); // fault 1 → skip 1 tick
            assertEqual(feed.faults, 1, "a rejecting reader is a fault");
            assertEqual(applied.length, 1, "a fault applies NOTHING — the last state stands");
            assert(feed.polling, "a fault does not stop the poll");
            await tick(); // skipped
            assertEqual(reads, 2, "the tick after a fault was skipped (backoff 1)");
            await tick(); // fault 2 → skip 2
            assertEqual(feed.faults, 2, "the second consecutive fault");
            assertEqual(reads, 3, "…was read on the tick after the skip");
            await tick();
            await tick();
            assertEqual(reads, 3, "two ticks skipped after the second fault (backoff 2)");
            await tick(); // fault 3 → skip 4
            assertEqual(reads, 4, "the third read");
            assertEqual(feed.faults, 3, "the third consecutive fault");
            for (let i = 0; i < 4; i += 1) {
                await tick();
            }
            assertEqual(reads, 4, "four ticks skipped after the third fault (backoff 4)");
            await tick(); // fault 4 → skip min(8, MAX)
            assertEqual(feed.faults, 4, "the fourth consecutive fault");
            for (let i = 0; i < STATUSBAR_LINK_MAX_BACKOFF_TICKS; i += 1) {
                await tick();
            }
            assertEqual(reads, 5, `the ceiling: ${String(STATUSBAR_LINK_MAX_BACKOFF_TICKS)} ticks skipped, not 8`);
            await tick(); // the good read: recovery
            assertEqual(reads, 6, "read again after the ceiling");
            assertEqual(feed.faults, 0, "a good read resets the fault count");
            assertEqual(applied.length, 2, "…and applies");
            assertEqual(applied[1]?.readOnly, false, "…the recovered link");
            await tick(); // no skip after a good read
            assertEqual(reads, 7, "the very next tick reads again — the backoff was reset");
            assertEqual(applied[2], null, "…and the refusal applied (the field hides on it)");
            assert(!feed.polling, "a REFUSAL still self-stops — that one is not a fault");
        },
    },
    {
        name: "d2 statusbar: an in-flight read swallows the next tick (no overlapping bridge calls)",
        run: async () => {
            // A holder object, not a `let`: TS narrows a closed-over `let` at the use site and
            // cannot see the executor's assignment; a property stays un-narrowed.
            const pending: { resolve: ((state: DaemonLinkState | null) => void) | null } = {
                resolve: null,
            };
            let reads = 0;
            const applied: (DaemonLinkState | null)[] = [];
            const feed = new StatusbarLinkFeed(
                (): Promise<DaemonLinkState | null> => {
                    reads += 1;
                    return new Promise((resolve) => {
                        pending.resolve = resolve;
                    });
                },
                (state): void => {
                    applied.push(state);
                },
                new ManualScheduler(),
            );
            const first = feed.refresh();
            const second = feed.refresh();
            assertEqual(reads, 1, "the second refresh returned early instead of stacking a read");
            pending.resolve?.(link());
            await first;
            await second;
            assertEqual(applied.length, 1, "one read, one apply");
        },
    },
];
