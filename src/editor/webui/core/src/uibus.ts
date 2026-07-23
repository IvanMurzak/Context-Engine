// The `editor.ui` LOCAL BUS (M9 e08c, design 05 §5 / 02, D7 tier two).
//
// D7 splits the editor's facts into TWO tiers, and this module is the second one:
//
//   tier 1 — SEMANTIC facts (selection, cameras, play state) live DAEMON-side (e08a) and reach every
//            client — CLI, agents, other windows — over the daemon's own event stream.
//   tier 2 — UI CHROME facts (what is focused, how the dock is arranged, a drag in flight, a hover
//            in a viewport, the active theme, the palette) are EDITOR-LOCAL. They are noise to a
//            daemon whose job is authored data, and they are NEVER forwarded to it.
//
// This bus is tier 2. Its envelope deliberately MIRRORS the daemon stream's (`event_stream.h`:
// `{seq, …, topic, payload}` with snapshot-then-delta subscription), so a reader written against one
// reads the other with no second mental model — that is the whole point of building a bus here
// rather than letting every panel invent a private callback list.
//
// ⚠ THE D7 BOUNDARY IS ASSERTED, NOT DOCUMENTED. This file imports NOTHING that can reach the
// daemon: editor-core's ONE way out is `ShellBridge` (bridge.ts — it holds the socket and the attach
// token), and this module never imports it, never names it, and offers no seam through which a
// caller could hand it one. Two independent checks hold that down, and each catches what the other
// cannot:
//
//   * `tools/check_ui_bus_boundary.py` (ctest `webui-uibus-boundary`, a SOURCE scan on every build
//     leg) refuses a bridge import in this file AND refuses any `editor.ui.*` subscription anywhere
//     in editor-core whose callback reaches a bridge call — a forwarding path planted OUTSIDE the
//     paths a test happens to drive;
//   * `test/uibus.test.ts` installs a recording query function as the injected Shell channel and
//     drives the REAL theme engine plus a publish on every built-in topic, asserting the channel saw
//     zero traffic — a forwarding path that compiles past the source scan.
//
// A test that would still pass with a forwarding path in place proves nothing; both of the above
// were verified by PLANTING one and watching them fail.
//
// CROSS-WINDOW MIRRORING IS A SEAM HERE, NOT A DRILL. The design has this bus "mirrored across
// windows via the Shell". `UiMirrorSink` + `receiveMirrored` below ARE that seam — a sink to hand an
// envelope out of this window, and a total entry point for one arriving from another — and the
// envelope is unit-tested end to end across two buses. The actual multi-window PROPAGATION drill
// (a real second window, a real Shell hop) belongs to e10, which owns multi-window; per the TD
// ruling of 2026-07-22 it is NOT attempted here. Nothing wires a sink at boot, deliberately: the
// Shell's bridge router denies unknown methods, so a boot-time call to a not-yet-existing mirror
// method would redden every CEF smoke that had not installed a stub for it.

/** The reserved prefix every BUILT-IN ui-chrome topic carries. Packages may never publish under it. */
export const UI_TOPIC_PREFIX = "editor.ui.";

/** Active window / panel focus changed (05 §5; the `panelFocus` / `windowType` when-contexts' fact). */
export const UI_TOPIC_FOCUS = "editor.ui.focus";
/** The dock / layout arrangement changed. */
export const UI_TOPIC_LAYOUT = "editor.ui.layout";
/** A drag session began, moved, or ended. */
export const UI_TOPIC_DRAG = "editor.ui.drag";
/** Viewport hover / pick facts (`viewportMode`'s fact — NOT the camera, which is daemon-side). */
export const UI_TOPIC_VIEWPORT = "editor.ui.viewport";
/** The active theme changed (e06b's engine is this topic's publisher). */
export const UI_TOPIC_THEME_CHANGED = "editor.ui.theme-changed";
/** The command palette opened, filtered, or closed. */
export const UI_TOPIC_PALETTE = "editor.ui.palette";

