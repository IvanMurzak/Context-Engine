// The TOASTS, EMPTY-STATES and SKELETONS families (M9 e06c2; design 06 §3) — the three surfaces that
// tell a user what the editor is doing when the content they wanted is absent, pending, or has just
// changed underneath them.
//
// ⚠ A LIVE REGION'S POLITENESS MUST BE DECIDED BEFORE ITS CONTENT ARRIVES, WHICH IS WHY THE TOAST
// REGION IS A PAIR OF STANDING CONTAINERS RATHER THAN AN ELEMENT PER TOAST. Assistive technology
// starts OBSERVING an element when it is inserted with a live role already on it; inserting an
// element that carries `role="alert"` and its message at the same time is the single most common way
// a toast ends up silent, because the observer is attached to a subtree whose mutation has already
// happened. So this module mounts two empty, permanently-present regions — one `polite`
// (`role="status"`) and one `assertive` (`role="alert"`) — and appends messages INTO them. That also
// gets the severity split right for free: an error interrupts, a success does not.
//
// SKELETONS DO NOT ANIMATE, AND THAT IS A RECORDED FINDING RATHER THAN A SHORTCUT. A shimmer needs a
// long-loop duration of roughly 1.2-1.6s. e06a's `motion.duration` scale publishes the MICRO-
// transition steps (fast/base/slow, 120-160ms) plus the 350ms theme cross-fade, and the only longer
// values in the schema are `motion.flourish.states.*.duration`, which are Pulse-of-Work PLAY-STATE
// semantics (06 §2) and mean something specific. Animating a skeleton at 160ms would be a strobe — an
// accessibility hazard, not a design choice — and `calc(var(--ctx-motion-duration-slow) * 10)` would
// be a raw value wearing a token's clothes, exactly the bypass the tokens-only lint's `var()`-
// fallback rule exists to refuse. So the skeleton ships as a STATIC surface step, and the missing
// token is written down in `../README.md` § Known gaps for e06d/e16 to adopt.

import { createElement, setText, type SemanticTone } from "./dom.js";
import { createButton, type ButtonOptions } from "./button.js";

/** The roster root classes (mirrored in `index.ts`'s family table). */
export const TOAST_CLASS = "ctx-toast";
export const EMPTY_STATE_CLASS = "ctx-empty-state";
export const SKELETON_CLASS = "ctx-skeleton";

// ------------------------------------------------------------------------------------------ toasts

export interface ToastOptions {
    readonly message: string;
    readonly tone?: SemanticTone;
    /** An inline action ("Undo", "Show log"). Rendered as a kit button, so it is keyboard-reachable. */
    readonly action?: ButtonOptions;
}

export interface KitToast {
    readonly element: HTMLElement;
    dismiss(): void;
}

export interface KitToastRegion {
    /** Mount this once per window. Both live regions live inside it. */
    readonly element: HTMLElement;
    show(options: ToastOptions): KitToast;
    /** How many toasts are currently on screen — the falsifiable handle a test needs. */
    readonly count: number;
}

/**
 * Build the toast region.
 *
 * NO AUTO-DISMISS TIMER, deliberately. A toast that vanishes on its own is unreadable for anyone who
 * reads slowly, uses a screen reader, or looked away — WCAG's "enough time" requirement in practice —
 * and a kit that baked a timeout in would make that undoable by its consumers. Dismissal is the
 * caller's decision (`KitToast.dismiss`) or the user's (the close control).
 */
export function createToastRegion(): KitToastRegion {
    const element = createElement("div", `${TOAST_CLASS}-region`);
    // Not a live region ITSELF: nesting live regions makes announcements duplicate unpredictably.
    element.setAttribute("role", "presentation");

    const polite = createElement("div", `${TOAST_CLASS}-region__lane`);
    polite.setAttribute("role", "status");
    polite.setAttribute("aria-live", "polite");
    const assertive = createElement("div", `${TOAST_CLASS}-region__lane`);
    assertive.setAttribute("role", "alert");
    assertive.setAttribute("aria-live", "assertive");
    element.append(polite, assertive);

    let count = 0;

    function show(options: ToastOptions): KitToast {
        const tone = options.tone ?? "neutral";
        const toast = createElement("div", TOAST_CLASS);
        toast.setAttribute("data-tone", tone);

        const message = createElement("span", "ctx-toast__message");
        setText(message, options.message);
        toast.append(message);

        if (options.action !== undefined) {
            toast.append(createButton(options.action).element);
        }

        const close = createButton({
            label: "",
            accessibleLabel: "Dismiss notification",
            onActivate: (): void => {
                dismiss();
            },
        });
        close.element.classList.add("ctx-toast__close");
        toast.append(close.element);

        // The severity split: only `bad` interrupts. `warn` means ACTIVE WORK in this design system
        // (06 §2), not danger, so it stays polite — an assertive announcement for every build step
        // would make the editor unusable with a screen reader.
        (tone === "bad" ? assertive : polite).append(toast);
        count += 1;

        let dismissed = false;
        function dismiss(): void {
            if (dismissed) {
                return;
            }
            dismissed = true;
            toast.remove();
            count -= 1;
        }
        return { element: toast, dismiss };
    }

    return {
        element,
        show,
        get count(): number {
            return count;
        },
    };
}

