// The kit's INTERNAL DOM helpers (M9 e06c2; design 06 §3, 04 §4).
//
// Deliberately tiny and deliberately not exported from `index.ts`: these are how the authored
// component families build elements, not part of the surface a package author programs against. The
// published surface is the `create*` factories and the constants `index.ts` names.
//
// TWO INVARIANTS LIVE HERE, and they are why the families never touch the DOM directly.
//
//   1. NO INLINE STYLING, EVER. Every appearance a family has comes from `styles/components.css`,
//      which the `webui-kit-tokens-only` lint scans. An `element.style.color = "#fff"` in a kit
//      module would be a raw value in kit source that the CSS lint cannot see at all — the exact
//      failure mode e06c1's `var()`-fallback rule closed one layer down. `webui-kit-source-tokens`
//      now scans these TS sources for it, so the two gates together cover both halves of the kit.
//      That is also why nothing here takes a colour, a length or a duration: a helper that accepted
//      one would be the hole.
//   2. TEXT IS TEXT. Every string a family renders goes through `textContent`, never `innerHTML` —
//      the kit is downstream of the same authored-project-strings threat the hydration runtime's
//      escaping contract addresses (04 §4, C-F6), and a component that interpolated markup would
//      re-open it in the trusted editor-core zone with the CSP as the only remaining backstop.

/**
 * Monotonic id source for the `id` / `aria-*` wiring the interactive families need.
 *
 * A COUNTER RATHER THAN A RANDOM STRING because an id that changes between two runs of the same
 * code makes a DOM snapshot untestable and a golden screenshot's accessibility tree unstable; the
 * ids only have to be unique within one document, which a per-bundle counter already guarantees
 * (one editor-core instance per window, 04 §1).
 */
let idCounter = 0;

/** A document-unique id of the form `<prefix>-<n>`. */
export function uniqueId(prefix: string): string {
    idCounter += 1;
    return `${prefix}-${idCounter}`;
}

/** Create an element and give it classes. Classes only — never a style, never markup. */
export function createElement<K extends keyof HTMLElementTagNameMap>(
    tag: K,
    ...classNames: readonly string[]
): HTMLElementTagNameMap[K] {
    const element = document.createElement(tag);
    for (const name of classNames) {
        if (name !== "") {
            element.classList.add(name);
        }
    }
    return element;
}

/** Set an element's text. The ONLY way a kit component puts a caller's string into the DOM. */
export function setText(element: HTMLElement, text: string): void {
    element.textContent = text;
}

/** Add or remove a class from a boolean, so a family's state toggles read the same everywhere. */
export function toggleClass(element: HTMLElement, className: string, on: boolean): void {
    element.classList.toggle(className, on);
}

/**
 * The tones the kit's semantic surfaces (badges, chips, toasts) may carry.
 *
 * EXACTLY the five reserved status hues of design 06 §2 plus `neutral`, and no others. The
 * semantics are reserved — `warn` means ACTIVE WORK, `wait` means awaiting a human — so a family
 * that invented a sixth tone would either need a colour outside the theme (which the lint refuses)
 * or would overload a reserved meaning, which is worse because nothing would report it.
 */
export type SemanticTone = "neutral" | "good" | "warn" | "bad" | "wait" | "idle";

/** The `data-tone` attribute the stylesheet selects on. An attribute, not a class-per-tone, so the
 * six rules stay one selector list instead of six near-identical blocks. */
export const TONE_ATTRIBUTE = "data-tone";

/** Apply a tone to an element. */
export function applyTone(element: HTMLElement, tone: SemanticTone): void {
    element.setAttribute(TONE_ATTRIBUTE, tone);
}

/** The selector matching everything the browser will let a user Tab to. */
const FOCUSABLE_SELECTOR = [
    "a[href]",
    "button:not([disabled])",
    "input:not([disabled])",
    "select:not([disabled])",
    "textarea:not([disabled])",
    '[tabindex]:not([tabindex="-1"])',
].join(",");

/** Every keyboard-focusable descendant of `root`, in document (= tab) order. */
export function focusableWithin(root: ParentNode): readonly HTMLElement[] {
    return [...root.querySelectorAll<HTMLElement>(FOCUSABLE_SELECTOR)];
}

/**
 * Move focus to `index` of a ROVING-TABINDEX group: exactly one member is tabbable at a time and
 * the arrow keys move which.
 *
 * The pattern every composite widget in the kit uses (tabs, trees, lists), because the alternative
 * — leaving every member tabbable — turns one Tab stop into N and makes a long tree impossible to
 * Tab past. Returns the element that received focus so a caller can update its own state from the
 * SAME decision rather than re-deriving the index.
 */
export function focusRoving(
    members: readonly HTMLElement[],
    index: number,
): HTMLElement | undefined {
    if (members.length === 0) {
        return undefined;
    }
    const clamped = Math.max(0, Math.min(index, members.length - 1));
    for (let i = 0; i < members.length; i += 1) {
        const member = members[i];
        if (member !== undefined) {
            member.tabIndex = i === clamped ? 0 : -1;
        }
    }
    const target = members[clamped];
    target?.focus();
    return target;
}

/**
 * The index an arrow key moves a roving group to, or `undefined` when the key is not a navigation
 * key for this group.
 *
 * PURE and shared, so tabs / trees / lists cannot drift into three subtly different keyboard
 * contracts — the thing an a11y audit finds late and expensive. `Home`/`End` are part of the ARIA
 * authoring practices for every one of these patterns, not an extra.
 */
export function rovingIndexFor(
    key: string,
    current: number,
    count: number,
    orientation: "horizontal" | "vertical",
): number | undefined {
    if (count === 0) {
        return undefined;
    }
    const next = orientation === "horizontal" ? "ArrowRight" : "ArrowDown";
    const previous = orientation === "horizontal" ? "ArrowLeft" : "ArrowUp";
    if (key === next) {
        return (current + 1) % count;
    }
    if (key === previous) {
        return (current - 1 + count) % count;
    }
    if (key === "Home") {
        return 0;
    }
    if (key === "End") {
        return count - 1;
    }
    return undefined;
}
