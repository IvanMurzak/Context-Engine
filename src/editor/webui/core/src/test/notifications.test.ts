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
// The last one is the a11y coverage this task owes: `wait` had no production consumer before e09b-3,
// so its accessible behaviour is net-new and is asserted here rather than assumed from the token.

import { assert, assertEqual, assertNull, type TestCase } from "./harness.js";
import {
    createNotificationHost,
    parseWriteNotice,
    writeNoticeHeadline,
    writeNoticeText,
    writeNoticeTone,
    WRITE_NOTICE_KIND_DROP,
    WRITE_NOTICE_KIND_REFUSAL,
} from "../notifications.js";
import {
    EditorUiBus,
    UI_TOPIC_PALETTE,
    UI_TOPIC_WRITE_NOTICE,
    type EditorUiEvent,
} from "../uibus.js";

/** The payload the Shell puts on the wire (write_notice.cpp `write_notice_envelope`). */
function payload(kind: string, overrides: Record<string, unknown> = {}): Record<string, unknown> {
    return {
        kind,
        action: "edit",
        code: kind === WRITE_NOTICE_KIND_DROP ? "cas.mismatch" : "shell.no_daemon",
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

    // ------------------------------------- 8. a notice that arrived BEFORE the host still reaches it
    {
        name: "notifications: a notice published before the host existed is still shown (snapshot)",
        run: () => {
            // A refused write while the dock is still materialising is not less important than one a
            // second later. The bus hands a late subscriber its topic's retained envelope immediately
            // (uibus.ts property 2), and this is the case that keeps that dependency deliberate.
            const bus = new EditorUiBus({ origin: "0" });
            bus.receiveMirrored(shellEnvelope(WRITE_NOTICE_KIND_DROP));

            const host = createNotificationHost(bus);
            assertEqual(host.shown, 1, "the retained notice was delivered on subscribe");
            assertEqual(toasts(host.element).length, 1, "and rendered");
            host.dispose();
        },
    },
];
