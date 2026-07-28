// T1 for the LOUD refused-write NOTIFICATION HOST (M9 e09b-3, design 05 §8, 06 §2-§3,
// 10 § Non-negotiable UX invariants).
//
// WHAT THIS HAS TO PROVE, AND WHY EACH HALF IS EASY TO FAKE. "LOUD" is exactly the kind of claim that
// passes vacuously: a host that renders SOMETHING on every bus event satisfies every positive case
// here while being useless (a toast after each successful edit trains the human to ignore the
// channel), and a host that renders NOTHING satisfies every a11y case by construction. So each of the
// three sinks the design names gets BOTH directions:
//
//   * NOTIFICATION — a refused write shows a toast (positive), and a fact on ANY OTHER topic, or a
//     payload that carries nothing, shows none (control).
//   * `editor.ui` FACT — the notice reaches the host over the REAL arrival path a Shell publish takes
//     (`receiveMirrored`, origin "shell"), not merely a local `publish` a test invented.
//   * WAIT-HUE — the DOM really carries `data-tone="wait"` for a drop and `"bad"` for a refusal, and
//     colour is provably NOT the only signal: the same element's TEXT states what happened, and it
//     lands in the ASSERTIVE live region so a screen-reader user is told at the same moment.
//
// M9 x10 (CE #452) ADDS A THIRD KIND — an ABANDONED gesture, where no write was ever attempted because
// the Inspector had to replace the model mid-edit. It shares the drop's `wait` hue (the human is
// who must act) and has its OWN sentence (the drop's would be false in every clause), so the cases
// below assert BOTH: the shared hue, and that the drop's story appears nowhere in it. A kind whose only
// distinguishing test was its own token would be a vocabulary entry, not a behaviour.
//
// That sentence is also CAUSE-NEUTRAL, and the cases below pin that too — as a NEGATIVE, because it was
// wrong before review: a notice sees only `kind`, and an abandonment comes from a foreign selection move
// OR from a same-entity re-read, so asserting "the selection changed" was false on the commoner one.
//
// The last one is the a11y coverage this task owes: `wait` had no production consumer before e09b-3,
// so its accessible behaviour is net-new and is asserted here rather than assumed from the token.

import { assert, assertEqual, assertNull, type TestCase } from "./harness.js";
import {
    createNotificationHost,
    MAX_STANDING_NOTICES,
    parseWriteNotice,
    writeNoticeHeadline,
    writeNoticeText,
    writeNoticeTone,
    WRITE_NOTICE_KIND_ABANDONED,
    WRITE_NOTICE_KIND_DROP,
    WRITE_NOTICE_KIND_REFUSAL,
} from "../notifications.js";
import {
    EditorUiBus,
    UI_TOPIC_PALETTE,
    UI_TOPIC_WRITE_NOTICE,
    type EditorUiEvent,
} from "../uibus.js";
import { createToastRegion, type KitToastRegion } from "../../../kit/src/index.js";

/** The payload the Shell puts on the wire (write_notice.cpp `write_notice_envelope`). */
function payload(kind: string, overrides: Record<string, unknown> = {}): Record<string, unknown> {
    return {
        kind,
        action: "edit",
        // Each kind carries the code its own producer actually mints — `cas.mismatch` from the L-30
        // engine, `shell.no_daemon` from the wire gateway, and the Shell-minted
        // `shell.gesture_abandoned` (inspector_feed.h) for an abandonment, which is deliberately NOT a
        // contract-catalog entry because no daemon verb was called at all. A fixture that gave all
        // three the same code would let a renderer that keyed on the CODE instead of the KIND pass.
        code:
            kind === WRITE_NOTICE_KIND_DROP
                ? "cas.mismatch"
                : kind === WRITE_NOTICE_KIND_ABANDONED
                  ? "shell.gesture_abandoned"
                  : "shell.no_daemon",
        message: "another writer advanced the file",
        pointer: "/components/camera/fov",
        ...overrides,
    };
}

/** The WHOLE envelope, exactly as the Shell's relay broadcasts it — origin "shell", never a window id. */
function shellEnvelope(kind: string, seq = 1): EditorUiEvent {
    return { seq, topic: UI_TOPIC_WRITE_NOTICE, origin: "shell", payload: payload(kind) };
}

/** Every toast currently rendered inside a host's region. */
function toasts(element: HTMLElement): HTMLElement[] {
    return [...element.querySelectorAll(".ctx-toast")] as HTMLElement[];
}

/** The live region an element sits inside — the lane that decides whether it interrupts. */
function lane(toast: HTMLElement): HTMLElement | null {
    return toast.closest(".ctx-toast-region__lane") as HTMLElement | null;
}

