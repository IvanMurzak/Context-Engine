// The DIALOGS and TOOLTIPS families (M9 e06c2; design 06 §3) — the two overlay surfaces the task
// singles out because "they need focus handling that a later audit cannot retrofit cheaply".
//
// THE DIALOG IS A NATIVE `<dialog>` DRIVEN BY `showModal()`, NOT A DIV WITH A FOCUS TRAP. That is the
// same trade `kit.css` already makes for the checkbox (`accent-color` over rebuilding the control out
// of pseudo-elements): the platform's implementation is better than the one a kit would write, and
// re-implementing it is how a11y bugs get written. `showModal()` gives, for free and correctly:
//
//   * the TOP LAYER, so no z-index arms race with dockview's floating groups;
//   * INERTNESS of everything outside the dialog — not a Tab-cycling trap that a screen reader's
//     virtual cursor walks straight past, but real inertness the browser enforces for every input
//     mode at once. `kit_components.test.ts` asserts exactly that, by calling `.focus()` on an
//     element OUTSIDE an open dialog and requiring that focus does not move;
//   * the `::backdrop` pseudo-element, so the scrim needs no extra DOM.
//
// What this module adds on top is the part the platform leaves to the author, and both halves are
// asserted: a DETERMINISTIC initial focus (the first focusable inside, rather than whatever the UA
// picks), and focus RESTORATION to the element that opened the dialog.
//
// ESCAPE IS HANDLED TWICE, ON PURPOSE. The native `cancel` event is the production path. A `keydown`
// listener duplicates it because a SYNTHETIC `KeyboardEvent` — the only kind a T1 test can dispatch —
// does not drive native browser behaviour, so an Escape contract that existed only natively would be
// untestable, and an untestable a11y contract is one that silently regresses. `close()` is idempotent,
// so the two paths cannot double-fire.

import { createButton, type ButtonOptions } from "./button.js";
import { createElement, focusableWithin, setText, uniqueId } from "./dom.js";

/** The roster root classes (mirrored in `index.ts`'s family table). */
export const DIALOG_CLASS = "ctx-dialog";
export const TOOLTIP_CLASS = "ctx-tooltip";

// ----------------------------------------------------------------------------------------- dialogs

/** Why a dialog closed. Carried to `onClose` so a caller can distinguish confirm from dismissal. */
export type DialogCloseReason = "confirm" | "cancel" | "dismiss" | "programmatic";

export interface DialogOptions {
    readonly title: string;
    readonly description?: string;
    /** The dialog's body. A Node, so it can be any kit component (a field stack, a table). */
    readonly content?: Node;
    /** The primary action. Rendered `primary`-toned unless the caller says otherwise. */
    readonly confirm?: ButtonOptions;
    /** The dismissing action's label; omit for a dialog with no explicit cancel. */
    readonly cancelLabel?: string;
    readonly onClose?: (reason: DialogCloseReason) => void;
}

export interface KitDialog {
    readonly element: HTMLDialogElement;
    readonly isOpen: boolean;
    open(): void;
    close(reason?: DialogCloseReason): void;
}

/** Build a modal dialog. Mount `element` once; `open()`/`close()` drive it thereafter. */
export function createDialog(options: DialogOptions): KitDialog {
    const element = createElement("dialog", DIALOG_CLASS);
    const titleId = uniqueId("ctx-dialog-title");
    element.setAttribute("aria-labelledby", titleId);

    const surface = createElement("div", "ctx-dialog__surface");
    const title = createElement("h2", "ctx-dialog__title");
    title.id = titleId;
    setText(title, options.title);
    surface.append(title);

    if (options.description !== undefined) {
        const description = createElement("p", "ctx-dialog__description");
        description.id = uniqueId("ctx-dialog-description");
        setText(description, options.description);
        element.setAttribute("aria-describedby", description.id);
        surface.append(description);
    }
    if (options.content !== undefined) {
        const body = createElement("div", "ctx-dialog__body");
        body.append(options.content);
        surface.append(body);
    }

    const actions = createElement("div", "ctx-dialog__actions");
    if (options.cancelLabel !== undefined) {
        const cancel = createButton({
            label: options.cancelLabel,
            onActivate: (): void => {
                close("cancel");
            },
        });
        actions.append(cancel.element);
    }
    if (options.confirm !== undefined) {
        const confirmOptions = options.confirm;
        const confirm = createButton({
            ...confirmOptions,
            tone: confirmOptions.tone ?? "primary",
            onActivate: (): void => {
                confirmOptions.onActivate?.();
                close("confirm");
            },
        });
        actions.append(confirm.element);
    }
    if (actions.childElementCount > 0) {
        surface.append(actions);
    }
    element.append(surface);

    let opener: HTMLElement | undefined;

    function open(): void {
        if (element.open) {
            return;
        }
        const active = document.activeElement;
        opener = active instanceof HTMLElement ? active : undefined;
        // `aria-modal` is implicit for a `showModal()`ed `<dialog>`; stating it makes the modality
        // legible to the a11y scan and to a DOM dump, and survives a future swap of the element.
        element.setAttribute("aria-modal", "true");
        element.showModal();
        // DETERMINISTIC initial focus. The UA's own choice depends on autofocus heuristics that vary
        // by version; a dialog whose focus lands somewhere different across Chromium releases is an
        // a11y contract nothing can assert.
        const first = focusableWithin(element)[0];
        (first ?? element).focus();
    }

    function close(reason: DialogCloseReason = "programmatic"): void {
        if (!element.open) {
            return;
        }
        element.removeAttribute("aria-modal");
        element.close();
        // Restore focus to whoever opened it — `isConnected` because the opener may have been removed
        // by whatever the dialog just did, and focusing a detached element silently focuses `<body>`.
        if (opener !== undefined && opener.isConnected) {
            opener.focus();
        }
        opener = undefined;
        options.onClose?.(reason);
    }

    // The NATIVE Escape path (and the browser's own close-request gesture).
    element.addEventListener("cancel", (event: Event) => {
        event.preventDefault();
        close("dismiss");
    });

    // The dialog's own Escape dismissal, duplicating the native `cancel` path so the contract is
    // assertable from a synthetic event (see the module header). Scoped to the dialog element,
    // resolves to no command id, and cannot shadow the editor keymap — an open modal makes the rest
    // of the document inert anyway.
    // key-handler-ok: modal Escape dismissal, scoped to the dialog, no command id.
    element.addEventListener("keydown", (event: KeyboardEvent) => {
        if (event.key !== "Escape") {
            return;
        }
        event.preventDefault();
        close("dismiss");
    });

    return {
        element,
        get isOpen(): boolean {
            return element.open;
        },
        open,
        close,
    };
}

