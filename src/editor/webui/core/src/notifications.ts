// The EDITOR-WIDE NOTIFICATION HOST (M9 e09b-3, design 05 §8, 06 §2-§3, 10 § UX invariants).
//
// WHAT IT IS FOR. e09b-1/e09b-2 made a concurrent-writer collision DROP the human's edit instead of
// clobbering the co-writer, and e09c did the same for an undo replay. Both refusals were counted and
// written to stderr — which in a GUI is indistinguishable from silence. Design 05 §8 states the
// requirement in one line ("drop LOUDLY + notification + editor.ui fact") and design 10 makes it
// non-negotiable ("Destructive/lossy moments (gesture drop, daemon lost, panel crash) are LOUD
// (wait/bad hues), never silent"). This module is the notification + wait-hue two thirds of that; the
// `editor.ui` fact is the topic it subscribes to.
//
// WHY THE HOST IS DRIVEN BY THE BUS AND NOT BY THE TRANSPORT. The notice arrives from the SHELL, over
// the `ui.mirror` relay, and `UiMirrorPoller` already applies every mirrored envelope to the bus. So
// this module subscribes to a topic and knows nothing about how the fact got here — which is what
// keeps it (a) free of editor-core's exit, so `tools/check_ui_bus_boundary.py`'s rule 2 stays
// trivially satisfied, and (b) drivable at T1 by publishing on a bare bus with no Shell at all.
//
// WHY THE PROSE IS WRITTEN HERE AND NOT IN C++. The Shell puts FACTS on the wire (kind / action /
// code / message / pointer); the sentence a human reads is composed here, where the copy, the tone
// mapping and the accessible naming already live. A sentence assembled in C++ would be a second place
// user-facing text has to be reviewed, and the one place nobody would look for it.
//
// THE HUE IS THE MESSAGE, and the two are bound 1:1 by design 06 §2's reserved semantics:
//
//   * a DROP is `wait` — "awaiting-human". Nothing was lost and nothing was overwritten; the edit
//     simply did not land and must be re-made against the value that is there now. It is not an
//     error, and colouring it as one would teach the human that the editor breaks under co-editing
//     when in fact it protected them.
//   * a REFUSAL is `bad` — the write path said no (no daemon, an unreadable field). Nothing was
//     written, and the thing to do about it is not "re-apply" but "wait for the project to be
//     reachable".
//
// ⚠ COLOUR IS NEVER THE ONLY SIGNAL (R-A11Y-001). Every notice carries its meaning in TEXT — the
// headline names what was refused and why — and is announced through the kit's ASSERTIVE live region
// (`assertive: true`), so a screen-reader user gets the same event at the same moment as a sighted
// one. The kit's default lane derivation would put `wait` in the polite lane, which for this surface
// would be silence for exactly the users who can least afford it; `ToastOptions.assertive` exists for
// that reason and this is its first caller.

import {
    createToastRegion,
    type KitToast,
    type KitToastRegion,
    type SemanticTone,
} from "../../kit/src/index.js";
import { isRecord } from "./bridge.js";
import { UI_TOPIC_WRITE_NOTICE, type EditorUiBus, type EditorUiSubscription } from "./uibus.js";

// --------------------------------------------------------------------------- the wire vocabulary
// MUST match write_notice.h's `kWriteNoticeKind*`. Cross-checked byte-for-byte out of the BUILT
// bundle by `tools/check_webui_assets.py --panel-contract`, the same drift gate the `ui.mirror` /
// `window.*` / `session.*` surfaces ride: a rename on either side makes every notice fall through to
// the unknown-kind default below, which would silently mis-hue a data-integrity moment — the failure
// mode is a WRONG message rather than a missing one, which is strictly harder to notice.

/** An L-30 concurrent-writer DROP: the edit was refused, never overwritten. */
export const WRITE_NOTICE_KIND_DROP = "drop";
/** A write-PATH refusal: no daemon, an unreadable field, a compose refusal. Nothing was written. */
export const WRITE_NOTICE_KIND_REFUSAL = "refusal";