/**
 * The CLOSED built-in topic set (05 §5).
 *
 * Closed on purpose, exactly like `GESTURE_VERBS` and the panel content-type vocabulary: a topic
 * nobody declared is a typo, and silently accepting it would produce a subscription that can never
 * fire — the least diagnosable failure this bus could have.
 */
export const BUILTIN_UI_TOPICS = [
    UI_TOPIC_FOCUS,
    UI_TOPIC_LAYOUT,
    UI_TOPIC_DRAG,
    UI_TOPIC_VIEWPORT,
    UI_TOPIC_THEME_CHANGED,
    UI_TOPIC_PALETTE,
] as const;

/** The origin stamped on an envelope published by a bus that was given no window id. */
export const DEFAULT_UI_ORIGIN = "local";

/**
 * One `editor.ui` event.
 *
 * `seq` is monotonic and totally ordered WITHIN one bus — one window — mirroring the daemon stream's
 * per-incarnation ordering. `origin` names the window that published it, which is what lets a
 * mirrored envelope be told apart from a locally-published one (echo suppression, 05 §4's `origin`
 * contract applied to tier 2).
 *
 * The payload is generic rather than a union of the six built-in shapes: a package topic's payload is
 * by definition not in any union this file could write, and a bus that only carried shapes it knew
 * would not be a bus.
 */
export interface EditorUiEvent<P = unknown> {
    readonly seq: number;
    readonly topic: string;
    readonly origin: string;
    readonly payload: P;
}

/** Detaches a subscription. Mirrors Dockview's disposable convention so callers see one idiom. */
export interface EditorUiSubscription {
    dispose(): void;
}

/** A subscriber. Typed on the payload for ergonomics; the bus stores it payload-erased. */
export type UiListener<P = unknown> = (event: EditorUiEvent<P>) => void;

/**
 * The CROSS-WINDOW MIRROR SEAM (design 05 §5 "mirrored across windows via the Shell").
 *
 * A sink receives every envelope this bus publishes LOCALLY and is responsible for getting it to the
 * other windows. It is deliberately a bare interface with no transport in it: this module must stay
 * structurally incapable of reaching the Shell (see the D7 note at the top), so the transport-bearing
 * implementation lives with whoever owns the transport — e10.
 *
 * ⚠ A sink MUST target a SHELL-local method. The Shell mirrors chrome facts between ITS OWN windows;
 * routing them onward to the daemon would be the D7 violation this whole module exists to prevent.
 */
export interface UiMirrorSink {
    deliver(event: EditorUiEvent): void;
}

/** A refused publish / subscribe / declaration, and why. Surfaced, never silently dropped. */
export interface UiRejection {
    readonly topic: string;
    readonly diagnostic: string;
}

/** What a `publish` did. `event` is present exactly when `published` is true. */
export interface UiPublishReport<P = unknown> {
    readonly published: boolean;
    /** The assigned seq, or 0 when the publish was refused (a refusal consumes no seq). */
    readonly seq: number;
    readonly event: EditorUiEvent<P> | undefined;
    /** `""` on success; why the publish was refused otherwise. */
    readonly diagnostic: string;
}

/** The outcome of declaring a package's custom topics. */
export interface UiTopicDeclarationResult {
    readonly accepted: readonly string[];
    readonly rejected: readonly UiRejection[];
}

/**
 * The topic-name grammar. Lowercase, digit- and hyphen-bearing segments — the same shape the
 * built-ins use (`theme-changed`), so a package topic reads like an editor one.
 */
const SEGMENT = /^[a-z0-9][a-z0-9-]*$/;

/** `editor` is the editor's own namespace; a package may never declare inside it. */
const RESERVED_PACKAGE_IDS: readonly string[] = ["editor"];

function isSegmented(value: string): boolean {
    if (value === "") {
        return false;
    }
    return value.split(".").every((segment) => SEGMENT.test(segment));
}

/**
 * Validate a package id: a segmented, non-reserved name.
 *
 * Returns `""` when valid, or the diagnostic. Total — every caller reports rather than throws,
 * because a malformed manifest must cost that package its topics, not the editor its bus.
 */