// ------------------------------------------------------------------------------------ empty states

export interface EmptyStateOptions {
    readonly title: string;
    readonly description?: string;
    /** The one thing the user can do about it ("Open a project", "Add a component"). */
    readonly action?: ButtonOptions;
}

export interface KitEmptyState {
    readonly element: HTMLElement;
}

/**
 * Build an empty state.
 *
 * The title is a `<p>`, NOT a heading. A kit component cannot know its heading LEVEL — the same empty
 * state appears inside a panel body (where `<h2>` is right) and inside a dialog (where it is not) —
 * and a wrong level is worse than no heading at all, because it corrupts the document outline a
 * screen-reader user navigates by. A caller that genuinely wants a heading wraps one around it.
 */
export function createEmptyState(options: EmptyStateOptions): KitEmptyState {
    const element = createElement("div", EMPTY_STATE_CLASS);
    const mark = createElement("div", "ctx-empty-state__mark");
    mark.setAttribute("aria-hidden", "true");
    element.append(mark);

    const title = createElement("p", "ctx-empty-state__title");
    setText(title, options.title);
    element.append(title);

    if (options.description !== undefined) {
        const description = createElement("p", "ctx-empty-state__description");
        setText(description, options.description);
        element.append(description);
    }
    if (options.action !== undefined) {
        element.append(createButton(options.action).element);
    }
    return { element };
}

// -------------------------------------------------------------------------------------- skeletons

export interface SkeletonOptions {
    /** How many placeholder lines to draw. Defaults to three. */
    readonly lines?: number;
    /**
     * When given, the skeleton becomes a polite `status` NAMED by this string ("Loading panels").
     *
     * Off by default: the placeholder bars themselves are decorative and are always `aria-hidden`, so
     * a screen-reader user meets one named region instead of N unlabelled boxes.
     *
     * ⚠ THE NAME IS NOT A GUARANTEED ANNOUNCEMENT, and the reason is the rule stated at the top of
     * this module. A live region announces CONTENT CHANGES observed after it is registered; this one
     * is mounted complete, and its label is an `aria-label` rather than content — the same shape the
     * toast region deliberately avoids. So a user who navigates INTO the region always hears the
     * name, but a user elsewhere on the page may hear nothing. That is an honest read-out of what the
     * DOM does, not a claim of parity with the toast lanes; the fix needs a standing region owned by
     * the CALLER, which is a contract change and is recorded in ../README.md § Recorded findings.
     */
    readonly busyLabel?: string;
}

export interface KitSkeleton {
    readonly element: HTMLElement;
}

/** Build a loading placeholder. Static by design — see the module header's recorded finding. */
export function createSkeleton(options: SkeletonOptions = {}): KitSkeleton {
    const element = createElement("div", SKELETON_CLASS);
    if (options.busyLabel === undefined) {
        element.setAttribute("aria-hidden", "true");
    } else {
        element.setAttribute("role", "status");
        element.setAttribute("aria-live", "polite");
        element.setAttribute("aria-label", options.busyLabel);
    }
    const lines = Math.max(1, options.lines ?? 3);
    for (let index = 0; index < lines; index += 1) {
        const line = createElement("div", "ctx-skeleton__line");
        line.setAttribute("aria-hidden", "true");
        // Three repeating widths so a placeholder block reads as text rather than as a solid slab.
        // Discrete VARIANTS rather than a computed width, because a computed width would have to be
        // written as an inline style — the one thing a kit component may never do.
        line.setAttribute("data-width", index % 3 === 2 ? "short" : index % 3 === 1 ? "long" : "full");
        element.append(line);
    }
    return { element };
}
