// The FIELDS family (M9 e06c2; design 06 §3) — labelled text, checkbox and select controls.
//
// BUILT ON e06c1's `textbox` / `checkbox` role primitives, for the same reason buttons are: the
// control element carries the hydration widget class, so a field's paint is decided once in
// `kit.css` and an authored field is visually the same control a C++ panel's uitree node hydrates
// into. The family adds the part the widget layer has no opinion about — the LABEL ASSOCIATION and
// the description/error wiring, which is what turns a themed input into an accessible field.
//
// THE LABEL IS A REAL `<label for>`, NOT AN `aria-label`. Both name the control for assistive
// technology, but only the real label is a CLICK TARGET that focuses the input, and only it is
// visible to a sighted user who needs the field explained. `aria-label` remains available for the
// genuinely label-less case (a search box with a magnifier) through `accessibleLabel`.
//
// ⚠ `select` HAS NO e06c1 ROLE PRIMITIVE — a recorded exception, not an oversight. The closed
// hydration vocabulary (`uitree::Role`) has `textbox` and `checkbox` and no select/combobox member,
// because no C++ panel model emits one today. The Settings panel (e06d, this kit's first consumer)
// needs one for the theme picker (06 §4), so the family ships it as net-new styling on a NATIVE
// `<select>`. When the C++ vocabulary grows a `select` role, the right move is to add it to
// `WIDGET_CLASSES` + `kit.css` and re-point `.ctx-field__select` at it — the role-coverage gate will
// force that conversation rather than let two paths coexist quietly.

import { createElement, setText, uniqueId } from "./dom.js";
import { requireWidgetClass } from "./widgets.js";

/** The family's root class — the wrapper that owns the label/control/hint stack. */
export const FIELD_CLASS = "ctx-field";

/** One `<option>` of a select field. */
export interface FieldOption {
    readonly value: string;
    readonly label: string;
}

interface CommonFieldOptions {
    readonly label: string;
    /** A hint rendered under the control AND wired as the control's `aria-describedby`. */
    readonly description?: string;
    readonly disabled?: boolean;
    /** Replaces the visible `<label>` with an `aria-label` — for a genuinely label-less control. */
    readonly accessibleLabel?: string;
}

export interface TextFieldOptions extends CommonFieldOptions {
    readonly value?: string;
    readonly placeholder?: string;
    readonly onInput?: (value: string) => void;
}

export interface CheckboxFieldOptions extends CommonFieldOptions {
    readonly checked?: boolean;
    readonly onChange?: (checked: boolean) => void;
}

export interface SelectFieldOptions extends CommonFieldOptions {
    readonly options: readonly FieldOption[];
    readonly value?: string;
    readonly onChange?: (value: string) => void;
}

/** A field: the wrapper to mount, and the control to read/write. */
export interface KitField<TControl extends HTMLElement> {
    readonly element: HTMLElement;
    readonly control: TControl;
    setDisabled(disabled: boolean): void;
}

/**
 * The label + description scaffolding every field shares.
 *
 * `describedBy` is only set when a description EXISTS: an `aria-describedby` pointing at an empty or
 * absent element is worse than none — assistive technology announces nothing and the author believes
 * the field is described.
 */
function buildScaffold(
    options: CommonFieldOptions,
    control: HTMLElement,
    kind: string,
): { readonly wrapper: HTMLElement; readonly label: HTMLLabelElement } {
    const wrapper = createElement("div", FIELD_CLASS);
    wrapper.setAttribute("data-field-kind", kind);
    const controlId = uniqueId("ctx-field");
    control.id = controlId;
    control.classList.add("ctx-field__control");

    const label = createElement("label", "ctx-field__label");
    label.htmlFor = controlId;
    setText(label, options.label);
    if (options.accessibleLabel !== undefined) {
        control.setAttribute("aria-label", options.accessibleLabel);
        label.hidden = true;
    }

    if (options.description !== undefined) {
        const description = createElement("p", "ctx-field__description");
        description.id = uniqueId("ctx-field-description");
        setText(description, options.description);
        control.setAttribute("aria-describedby", description.id);
        wrapper.append(label, control, description);
    } else {
        wrapper.append(label, control);
    }
    return { wrapper, label };
}

/** A labelled single-line text field, built on the `textbox` role primitive. */
export function createTextField(options: TextFieldOptions): KitField<HTMLInputElement> {
    const control = createElement("input", requireWidgetClass("textbox"));
    control.type = "text";
    control.value = options.value ?? "";
    if (options.placeholder !== undefined) {
        control.placeholder = options.placeholder;
    }
    control.disabled = options.disabled === true;
    const onInput = options.onInput;
    if (onInput !== undefined) {
        control.addEventListener("input", () => {
            onInput(control.value);
        });
    }
    const { wrapper } = buildScaffold(options, control, "text");
    return {
        element: wrapper,
        control,
        setDisabled: (disabled: boolean): void => {
            control.disabled = disabled;
        },
    };
}

/**
 * A labelled checkbox, built on the `checkbox` role primitive.
 *
 * The label sits AFTER the control and the wrapper carries `data-field-kind="checkbox"`, which is
 * how the stylesheet flips the stack to a row without a second class: a checkbox reads left-to-right
 * (box then text), every other field reads top-to-bottom (label then control).
 */
export function createCheckboxField(options: CheckboxFieldOptions): KitField<HTMLInputElement> {
    const control = createElement("input", requireWidgetClass("checkbox"));
    control.type = "checkbox";
    control.checked = options.checked === true;
    control.disabled = options.disabled === true;
    const onChange = options.onChange;
    if (onChange !== undefined) {
        control.addEventListener("change", () => {
            onChange(control.checked);
        });
    }
    const { wrapper, label } = buildScaffold(options, control, "checkbox");
    // Control first, then its label: the scaffold appends label-then-control, and a checkbox is the
    // one field whose reading order is the other way round.
    wrapper.insertBefore(control, label);
    return {
        element: wrapper,
        control,
        setDisabled: (disabled: boolean): void => {
            control.disabled = disabled;
        },
    };
}

/** A labelled native `<select>` (see the header's recorded exception). */
export function createSelectField(options: SelectFieldOptions): KitField<HTMLSelectElement> {
    const control = createElement("select", "ctx-field__select");
    for (const option of options.options) {
        const element = createElement("option");
        element.value = option.value;
        setText(element, option.label);
        control.append(element);
    }
    if (options.value !== undefined) {
        control.value = options.value;
    }
    control.disabled = options.disabled === true;
    const onChange = options.onChange;
    if (onChange !== undefined) {
        control.addEventListener("change", () => {
            onChange(control.value);
        });
    }
    const { wrapper } = buildScaffold(options, control, "select");
    return {
        element: wrapper,
        control,
        setDisabled: (disabled: boolean): void => {
            control.disabled = disabled;
        },
    };
}