export function validatePackageId(packageId: string): string {
    if (!isSegmented(packageId)) {
        return `"${packageId}" is not a valid package id (lowercase dotted segments)`;
    }
    if (RESERVED_PACKAGE_IDS.includes(packageId)) {
        return `"${packageId}" is a reserved editor namespace`;
    }
    return "";
}

/**
 * Validate ONE custom topic against its declaring package (05 §5: "namespaced custom topics
 * (`<pkg>.…`), declared in the manifest").
 *
 * Returns `""` when valid, or the diagnostic. The rule is deliberately strict on BOTH ends: the topic
 * must sit under its own package's namespace (so a package cannot name a topic for another package,
 * nor squat `editor.ui.*`), and it must be a real sub-topic rather than the bare package id.
 */
export function validatePackageTopic(packageId: string, topic: string): string {
    const idDiagnostic = validatePackageId(packageId);
    if (idDiagnostic !== "") {
        return idDiagnostic;
    }
    if (!isSegmented(topic)) {
        return `"${topic}" is not a valid topic name (lowercase dotted segments)`;
    }
    if (topic.startsWith(UI_TOPIC_PREFIX)) {
        return `"${topic}" is inside the reserved "${UI_TOPIC_PREFIX}" namespace`;
    }
    if (!topic.startsWith(`${packageId}.`) || topic.length <= packageId.length + 1) {
        return `"${topic}" is not namespaced under its declaring package "${packageId}"`;
    }
    return "";
}

/** A package's manifest-declared ui topics (04 §3's `Contribution` model, tier-2 half). */
export interface PackageUiTopics {
    readonly packageId: string;
    readonly topics: readonly string[];
}

/**
 * Read a manifest's `uiTopics` declaration, total against anything.
 *
 * A manifest that declares none (every manifest today) yields an empty list rather than a diagnostic:
 * the capability is opt-in, and a panel that never publishes a custom topic is not in error.
 */
export function parsePackageUiTopics(packageId: unknown, uiTopics: unknown): PackageUiTopics {
    const id = typeof packageId === "string" ? packageId : "";
    if (!Array.isArray(uiTopics)) {
        return { packageId: id, topics: [] };
    }
    return {
        packageId: id,
        topics: uiTopics.filter((entry): entry is string => typeof entry === "string"),
    };
}

export interface EditorUiBusOptions {
    /** The id of the window this bus serves. Stamped on every locally-published envelope. */
    readonly origin?: string;
}

/**
 * The `editor.ui` bus.
 *
 * THREE properties carry the design (05 §5), and each has a T1 test that fails without it:
 *
 *   1. MONOTONIC `seq` — one counter per bus, assigned at publish, never consumed by a refusal, so a
 *      subscriber can order and gap-detect exactly as it would on the daemon stream.
 *   2. SNAPSHOT-ON-SUBSCRIBE — a late subscriber is handed the retained envelope for its topic
 *      IMMEDIATELY rather than sitting silent until the next change. A panel mounted after a theme
 *      switch must not render untokened, and "subscribe, then separately go ask for current state"
 *      is the race that model exists to remove.
 *   3. FACTS ONLY, NEVER COMMANDS — a topic reports what happened; nothing here dispatches. There is
 *      no request/response shape on this bus by construction, which is also why it can be fire-and-
 *      forget and total.
 *
 * TOTAL: a throwing subscriber is isolated so one bad panel cannot break a theme switch for every
 * other subscriber, and no method throws — every refusal is reported.
 */
export class EditorUiBus {
    readonly #origin: string;
    readonly #listeners = new Map<string, Set<UiListener>>();
    readonly #retained = new Map<string, EditorUiEvent>();
    /** topic -> the package id that declared it. Built-ins are not in here; they are always known. */
    readonly #declared = new Map<string, string>();
    readonly #mirrors = new Set<UiMirrorSink>();
    readonly #rejections: UiRejection[] = [];
    #seq = 0;

    constructor(options: EditorUiBusOptions = {}) {
        this.#origin = options.origin ?? DEFAULT_UI_ORIGIN;
    }