export const notificationTests: TestCase[] = [
    // ------------------------------------------------------------------ 1. the total wire parser
    {
        name: "notifications: parseWriteNotice is total — a non-record is null, a bad member defaults",
        run: () => {
            assertNull(parseWriteNotice(null), "null carries no notice");
            assertNull(parseWriteNotice("drop"), "a bare string is not a payload");
            assertNull(parseWriteNotice(42), "nor is a number");

            // A member of the WRONG type reads as its neutral default rather than throwing: this runs
            // inside a bus fan-out, where a throw would cost every other subscriber the fact.
            const notice = parseWriteNotice({ kind: 7, action: null, pointer: "/a" });
            assert(notice !== null, "a record always parses");
            assertEqual(notice?.kind, "", "a non-string kind reads as empty");
            assertEqual(notice?.action, "", "and so does a null action");
            assertEqual(notice?.pointer, "/a", "while a real string survives");
        },
    },

    // -------------------------------------------------------------------- 2. the hue is the message
    {
        name: "notifications: a drop is the wait hue, a refusal is bad, and an unknown kind fails LOUD",
        run: () => {
            // 06 §2 binds the hues 1:1 to semantics: `wait` is awaiting-human (your edit did not land,
            // re-apply it), `bad` is an error (nothing was written and re-applying will not help).
            assertEqual(writeNoticeTone(WRITE_NOTICE_KIND_DROP), "wait", "a drop awaits the human");
            assertEqual(writeNoticeTone(WRITE_NOTICE_KIND_REFUSAL), "bad", "a refusal is an error");
            // The two tokens are the C++ spellings the panel-contract gate pins; assert the VALUES so
            // a rename that slipped past the gate still reds here.
            assertEqual(WRITE_NOTICE_KIND_DROP, "drop", "the C++ drop token");
            assertEqual(WRITE_NOTICE_KIND_REFUSAL, "refusal", "the C++ refusal token");
            // M9 x10 (CE #452): an ABANDONMENT shares the `wait` hue with a drop — the write path is
            // healthy and the human is who must act — while being a DISTINCT kind, because the
            // sentence differs (case 3 below). Both facts are asserted: the shared hue, so a future
            // refactor cannot quietly demote it to `bad` ("the project is unreachable", which would be
            // a lie), and the distinct token, so it cannot collapse into `drop`.
            assertEqual(
                writeNoticeTone(WRITE_NOTICE_KIND_ABANDONED),
                "wait",
                "an abandoned gesture awaits the human too",
            );
            assertEqual(WRITE_NOTICE_KIND_ABANDONED, "abandoned", "the C++ abandoned token");
            // ⚠ NO `WRITE_NOTICE_KIND_ABANDONED !== WRITE_NOTICE_KIND_DROP` ASSERTION HERE, and the
            // reason is worth recording: tsgo REFUSES it (TS2367 — "the types '\"abandoned\"' and
            // '\"drop\"' have no overlap"), because both are literal-typed `const`s, so the comparison
            // is decided at COMPILE time and could never fail at runtime. It is exactly the
            // assertion-that-cannot-fail class this milestone keeps shipping, and the typechecker
            // catches it here. The two `assertEqual`s above already pin the distinctness, at the only
            // place a drift could actually occur — the VALUES.
            // The behavioural distinction (they produce different SENTENCES) is case 3's.
            // An unknown kind takes the SEVERE hue, not the gentle one: over-stating costs a second
            // look, under-stating sends the human re-applying an edit that cannot land.
            assertEqual(writeNoticeTone("something-new"), "bad", "an unknown kind is not soothing");
        },
    },

    // ------------------------------------------------------------ 3. the sentence a human can act on
    {
        name: "notifications: the headline names the field, says nothing was overwritten, and what to do",
        run: () => {
            const drop = parseWriteNotice(payload(WRITE_NOTICE_KIND_DROP));
            assert(drop !== null, "the fixture parses");
            const headline = writeNoticeHeadline(drop!);
            // WHICH field. On a dense inspector "your edit was dropped" is not actionable.
            assert(headline.includes("/components/camera/fov"), "the headline names the field");
            // WHAT SURVIVED. This is the reassurance the L-30 guarantee actually earns, and the one
            // thing a human cannot infer from a red box.
            assert(headline.includes("Nothing was overwritten"), "it says the data is safe");
            // WHAT TO DO next.
            assert(headline.includes("re-apply"), "and what to do about it");

            // The engine's own diagnostic is carried as supporting detail, not dropped.
            assert(
                writeNoticeText(drop!).includes("another writer advanced the file"),
                "the write path's own message survives into the accessible text",
            );

            // A refusal reads differently — same surface, different meaning.
            const refusal = parseWriteNotice(payload(WRITE_NOTICE_KIND_REFUSAL));
            const refusalHeadline = writeNoticeHeadline(refusal!);
            assert(refusalHeadline.includes("could not be saved"), "a refusal says nothing was saved");
            assert(
                !refusalHeadline.includes("re-apply"),
                "and does NOT tell the human to re-apply — the project is unreachable, not contended",
            );

            // AND AN ABANDONMENT READS DIFFERENTLY AGAIN (M9 x10, CE #452) — the assertion that makes
            // the third kind worth having. Sharing the drop's hue is fine; sharing its SENTENCE would
            // be a confident falsehood, because there was no compare-and-swap, no co-writer need
            // exist, and the field it names is no longer the one on screen.
            const abandoned = parseWriteNotice(payload(WRITE_NOTICE_KIND_ABANDONED));
            assert(abandoned !== null, "the fixture parses");
            const abandonedHeadline = writeNoticeHeadline(abandoned!);
            // WHAT ACTUALLY HAPPENED, stated CAUSE-NEUTRALLY — see `writeNoticeHeadline`. A notice
            // renders from `kind` alone, and `abandoned` is produced both by a foreign selection move
            // AND by a same-entity re-read (read-your-writes landing on a gesture begun meanwhile), so
            // the headline may not assert either. The specific cause rides in `notice.message`.
            assert(
                abandonedHeadline.includes("replaced the content you were editing"),
                "the abandonment says the Inspector replaced what was under the edit",
            );
            assert(abandonedHeadline.includes("Nothing was written"), "and that nothing was written");
            // WHAT TO DO — go back to the field, which is different advice from the drop's.
            assert(abandonedHeadline.includes("re-open the field"), "and tells them to re-open it");
            // ⚠ AND IT MUST NOT NAME A CAUSE IT CANNOT KNOW. This is the discriminating negative for
            // the review finding that removed the old wording: the sentence used to assert "the
            // Inspector's selection changed" and tell the human to "re-select that entity", which is
            // false and unactionable on the re-read path.
            assert(
                !abandonedHeadline.includes("selection changed"),
                "it does NOT assert a selection change it cannot know happened",
            );
            assert(
                !abandonedHeadline.includes("re-select"),
                "nor tell them to re-select an entity that may never have been deselected",
            );
            // AND IT MUST NOT INHERIT THE DROP'S STORY. This is the discriminating pair: without it, a
            // regression that routed `abandoned` through the drop branch (or dropped the branch
            // altogether, falling through to the refusal text) would leave every assertion above
            // satisfiable by the wrong sentence.
            assert(
                !abandonedHeadline.includes("someone else changed that field first"),
                "it does NOT claim a co-writer collision that never happened",
            );
            assert(
                !abandonedHeadline.includes("could not be saved"),
                "nor does it fall through to the refusal text — the write path was never asked",
            );
        },
    },

    // ---------------------------------- 4. the host renders a real notice, over the REAL arrival path
    {
        name: "notifications: a Shell-published notice arrives over receiveMirrored and is rendered",
        run: () => {
            // The bus origin is this WINDOW's id, exactly as boot.ts sets it. That is what makes the
            // next line a real test of the arrival path: `receiveMirrored` DROPS an envelope whose
            // origin matches the bus's own, so a notice stamped with a window id would be swallowed.
            const bus = new EditorUiBus({ origin: "0" });
            const host = createNotificationHost(bus);

            const report = bus.receiveMirrored(shellEnvelope(WRITE_NOTICE_KIND_DROP));
            assert(report.published, "the Shell's envelope was applied, not echo-suppressed");
            assertEqual(host.shown, 1, "and the host rendered it");
            assertEqual(host.onScreen, 1, "the toast is on screen");
            assertEqual(host.last?.code, "cas.mismatch", "carrying the write path's code");

            const rendered = toasts(host.element);
            assertEqual(rendered.length, 1, "exactly one toast");
            // THE WAIT HUE, in the DOM rather than in a variable.
            assertEqual(
                rendered[0]?.getAttribute("data-tone"),
                "wait",
                "a drop paints the wait hue (06 §2 awaiting-human)",
            );
            host.dispose();
        },
    },

    // ------------------------------------------------------- 5. colour is NEVER the only signal
    {
        name: "notifications: the notice is announced assertively and states its meaning in TEXT",
        run: () => {
            const bus = new EditorUiBus({ origin: "0" });
            const host = createNotificationHost(bus);
            bus.receiveMirrored(shellEnvelope(WRITE_NOTICE_KIND_DROP));

            const toast = toasts(host.element)[0];
            assert(toast !== undefined, "a toast was rendered");

            // (a) TEXT. Strip the hue away entirely and the element still says what happened — the
            // R-A11Y-001 rule that a status may not be carried by colour alone.
            const text = toast?.textContent ?? "";
            assert(text.includes("was not applied"), "the text states the outcome");
            assert(text.includes("/components/camera/fov"), "and which field it was about");

            // (b) ANNOUNCEMENT. It lands in the ASSERTIVE lane (role=alert), so a screen-reader user
            // is told at the moment it happens rather than whenever they next navigate here. The
            // kit's tone-derived default would have put `wait` in the polite lane; `assertive: true`
            // is what overrides that, and this assertion is what stops a refactor undoing it.
            const region = lane(toast!);
            assert(region !== null, "the toast sits inside a live region");
            assertEqual(region?.getAttribute("role"), "alert", "which is the assertive one");
            assertEqual(region?.getAttribute("aria-live"), "assertive", "and says so");

            // (c) DISMISSIBLE BY KEYBOARD. The kit gives every toast a labelled close control; a
            // notice a keyboard user cannot clear is a permanent obstruction, not a notification.
            const close = toast?.querySelector(".ctx-toast__close");
            assert(close !== null && close !== undefined, "there is a close control");
            assertEqual(
                close?.getAttribute("aria-label"),
                "Dismiss notification",
                "and it is named for assistive technology",
            );
            host.dispose();
        },
    },

    // ------------------------------------------------------------------ 6. a refusal takes `bad`
    {
        name: "notifications: a write-path refusal renders the bad hue, not the wait hue",
        run: () => {
            const bus = new EditorUiBus({ origin: "0" });
            const host = createNotificationHost(bus);
            bus.receiveMirrored(shellEnvelope(WRITE_NOTICE_KIND_REFUSAL));

            const toast = toasts(host.element)[0];
            assertEqual(toast?.getAttribute("data-tone"), "bad", "a refusal is an error hue");
            // Still assertive — both kinds are moments design 10 makes non-negotiably loud.
            assertEqual(lane(toast!)?.getAttribute("role"), "alert", "and still interrupts");
            host.dispose();
        },
    },

    // ------------------------------- 6b. an ABANDONED gesture takes `wait` and says its own thing
    {
        name: "notifications: an abandoned gesture renders the wait hue with its OWN sentence",
        run: () => {
            // The end-to-end half of case 3, over the REAL arrival path a Shell publish takes. It
            // matters because the two halves fail independently: the pure functions could be right
            // while the host mis-hues (it reads `notice.kind`, which a parser regression can blank),
            // and the DOM could carry the right tone while showing the drop's sentence.
            const bus = new EditorUiBus({ origin: "0" });
            const host = createNotificationHost(bus);
            bus.receiveMirrored(shellEnvelope(WRITE_NOTICE_KIND_ABANDONED));

            assertEqual(host.shown, 1, "the abandonment was shown at all");
            const toast = toasts(host.element)[0];
            assert(toast !== undefined, "a toast is in the DOM");
            assertEqual(toast?.getAttribute("data-tone"), "wait", "an abandonment awaits the human");
            // Colour is NEVER the only signal (R-A11Y-001): the element's own text says what happened,
            // and it is the ABANDONMENT text rather than the drop's.
            assert(
                (toast?.textContent ?? "").includes("replaced the content you were editing"),
                "and the text says what happened, not just the hue",
            );
            assert(
                !(toast?.textContent ?? "").includes("someone else changed that field first"),
                "with the drop's story nowhere in it",
            );
            // Still assertive — design 10's LOUD invariant does not soften for the gentler hue.
            assertEqual(lane(toast!)?.getAttribute("role"), "alert", "and still interrupts");
            host.dispose();
        },
    },

    // ------------------------------------------------- 7. THE CONTROLS — what must NOT be rendered
    {
        name: "notifications: an unrelated topic, an unparseable payload, and a disposed host show nothing",
        run: () => {
            const bus = new EditorUiBus({ origin: "0" });
            const host = createNotificationHost(bus);

            // (a) ANOTHER TOPIC. A host that rendered every bus event would pass every case above
            // while toasting the human on each palette keystroke.
            bus.publish(UI_TOPIC_PALETTE, { open: true });
            assertEqual(host.shown, 0, "a palette fact is not a notification");
            assertEqual(toasts(host.element).length, 0, "and renders nothing");

            // (b) AN UNPARSEABLE PAYLOAD. `parseWriteNotice` answers null, and a null must produce NO
            // toast rather than an empty one — a blank notification is worse than none, because it
            // tells the human something went wrong and refuses to say what.
            bus.receiveMirrored({
                seq: 2,
                topic: UI_TOPIC_WRITE_NOTICE,
                origin: "shell",
                payload: "not-an-object",
            });
            assertEqual(host.shown, 0, "an unparseable payload shows nothing");

            // (c) DISPOSED. A real notice now WOULD render — so this proves the two controls above
            // failed for their own reasons, not because the host was inert all along.
            bus.receiveMirrored(shellEnvelope(WRITE_NOTICE_KIND_DROP, 3));
            assertEqual(host.shown, 1, "the host was live the whole time");

            host.dispose();
            bus.receiveMirrored(shellEnvelope(WRITE_NOTICE_KIND_DROP, 4));
            assertEqual(host.shown, 1, "a disposed host stops rendering");
        },
    },

    // ------------------------------------------------------ 8. the standing surface stays BOUNDED
    {
        name: "notifications: refusals past the cap retire the OLDEST, and never stop being shown",
        run: () => {
            // The kit ships no auto-dismiss timer on purpose, so nothing here expires — which makes
            // this host, the editor's first AUTOMATIC producer on that region, the one that can grow
            // the DOM without bound. A dead daemon refuses EVERY commit and every replay, so the
            // pathological run is an ordinary editing session, not an exotic one.
            const bus = new EditorUiBus({ origin: "0" });
            const host = createNotificationHost(bus);

            const burst = MAX_STANDING_NOTICES + 5;
            for (let i = 0; i < burst; i += 1) {
                bus.receiveMirrored(shellEnvelope(WRITE_NOTICE_KIND_DROP, i + 1));
            }

            // STILL LOUD: every one was shown and counted — the cap bounds what STANDS, never what
            // the human was told, which is the record `data-editor-notices` reports.
            assertEqual(host.shown, burst, "every refusal was shown");
            // BUT BOUNDED, on both the host's own handle and the real DOM.
            assertEqual(host.onScreen, MAX_STANDING_NOTICES, "the standing set is capped");
            assertEqual(
                toasts(host.element).length,
                MAX_STANDING_NOTICES,
                "and the capped ones really left the DOM, rather than only the counter moving",
            );
            // The survivors are the NEWEST — the refusals still worth acting on.
            assertEqual(host.last?.kind, WRITE_NOTICE_KIND_DROP, "the newest notice is retained");
            host.dispose();
        },
    },

    // ------------------------------------- 9. a notice that arrived BEFORE the host still reaches it
    {
        name: "notifications: a retained notice renders INTO AN ALREADY-MOUNTED region",
        run: () => {
            // A refused write while the dock is still materialising is not less important than one a
            // second later, so the host must render the envelope the bus retained before it existed
            // (uibus.ts property 2). This case keeps that dependency deliberate — and pins the ORDER it
            // rests on, which is the half that is easy to get wrong and invisible once wrong.
            //
            // The retained envelope is delivered SYNCHRONOUSLY inside `subscribe`, so this is the one
            // path where the toast exists before any caller could have appended the region. Asserting
            // only "it rendered" would pass identically against a region still DETACHED from the
            // document — a toast on screen that no screen reader ever announces, because AT begins
            // observing a live region when it is INSERTED and this one's message would predate the
            // insert (feedback.ts header). So the region records whether it was connected at show time,
            // which is the only way this distinction is observable after the fact.
            const bus = new EditorUiBus({ origin: "0" });
            bus.receiveMirrored(shellEnvelope(WRITE_NOTICE_KIND_DROP));

            const inner = createToastRegion();
            const connectedAtShow: boolean[] = [];
            const region: KitToastRegion = {
                element: inner.element,
                show: (options) => {
                    connectedAtShow.push(inner.element.isConnected);
                    return inner.show(options);
                },
                get count(): number {
                    return inner.count;
                },
            };

            const mount = document.createElement("div");
            document.body.append(mount);
            try {
                const host = createNotificationHost(bus, { region, mount });
                assertEqual(host.shown, 1, "the retained notice was delivered on subscribe");
                assertEqual(toasts(host.element).length, 1, "and rendered");
                assertEqual(connectedAtShow.length, 1, "rendered exactly once");
                assert(
                    connectedAtShow[0] === true,
                    "the region was ALREADY in the document when the retained notice rendered into " +
                        "it — a live region inserted with its message already inside is never spoken",
                );
                host.dispose();
            } finally {
                mount.remove();
            }
        },
    },
];