/** One refused write, as the Shell put it on the wire. */
export interface WriteNotice {
    /** `WRITE_NOTICE_KIND_DROP` or `WRITE_NOTICE_KIND_REFUSAL`; anything else reads as a refusal. */
    readonly kind: string;
    /** What the human tried to do — "edit", "undo", "redo". Free prose from the Shell. */
    readonly action: string;
    /** The catalog code the write path answered with (`cas.mismatch`, `shell.no_daemon`, …). */
    readonly code: string;
    /** The engine's own diagnostic, shown as supporting detail. */
    readonly message: string;
    /** The field pointer the refused write targeted. Empty when there was none. */
    readonly pointer: string;
}

/**
 * Narrow one `editor.ui.write-notice` payload, total against anything.
 *
 * TOTAL AND LOSSY-CLOSED: a member of the wrong type reads as its neutral default rather than
 * throwing, because this runs inside a bus fan-out where a throw would cost every OTHER subscriber
 * the fact. `null` only for a non-object — a payload that is not a record carries nothing to show,
 * and inventing a notice for it would put an empty toast on screen.
 *
 * An UNKNOWN `kind` is deliberately KEPT rather than normalised, so `writeNoticeTone` can apply the
 * fail-loud default in one place instead of two.
 */
export function parseWriteNotice(value: unknown): WriteNotice | null {
    if (!isRecord(value)) {
        return null;
    }
    const text = (key: string): string => {
        const member = value[key];
        return typeof member === "string" ? member : "";
    };
    return {
        kind: text("kind"),
        action: text("action"),
        code: text("code"),
        message: text("message"),
        pointer: text("pointer"),
    };
}

/**
 * The hue for a notice kind (06 §2's reserved semantics — see the module header).
 *
 * AN UNKNOWN KIND IS `bad`, not `wait`. If the two sides ever drift, the safe direction is to
 * over-state the severity: telling a human their write hit an error when it was really a co-writer
 * collision costs them a second look, while telling them "awaiting you" when the project is actually
 * unreachable sends them re-applying an edit that cannot land.
 */
export function writeNoticeTone(kind: string): SemanticTone {
    return kind === WRITE_NOTICE_KIND_DROP ? "wait" : "bad";
}

/**
 * The sentence the human reads.
 *
 * It states, in this order, WHAT was refused, WHY, and WHAT TO DO — because a notification that names
 * only the failure leaves the reader to guess whether their work survived. The field pointer is
 * included when there is one: on a dense inspector "your edit was dropped" is not actionable and
 * "/components/camera/fov was dropped" is.
 */
export function writeNoticeHeadline(notice: WriteNotice): string {
    const what = notice.action === "" ? "write" : notice.action;
    const where = notice.pointer === "" ? "" : ` to ${notice.pointer}`;
    if (notice.kind === WRITE_NOTICE_KIND_DROP) {
        return (
            `Your ${what}${where} was not applied — someone else changed that field first. ` +
            `Nothing was overwritten; re-apply it against the current value.`
        );
    }
    return `Your ${what}${where} could not be saved. Nothing was written.`;
}

/** The full accessible text of one notice: the headline, plus the engine's own diagnostic. */
export function writeNoticeText(notice: WriteNotice): string {
    const headline = writeNoticeHeadline(notice);
    return notice.message === "" ? headline : `${headline} (${notice.message})`;
}

/** A live notification host. `dispose` detaches it from the bus; the region stays where it was put. */
export interface NotificationHost {
    /** The toast region's root — mount it once per window. */
    readonly element: HTMLElement;
    /** How many notices this host has SHOWN (cumulative; a dismissal does not decrease it). */
    readonly shown: number;
    /** How many are on screen right now. */
    readonly onScreen: number;
    /** The last notice rendered, or `null` before the first. */
    readonly last: WriteNotice | null;
    dispose(): void;
}

/**
 * How many refused-write toasts stay on screen at once, oldest retired first.
 *
 * Sized to be reached only by a genuinely pathological run (a dead daemon refusing every gesture), so
 * an ordinary burst of two or three collisions is never truncated — the cap is a leak bound, not a
 * display policy.
 */
export const MAX_STANDING_NOTICES = 8;

export interface NotificationHostOptions {
    /**
     * The toast region to render into. Defaults to a fresh one — the ordinary case, since the editor
     * has exactly one notification host. Injectable so a test can drive a region it also inspects.
     */
    readonly region?: KitToastRegion;
    /**
     * Where to mount the region. Supplied here rather than appended by the caller afterwards BECAUSE
     * THE ORDER IS LOAD-BEARING — see `createNotificationHost`.
     */
    readonly mount?: HTMLElement;
}

