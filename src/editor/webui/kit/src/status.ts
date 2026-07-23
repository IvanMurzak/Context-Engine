// The CHIPS and BADGES families (M9 e06c2; design 06 §3) — the small labelled pills that carry the
// editor's reserved status semantics (06 §2: good / warn / bad / wait / idle, "the only chroma").
//
// ONE APPEARANCE, TWO SEMANTICS — the decision this module exists to make explicitly.
//
//   * A CHIP is an interactive or removable token: a filter, a selected tag, an applied facet. It is
//     a real `<button>` when it does something, so it is keyboard-reachable with no ARIA of our own.
//   * A BADGE is a READ-OUT: a count, a state word. Its default form is an inert `<span>`.
//
// ⚠ WHY A BADGE IS NOT ALWAYS THE `status` ROLE PRIMITIVE — a recorded exception with its reason.
// e06c1 publishes a `status` role (`<output class="ctx-widget-status">`), and design 06 §2 names the
// "problems badge" among the surfaces that bind to the status semantics, so the obvious reading is
// "every badge is a `ctx-widget-status`". That reading is wrong, and quietly harmful: `<output>` is
// an ARIA LIVE REGION, so every badge built that way would announce itself to a screen reader on
// every change. An editor is full of badges that change constantly for cosmetic reasons (a tab's
// count, a chip's tally) and a user running a screen reader would be read a stream of numbers nobody
// asked for — the accessibility equivalent of a notification storm.
//
// So the family REUSES the primitive where the semantics are real and opts out where they are not:
// `createBadge({ live: true })` — the Problems count, a derivation state — renders the actual
// `ctx-widget-status` element, inheriting e06c1's muted ink step and every future change to it, while
// the default renders a `<span>` with the SAME painted appearance. The shared appearance is enforced
// by CSS rather than by duplication: the live form is selected as `.ctx-widget-status.ctx-badge`
// (specificity 0,2,0), which beats `.ctx-widget-status` no matter which stylesheet the browser
// reaches last — the "do not let document order decide appearance" rule e06c1's one-styling-owner
// gate exists to enforce, honoured here without needing a gate to notice.

import { applyTone, createElement, setText, type SemanticTone } from "./dom.js";
import { requireWidgetClass } from "./widgets.js";

/** The roster root classes (mirrored in `index.ts`'s family table). */
export const CHIP_CLASS = "ctx-chip";
export const BADGE_CLASS = "ctx-badge";

export interface ChipOptions {
    readonly label: string;
    readonly tone?: SemanticTone;
    /** Makes the chip itself a button. */
    readonly onActivate?: () => void;
    /** Adds a dedicated remove control (a chip may be both activatable and removable). */
    readonly onRemove?: () => void;
    /** The accessible name of the remove control; defaults to `Remove <label>`. */
    readonly removeLabel?: string;
    readonly selected?: boolean;
}

export interface KitChip {
    readonly element: HTMLElement;
    setLabel(label: string): void;
    setSelected(selected: boolean): void;
}

/**
 * Build a chip.
 *
 * A chip with `onActivate` is a `<button>`; a chip with only `onRemove` is a `<span>` CONTAINING a
 * button. The nesting matters: a button inside a button is invalid HTML and browsers recover from it
 * unpredictably, so an activatable AND removable chip is rendered as a span wrapper holding two
 * buttons rather than nesting them.
 */
export function createChip(options: ChipOptions): KitChip {
    const interactive = options.onActivate !== undefined;
    const removable = options.onRemove !== undefined;
    const element = createElement("span", CHIP_CLASS);
    applyTone(element, options.tone ?? "neutral");

    const label = createElement("span", "ctx-chip__label");
    setText(label, options.label);

    if (interactive) {
        const button = createElement("button", "ctx-chip__action");
        button.type = "button";
        button.append(label);
        const onActivate = options.onActivate;
        button.addEventListener("click", () => {
            onActivate?.();
        });
        element.append(button);
    } else {
        element.append(label);
    }

    if (removable) {
        const remove = createElement("button", "ctx-chip__remove");
        remove.type = "button";
        // An accessible name is MANDATORY here, not optional: the visible content is a glyph drawn by
        // the stylesheet, so without it the control announces as an unlabelled button and a screen
        // reader user cannot tell which chip it removes.
        remove.setAttribute("aria-label", options.removeLabel ?? `Remove ${options.label}`);
        const onRemove = options.onRemove;
        remove.addEventListener("click", () => {
            onRemove?.();
        });
        element.append(remove);
    }

    function setSelected(selected: boolean): void {
        element.classList.toggle("ctx-chip--selected", selected);
        const button = element.querySelector<HTMLElement>(".ctx-chip__action");
        // `aria-pressed` only on a real button — the toggle state of an inert span is meaningless and
        // an ARIA state on a non-widget is worse than none.
        button?.setAttribute("aria-pressed", selected ? "true" : "false");
    }

    if (interactive) {
        setSelected(options.selected === true);
    }

    return {
        element,
        setLabel: (next: string): void => {
            setText(label, next);
        },
        setSelected,
    };
}

export interface BadgeOptions {
    readonly label: string;
    readonly tone?: SemanticTone;
    /**
     * Render the e06c1 `status` role primitive (an `<output>` ARIA live region) instead of an inert
     * `<span>`, so a CHANGE to this badge is announced.
     *
     * Off by default, on purpose — see the module header. Turn it on only for a badge whose change is
     * genuinely worth interrupting a screen-reader user for (the Problems count, a build verdict).
     */
    readonly live?: boolean;
    /** Names the badge when its text alone is not self-explaining (`"7"` -> `"7 problems"`). */
    readonly accessibleLabel?: string;
}

export interface KitBadge {
    readonly element: HTMLElement;
    setLabel(label: string): void;
    setTone(tone: SemanticTone): void;
}

/** Build a badge. `live: true` reuses the `status` role primitive (see the module header). */
export function createBadge(options: BadgeOptions): KitBadge {
    const live = options.live === true;
    const element: HTMLElement = live
        ? createElement("output", requireWidgetClass("status"), BADGE_CLASS)
        : createElement("span", BADGE_CLASS);
    if (live) {
        // `role="status"` is `<output>`'s implicit role; stating it is defence against a future
        // element swap silently dropping the politeness, and it is what the a11y scan reads.
        element.setAttribute("role", "status");
        element.setAttribute("aria-live", "polite");
    }
    applyTone(element, options.tone ?? "neutral");
    setText(element, options.label);
    if (options.accessibleLabel !== undefined) {
        element.setAttribute("aria-label", options.accessibleLabel);
    }
    return {
        element,
        setLabel: (label: string): void => {
            setText(element, label);
        },
        setTone: (tone: SemanticTone): void => {
            applyTone(element, tone);
        },
    };
}