    /** The window id stamped on every envelope this bus publishes. */
    get origin(): string {
        return this.#origin;
    }

    /** The last assigned seq (0 before the first successful publish). */
    get seq(): number {
        return this.#seq;
    }

    /** Every refusal this bus has reported, oldest first. The diagnosability channel. */
    get rejections(): readonly UiRejection[] {
        return this.#rejections;
    }

    /** The topics a package has successfully declared, plus the built-ins. */
    get topics(): readonly string[] {
        return [...BUILTIN_UI_TOPICS, ...this.#declared.keys()];
    }

    /** Is `topic` publishable on this bus — a built-in, or a declared package topic? */
    isKnownTopic(topic: string): boolean {
        return (BUILTIN_UI_TOPICS as readonly string[]).includes(topic) || this.#declared.has(topic);
    }

    /**
     * Declare a package's custom topics (05 §5: manifest-declared + namespaced).
     *
     * FAIL-CLOSED PER TOPIC, not per package: one malformed entry costs that entry, not the package's
     * whole declaration — the same discipline the theme registry applies to a batch of files.
     *
     * This is the DECLARATION half only. The `ui_events` CAPABILITY gate — whether this package was
     * granted the right to ride the bus at all — is enforced end to end by e13; a package that
     * declares correctly but was never granted `ui_events` is e13's refusal, not this one's.
     */
    declareTopics(declaration: PackageUiTopics): UiTopicDeclarationResult {
        const accepted: string[] = [];
        const rejected: UiRejection[] = [];
        for (const topic of declaration.topics) {
            const diagnostic = this.#declare(declaration.packageId, topic);
            if (diagnostic === "") {
                accepted.push(topic);
            } else {
                const rejection: UiRejection = { topic, diagnostic };
                rejected.push(rejection);
                this.#rejections.push(rejection);
            }
        }
        return { accepted, rejected };
    }

    #declare(packageId: string, topic: string): string {
        const diagnostic = validatePackageTopic(packageId, topic);
        if (diagnostic !== "") {
            return diagnostic;
        }
        const owner = this.#declared.get(topic);
        if (owner !== undefined && owner !== packageId) {
            return `"${topic}" is already declared by "${owner}"`;
        }
        this.#declared.set(topic, packageId);
        return "";
    }