/**
 * Attach an editor-wide notification host to `bus`.
 *
 * ONE per window, created at boot. It subscribes to the write-notice topic and renders every fact
 * that arrives — including the RETAINED one, if a notice landed before this host existed: the bus
 * hands a late subscriber its topic's snapshot immediately (uibus.ts property 2), which is exactly
 * right here. A refused write that happened while the dock was still materialising is not less
 * important than one that happens a second later.
 *
 * ⚠ THE REGION IS MOUNTED BEFORE `subscribe`, AND THAT ORDER IS THE ONE THING THIS FUNCTION MUST NOT
 * GET WRONG. `subscribe` delivers the retained envelope SYNCHRONOUSLY, inside the call — so a host
 * that mounted afterwards would run `region.show()` into a region still detached from the document,
 * and the caller would then insert a lane that ALREADY CONTAINS its message. feedback.ts's header
 * names that exact shape as "the single most common way a toast ends up silent": assistive technology
 * begins observing a live region when it is inserted, so a mutation that happened before the insert is
 * never announced. The toast would be on screen and unspoken — colour as the only signal, which is
 * precisely the failure design 10's LOUD invariant forbids. Hence `mount` is an OPTION rather than an
 * append the caller makes after this returns: the ordering lives with the code that depends on it.
 */
export function createNotificationHost(
    bus: EditorUiBus,
    options: NotificationHostOptions = {},
): NotificationHost {
    const region = options.region ?? createToastRegion();
    options.mount?.append(region.element);
    let shown = 0;
    let last: WriteNotice | null = null;
    const standing: KitToast[] = [];

    const subscription: EditorUiSubscription = bus.subscribe(
        UI_TOPIC_WRITE_NOTICE,
        (event): void => {
            const notice = parseWriteNotice(event.payload);
            if (notice === null) {
                return; // nothing to show; see parseWriteNotice on why this is not an empty toast
            }
            standing.push(
                region.show({
                    message: writeNoticeText(notice),
                    tone: writeNoticeTone(notice.kind),
                    // ALWAYS assertive — see the module header. Both kinds are moments design 10
                    // makes non-negotiably loud, and the kit's tone-derived default would leave a
                    // `wait` notice in the polite lane, i.e. silent for a screen-reader user who is
                    // looking elsewhere.
                    assertive: true,
                }),
            );
            // BOUNDED. The kit deliberately ships no auto-dismiss TIMER (a toast that vanishes on its
            // own is unreadable for anyone who reads slowly or looked away — WCAG "enough time"), and
            // that is untouched here: nothing expires, the human still dismisses. But this host is the
            // editor's first AUTOMATIC producer on that region — every other caller is user-initiated
            // — and its worst case is systemic rather than exotic: with the daemon down the gateway
            // refuses EVERY commit and every replay, so one toast per gesture accumulates for the life
            // of the window, none of which anyone is likely to dismiss one by one. Retiring the OLDEST
            // past a cap keeps the surface loud (the newest refusals are the ones still actionable)
            // without the unbounded DOM growth uibus.ts's own retention note argues against. `shown`
            // stays cumulative, so the "the editor DID tell them" record is unaffected.
            while (standing.length > MAX_STANDING_NOTICES) {
                standing.shift()?.dismiss();
            }
            // COUNTED ONLY AFTER THE RENDER RETURNS. `shown` is cumulative precisely so that "the
            // editor DID tell them" stays answerable once a dismissed toast has left no pixels
            // (boot.ts NOTICES_ATTRIBUTE) — a tally incremented before the render would answer that
            // question with a claim it cannot back, since the bus swallows a throwing subscriber
            // (uibus.ts #invoke) and nothing else would record that the notice never landed.
            last = notice;
            shown += 1;
        },
    );

    return {
        element: region.element,
        get shown(): number {
            return shown;
        },
        get onScreen(): number {
            return region.count;
        },
        get last(): WriteNotice | null {
            return last;
        },
        dispose(): void {
            subscription.dispose();
        },
    };
}