// ---------------------------------------------------------------------------------------- tooltips

export interface TooltipOptions {
    /** The element the tooltip describes. It is MOVED into a positioning host. */
    readonly trigger: HTMLElement;
    readonly text: string;
    /** Where the bubble sits relative to the trigger. Purely a CSS variant. */
    readonly placement?: "top" | "bottom";
}

export interface KitTooltip {
    /** The positioning host that now wraps the trigger — mount THIS where the trigger used to be. */
    readonly element: HTMLElement;
    readonly bubble: HTMLElement;
    readonly visible: boolean;
    show(): void;
    hide(): void;
}

/**
 * Attach a tooltip to a trigger.
 *
 * THREE THINGS MAKE THIS AN ACCESSIBLE TOOLTIP RATHER THAN A HOVER DECORATION, and all three are the
 * ones a later audit would have to unpick the implementation to add:
 *
 *   1. It is reachable by KEYBOARD — `focusin` shows it, not only `mouseenter`. A tooltip that only
 *      appears on hover simply does not exist for a keyboard or touch user.
 *   2. It is DISMISSIBLE without moving the pointer (Escape), which is WCAG 1.4.13's first clause and
 *      matters because a bubble can cover the content underneath it.
 *   3. It is HOVERABLE — the bubble lives INSIDE the host that owns the hover, so moving the pointer
 *      onto the tooltip (to read a long string, or to select text from it) does not dismiss it. That
 *      falls out of the DOM shape rather than from a timer, which is why the shape is what it is.
 *
 * Position is CSS-only. A JS-measured position would have to be written as an inline style, which the
 * kit may never do (`webui-kit-source-tokens`), and a CSS-anchored bubble is enough for the tooltip
 * sizes an editor actually uses.
 */
export function createTooltip(options: TooltipOptions): KitTooltip {
    const { trigger } = options;
    const element = createElement("span", "ctx-tooltip-host");
    const parent = trigger.parentNode;
    if (parent !== null) {
        parent.insertBefore(element, trigger);
    }
    element.append(trigger);

    const bubble = createElement("span", TOOLTIP_CLASS);
    bubble.id = uniqueId("ctx-tooltip");
    bubble.setAttribute("role", "tooltip");
    bubble.setAttribute("data-placement", options.placement ?? "top");
    bubble.hidden = true;
    setText(bubble, options.text);
    element.append(bubble);

    // `aria-describedby`, not `aria-labelledby`: a tooltip explains a control that already has a
    // name. Using it as the NAME would replace the control's own label with the explanation, which
    // is how "Save" becomes "Writes the file to disk" in the accessibility tree.
    const existing = trigger.getAttribute("aria-describedby");
    trigger.setAttribute(
        "aria-describedby",
        existing === null || existing === "" ? bubble.id : `${existing} ${bubble.id}`,
    );

    let visible = false;
    function show(): void {
        visible = true;
        bubble.hidden = false;
    }
    function hide(): void {
        visible = false;
        bubble.hidden = true;
    }

    element.addEventListener("mouseenter", show);
    element.addEventListener("mouseleave", hide);
    element.addEventListener("focusin", show);
    element.addEventListener("focusout", hide);
    // key-handler-ok: WCAG 1.4.13 "dismissible" — Escape hides the bubble without moving the pointer.
    // Scoped to the tooltip host, resolves to no command id, and does not consume the event, so an
    // Escape that also means something to the editor keymap still reaches it.
    element.addEventListener("keydown", (event: KeyboardEvent) => {
        if (event.key === "Escape" && visible) {
            hide();
        }
    });

    return {
        element,
        bubble,
        get visible(): boolean {
            return visible;
        },
        show,
        hide,
    };
}
