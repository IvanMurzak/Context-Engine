// The BUTTONS family (M9 e06c2; design 06 §3). The first of the two families that had to be built on
// an e06c1 role primitive rather than beside it.
//
// THE REUSE IS THE WHOLE DESIGN. An authored button is a real `<button>` carrying
// `WIDGET_CLASSES.button` — the SAME class `uitree::render_html` puts on a C++ panel's button node —
// plus `ctx-button`, which adds LAYOUT ONLY (inline-flex, a minimum control height, the icon gap).
// So every paint decision (ink, chip surface, line border, radius, the focus ring) is decided in
// exactly one place, `styles/kit.css`, for both kinds of button at once, and a theme switch moves
// them together with no component change. `kit_components.test.ts` asserts that equality on the
// COMPUTED style of an authored button against a hand-built hydration button — the one form of the
// claim a reader cannot talk themselves out of.
//
// TONES ADD PAINT, AND ONLY TONES DO. `primary` and `danger` are the two states the editor actually
// needs a button to signal (a confirm action; a destructive one). They are `data-tone` attributes
// rather than extra classes so the stylesheet keeps one selector list, and the default tone declares
// NOTHING — which is what keeps "the authored default and the hydrated widget are the same button"
// true by construction rather than by discipline.

import { createElement, setText } from "./dom.js";
import { requireWidgetClass } from "./widgets.js";

/** The family's root class (also declared in `index.ts`'s roster, which the coverage gate reads). */
export const BUTTON_CLASS = "ctx-button";

/**
 * A button's emphasis.
 *
 * THREE, deliberately: `default` (the hydration widget's own appearance), `primary` (one per
 * surface — the accent-filled confirm) and `danger` (destructive, bordered in the reserved `bad`
 * hue). A fourth would need either a colour the theme does not publish or an overload of a reserved
 * status semantic (06 §2).
 */
export type ButtonTone = "default" | "primary" | "danger";

export interface ButtonOptions {
    readonly label: string;
    readonly tone?: ButtonTone;
    /** A `title` + `aria-label` for an icon-only button; omit for a labelled one. */
    readonly accessibleLabel?: string;
    readonly disabled?: boolean;
    /** The id of the command this button dispatches, mirrored to `data-command` for the a11y scan. */
    readonly commandId?: string;
    readonly onActivate?: () => void;
}

export interface KitButton {
    readonly element: HTMLButtonElement;
    setLabel(label: string): void;
    setDisabled(disabled: boolean): void;
    setTone(tone: ButtonTone): void;
}

/**
 * Build a themed button.
 *
 * `type="button"` is not a detail: the HTML default is `submit`, and a default-type button inside any
 * future `<form>` (a Settings panel field group is the obvious one, e06d) would submit and navigate
 * the trusted editor-core document away from `context-editor://app/index.html` — a blank editor, from
 * a one-word omission.
 */
export function createButton(options: ButtonOptions): KitButton {
    const element = createElement("button", requireWidgetClass("button"), BUTTON_CLASS);
    element.type = "button";
    setText(element, options.label);
    element.setAttribute("data-tone", options.tone ?? "default");
    if (options.accessibleLabel !== undefined) {
        element.setAttribute("aria-label", options.accessibleLabel);
        element.title = options.accessibleLabel;
    }
    if (options.commandId !== undefined) {
        element.setAttribute("data-command", options.commandId);
    }
    element.disabled = options.disabled === true;
    const onActivate = options.onActivate;
    if (onActivate !== undefined) {
        // A `click` listener, NOT a key handler: the browser already synthesises `click` from Enter
        // and Space on a real `<button>`, so routing activation through it is what makes the control
        // keyboard-operable without a single keystroke of our own (05 §6's "no raw key handlers"
        // holds here by construction rather than by a marker).
        element.addEventListener("click", () => {
            onActivate();
        });
    }
    return {
        element,
        setLabel: (label: string): void => {
            setText(element, label);
        },
        setDisabled: (disabled: boolean): void => {
            element.disabled = disabled;
        },
        setTone: (tone: ButtonTone): void => {
            element.setAttribute("data-tone", tone);
        },
    };
}