    /**
     * Publish a FACT on `topic`.
     *
     * An UNDECLARED topic is REFUSED: nothing is delivered, nothing is retained, and the seq counter
     * does not move — a refusal must be indistinguishable, to every subscriber, from the publish
     * never having happened.
     */
    publish<P>(topic: string, payload: P): UiPublishReport<P> {
        if (!this.isKnownTopic(topic)) {
            const diagnostic = topic.startsWith(UI_TOPIC_PREFIX)
                ? `"${topic}" is not one of the built-in editor.ui topics`
                : `"${topic}" was never declared by a package manifest`;
            const rejection: UiRejection = { topic, diagnostic };
            this.#rejections.push(rejection);
            return { published: false, seq: 0, event: undefined, diagnostic };
        }
        const event: EditorUiEvent<P> = {
            seq: ++this.#seq,
            topic,
            origin: this.#origin,
            payload,
        };
        this.#retain(event);
        this.#deliver(event);
        // The mirror sinks LAST: a local subscriber is in-process and a mirror is a Shell hop, so
        // delivering locally first keeps this window's own response independent of the transport.
        for (const sink of [...this.#mirrors]) {
            try {
                sink.deliver(event);
            } catch {
                // A failing transport is the transport's problem; the local fan-out already happened.
            }
        }
        return { published: true, seq: event.seq, event, diagnostic: "" };
    }

    /**
     * Subscribe to `topic`, receiving the retained snapshot IMMEDIATELY when one exists.
     *
     * An unknown topic yields an INERT subscription (and a recorded rejection) rather than a silent
     * one that could never fire: the caller still gets a disposable, so its own teardown stays
     * uniform, but the mistake is visible in `rejections`.
     */
    subscribe<P = unknown>(topic: string, listener: UiListener<P>): EditorUiSubscription {
        if (!this.isKnownTopic(topic)) {
            this.#rejections.push({
                topic,
                diagnostic: `cannot subscribe to "${topic}": it is not a built-in or declared topic`,
            });
            return { dispose: (): void => {} };
        }
        const erased = listener as UiListener;
        const existing = this.#listeners.get(topic);
        const set = existing ?? new Set<UiListener>();
        if (existing === undefined) {
            this.#listeners.set(topic, set);
        }
        set.add(erased);
        const snapshot = this.#retained.get(topic);
        if (snapshot !== undefined) {
            this.#invoke(erased, snapshot);
        }
        return {
            dispose: (): void => {
                set.delete(erased);
            },
        };
    }

    /** The retained envelope for `topic`, or `undefined` when nothing has been published on it. */
    snapshot(topic: string): EditorUiEvent | undefined {
        return this.#retained.get(topic);
    }

    /** Every retained envelope, in publish order — what a whole-bus snapshot-on-attach delivers. */
    snapshots(): readonly EditorUiEvent[] {
        return [...this.#retained.values()].sort((a, b) => a.seq - b.seq);
    }

    /**
     * Attach a cross-window mirror sink (the e10 seam).
     *
     * The sink is handed the current snapshot set immediately, for the same reason a subscriber is:
     * a window that joins late must not have to guess the chrome state it missed.
     */
    attachMirror(sink: UiMirrorSink): EditorUiSubscription {
        this.#mirrors.add(sink);
        for (const event of this.snapshots()) {
            try {
                sink.deliver(event);
            } catch {
                // See publish(): a transport failure never propagates into the bus.
            }
        }
        return {
            dispose: (): void => {
                this.#mirrors.delete(sink);
            },
        };
    }

    /** How many mirror sinks are attached. Zero in the shipping editor today (e10 wires the first). */
    get mirrorCount(): number {
        return this.#mirrors.size;
    }

    /**
     * Apply an envelope that arrived from ANOTHER window.
     *
     * Two properties make the seam loop-free and orderable, and both are asserted by
     * `uibus.test.ts`'s two-bus drill:
     *
     *   * the envelope is RE-SEALED with THIS bus's next seq, because `seq` is a per-bus ordering and
     *     splicing a foreign counter into it would break monotonicity for every local subscriber;
     *   * the ORIGIN is preserved, and a mirrored envelope is NOT re-delivered to the mirror sinks —
     *     that is the echo suppression, and without it two cross-attached windows ring forever.
     */
    receiveMirrored(event: EditorUiEvent): UiPublishReport {
        if (!this.isKnownTopic(event.topic)) {
            const diagnostic = `mirrored envelope on unknown topic "${event.topic}"`;
            this.#rejections.push({ topic: event.topic, diagnostic });
            return { published: false, seq: 0, event: undefined, diagnostic };
        }
        if (event.origin === this.#origin) {
            // Our own envelope came back around a mirror ring. Dropping it is what makes the seam
            // safe for a topology the editor does not control.
            const diagnostic = `mirrored envelope echoed back to its own origin "${event.origin}"`;
            this.#rejections.push({ topic: event.topic, diagnostic });
            return { published: false, seq: 0, event: undefined, diagnostic };
        }
        const resealed: EditorUiEvent = {
            seq: ++this.#seq,
            topic: event.topic,
            origin: event.origin,
            payload: event.payload,
        };
        this.#retain(resealed);
        this.#deliver(resealed);
        return { published: true, seq: resealed.seq, event: resealed, diagnostic: "" };
    }

    #retain(event: EditorUiEvent): void {
        this.#retained.set(event.topic, event);
    }

    #deliver(event: EditorUiEvent): void {
        const set = this.#listeners.get(event.topic);
        if (set === undefined) {
            return;
        }
        for (const listener of [...set]) {
            this.#invoke(listener, event);
        }
    }

    #invoke(listener: UiListener, event: EditorUiEvent): void {
        try {
            listener(event);
        } catch {
            // A subscriber's failure is its own; the fact must still reach everyone else.
        }
    }
}
