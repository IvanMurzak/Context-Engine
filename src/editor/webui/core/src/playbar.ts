// The PLAY-BAR strip (editor-window-chrome d1, target design 02 §7): the mockup transport —
// play / pause / stop buttons, status dot + label, the `t+` timer, and the Target chip — rendered
// into the a2 slot (`#editor-playbar`), kit controls throughout, riding the PROVEN daemon RPC chain.
//
// Four facts are load-bearing:
//
//   1. ONE IMPLEMENTATION. The buttons dispatch the `play.*` COMMANDS (commands.ts `playCommands`)
//      through the late-bound registry — the same ids the palette surfaces and the d3 menu will
//      bind — and those commands write through `session.control` (session.ts), which the Shell
//      relays to its e08b `SessionFeed` writer (`editor.play|pause|stop|step`, `origin` echo
//      suppression included). The strip invents NO second write path, and the daemon cannot tell a
//      strip press from a dock-panel press.
//
//   2. TRUTHFUL STATE, TWO FEEDS. The strip renders from `DaemonSessionState` (when.ts) — the same
//      sink the palette's when-contexts read — fed by (a) the existing 500 ms `session.state` poll
//      (session.ts `SessionFeed`, whose reply now carries the additive `simTick`) and (b) each
//      `session.control` reply, adopted immediately (`makePlayActions` below) so a press paints
//      inside the same tick rather than half a second later. The `t+` timer renders the daemon's
//      LAST-REPORTED simTick — it advances when facts/replies land, never from a browser clock:
//      that is the honest reading, not a limitation.
//
//      ⚠ KNOWN STALENESS (CE #356, INHERITED — not solved here). The daemon publishes play state as
//      a FACT and exposes no `play-state` GET verb, so after a daemon RESTART the Shell's (and so
//      this strip's) last-known state has no repair path until the next fact. session_bridge.h
//      § KNOWN STALENESS owns the full analysis; the real fix is a daemon-side read verb.
//
//   3. THE FLOURISH IS THE STATUS SIGNAL (06 §2 / O1). The Play button is the `ctx-flourish` host
//      and this module is the FIRST writer of its `data-play-state` attribute (app.css:
//      "all a Play button has to set"), with the honest 3->5 mapping: `edit`->`idle`,
//      `playing`->`running`, `paused`->`paused`. `compiling` and `error` are UNREACHABLE until the
//      build pipeline publishes those facts — `flourishState` below is the one extension point, and
//      it deliberately cannot emit them today (the 01 §4 vocabulary-mismatch note). Reduced-motion
//      handling lives entirely in the theme layer; this module adds NO motion rules.
//
//   4. HONESTY FOR MOCKUP ITEMS WITH NO SOURCE (02 §7). No fps readout — nothing measures fps until
//      e11. The Target chip renders a static, non-interactive "Target: Scene" as DECLARED FUTURE
//      surface (an inert kit chip; selection lands with a real target source). The strip hides on
//      the welcome screen (a2's slot gating — no session to control); this module simply is not
//      mounted there.
//
// DOM ONLY, no `innerHTML`, exactly like banners.ts / chrome.ts: every node is built with
// `createElement` + `textContent`, so nothing off the wire can inject markup into the trusted zone.

import { createBadge, createButton, createChip, type KitButton } from "../../kit/src/index.js";
// STEP has no strip button (the mockup's transport is play/pause/stop) — it stays palette/keymap
// reachable through its command id (commands.ts STEP_COMMAND_ID).
import {
    PAUSE_COMMAND_ID,
    PLAY_COMMAND_ID,
    STOP_COMMAND_ID,
    type CommandOutcome,
    type PlayCommandActions,
} from "./commands.js";
import {
    SESSION_CONTROL_VERB_PAUSE,
    SESSION_CONTROL_VERB_PLAY,
    SESSION_CONTROL_VERB_STEP,
    SESSION_CONTROL_VERB_STOP,
    type SessionControlSender,
    type SessionControlVerb,
} from "./session.js";
import { DaemonSessionState, PLAY_STATE_EVENT, type PlayState } from "./when.js";

// ------------------------------------------------------------------------------- the DOM classes

export const PLAYBAR_TRANSPORT_CLASS = "ctx-playbar__transport";
export const PLAYBAR_DIVIDER_CLASS = "ctx-playbar__divider";
export const PLAYBAR_STATUS_CLASS = "ctx-playbar__status";
export const PLAYBAR_DOT_CLASS = "ctx-playbar__dot";
export const PLAYBAR_TIMER_CLASS = "ctx-playbar__timer";
export const PLAYBAR_FILL_CLASS = "ctx-playbar__fill";
export const PLAYBAR_TARGET_CLASS = "ctx-playbar__target";

/** The flourish host class the theme layer keys the Pulse-of-Work bloom on (app.css). */
export const FLOURISH_CLASS = "ctx-flourish";

/** The attribute the flourish selector reads — set on the PLAY button, the strip's signature host. */
export const PLAY_STATE_ATTRIBUTE = "data-play-state";

/** Which transport a button is (`play` / `pause` / `stop`), for tests and smokes. */
export const PLAYBAR_CONTROL_ATTRIBUTE = "data-playbar-control";

/** The `<html>` report of what the strip renders — boot diagnosability, like `data-editor-strips`. */
export const PLAYBAR_ATTRIBUTE = "data-editor-playbar";

// ------------------------------------------------------------------------------- the vocabulary

/**
 * The flourish vocabulary (app.css `.ctx-flourish[data-play-state=...]`, five values). Three are
 * reachable today; `compiling` and `error` stay in the type as the DECLARED extension point for the
 * build pipeline's future facts — `flourishState` below cannot emit them, and the DOM tier asserts
 * that unreachability rather than skipping it.
 */
export type FlourishState = "idle" | "running" | "compiling" | "error" | "paused";

/**
 * The honest 3->5 mapping (02 §7, the 01 §4 vocabulary-mismatch note): the L-51 session vocabulary
 * has three states, the flourish five. `edit`->`idle`, `playing`->`running`, `paused`->`paused`;
 * nothing measures compilation or build errors yet, so nothing may claim those hues — when the
 * build pipeline publishes such facts, THIS function is where they join.
 */
export function flourishState(state: PlayState): FlourishState {
    switch (state) {
        case "playing":
            return "running";
        case "paused":
            return "paused";
        case "edit":
            return "idle";
    }
}

/** The status label + dot tone per session state (06 §2's reserved semantics, bound 1:1). */
export function statusTone(state: PlayState): "good" | "wait" | "idle" {
    switch (state) {
        case "playing":
            return "good";
        case "paused":
            return "wait";
        case "edit":
            return "idle";
    }
}

/** The human-readable status label — the L-51 provenance state, said plainly. */
export function statusLabel(state: PlayState): string {
    switch (state) {
        case "playing":
            return "Playing";
        case "paused":
            return "Paused";
        case "edit":
            return "Edit";
    }
}

// Plain text glyphs, the chrome.ts discipline (never icon fonts). The play glyph doubles as resume;
// its accessible label flips with the state so a screen reader hears the ACTION.
const GLYPH_PLAY = "▶";
const GLYPH_PAUSE = "❚❚";
const GLYPH_STOP = "■";

export const LABEL_PLAY = "Play";
export const LABEL_RESUME = "Resume";
export const LABEL_PAUSE = "Pause";
export const LABEL_STOP = "Stop";
export const LABEL_TIMER = "Simulation tick";
export const LABEL_TARGET = "Play target: Scene (future surface)";
export const TARGET_LABEL = "Target: Scene";

// ------------------------------------------------------------------------------- the strip mount

export interface MountPlaybarOptions {
    /** Dispatch a command id through the late-bound registry (boot.ts closes over the holder). */
    readonly executeCommand: (commandId: string) => void;
}

/** What `mountPlaybar` produced — the handle boot keeps and the DOM tier asserts on. */
export interface PlaybarMount {
    /** Re-render from the session sink's current truth. Idempotent; cheap on an unchanged state. */
    applySession(state: PlayState, simTick: number): void;
    /** The state the strip currently renders (the DOM tier's observable). */
    readonly renderedState: PlayState;
}

/**
 * Render the strip into the a2 play-bar slot. Replaces the slot's children wholesale (a re-mount is
 * a re-render, the `mountChrome` rule) and seeds the DOM from the `edit` boot baseline — the caller
 * applies the live session state right after (and on every feed report from then on).
 */
export function mountPlaybar(slot: HTMLElement, options: MountPlaybarOptions): PlaybarMount {
    const doc = slot.ownerDocument;
    slot.replaceChildren();

    const el = (tag: string, className: string, text = ""): HTMLElement => {
        const node = doc.createElement(tag);
        node.className = className;
        if (text !== "") {
            node.textContent = text;
        }
        return node;
    };

    // --- the transport (kit buttons; commands do the writing) -----------------------------------
    const transport = el("div", PLAYBAR_TRANSPORT_CLASS);
    transport.setAttribute("role", "group");
    transport.setAttribute("aria-label", "Play controls");
    const button = (
        label: string,
        accessibleLabel: string,
        commandId: string,
        control: string,
    ): KitButton => {
        const kit = createButton({
            label,
            accessibleLabel,
            commandId,
            onActivate: (): void => {
                options.executeCommand(commandId);
            },
        });
        kit.element.setAttribute(PLAYBAR_CONTROL_ATTRIBUTE, control);
        return kit;
    };
    const play = button(GLYPH_PLAY, LABEL_PLAY, PLAY_COMMAND_ID, "play");
    // The Play button IS the flourish host (module note 3) — the one signature flourish's element.
    play.element.classList.add(FLOURISH_CLASS);
    const pause = button(GLYPH_PAUSE, LABEL_PAUSE, PAUSE_COMMAND_ID, "pause");
    const stop = button(GLYPH_STOP, LABEL_STOP, STOP_COMMAND_ID, "stop");
    transport.append(play.element, pause.element, stop.element);
    slot.append(transport);

    slot.append(el("span", PLAYBAR_DIVIDER_CLASS));

    // --- the status dot + label ------------------------------------------------------------------
    // The dot is DECORATION (styled in app.css off `data-tone`); the label is the live kit badge —
    // a play-state change is a legitimate, rare announcement (the createBadge module header's rule).
    const status = el("span", PLAYBAR_STATUS_CLASS);
    const dot = el("span", PLAYBAR_DOT_CLASS);
    dot.setAttribute("aria-hidden", "true");
    const label = createBadge({ label: statusLabel("edit"), tone: "idle", live: true });
    status.append(dot, label.element);
    slot.append(status);

    slot.append(el("span", PLAYBAR_DIVIDER_CLASS));

    // --- the `t+` timer --------------------------------------------------------------------------
    const timer = el("span", PLAYBAR_TIMER_CLASS);
    timer.title = LABEL_TIMER;
    timer.setAttribute("aria-label", LABEL_TIMER);
    slot.append(timer);

    slot.append(el("span", PLAYBAR_FILL_CLASS));

    // --- the Target chip: static, non-interactive, declared future surface (module note 4) -------
    const target = createChip({ label: TARGET_LABEL });
    target.element.classList.add(PLAYBAR_TARGET_CLASS);
    target.element.title = LABEL_TARGET;
    slot.append(target.element);

    // --- the render function ---------------------------------------------------------------------
    let rendered: PlayState = "edit";
    let renderedTick = 0;
    const render = (state: PlayState, simTick: number): void => {
        rendered = state;
        renderedTick = simTick;
        play.element.setAttribute(PLAY_STATE_ATTRIBUTE, flourishState(state));
        // Play doubles as resume; the accessible label names the ACTION the press will perform.
        const playLabel = state === "paused" ? LABEL_RESUME : LABEL_PLAY;
        play.element.setAttribute("aria-label", playLabel);
        play.element.title = playLabel;
        // Enablement mirrors the `when` guards the palette filters on (commands.ts playCommands):
        // one state machine, two projections.
        play.setDisabled(state === "playing");
        pause.setDisabled(state !== "playing");
        stop.setDisabled(state === "edit");
        const tone = statusTone(state);
        dot.setAttribute("data-tone", tone);
        label.setLabel(statusLabel(state));
        label.setTone(tone);
        timer.textContent = `t+${String(simTick)}`;
        // The `<html>` report, updated with every render — the smoke/DevTools readout.
        doc.documentElement.setAttribute(
            PLAYBAR_ATTRIBUTE,
            `state ${state}; simTick ${String(simTick)}`,
        );
    };
    render("edit", 0);

    return {
        // The unchanged-state short-circuit is load-bearing, not an optimisation: boot's feed
        // callback calls this after EVERY 500 ms poll read, and the status label is an
        // `aria-live="polite"` region — `setText` replaces its text node, and a live region whose
        // node is replaced re-announces even when the text is identical, so an unguarded re-render
        // would have a screen reader saying "Playing" twice a second for the life of the window.
        applySession: (state: PlayState, simTick: number): void => {
            if (state === rendered && simTick === renderedTick) {
                return;
            }
            render(state, simTick);
        },
        get renderedState(): PlayState {
            return rendered;
        },
    };
}

// ------------------------------------------------------------------------------- the play actions

/** A late-bound holder for the mounted strip (boot fills it; the actions render through it). */
export interface PlaybarHolder {
    current: PlaybarMount | undefined;
}

/**
 * The real `PlayCommandActions`: each command sends its `session.control` verb, adopts the reply
 * into the ONE session sink, and re-renders the strip — extracted (and exported) so the T1 tier
 * drives THIS function against a scripted bridge rather than a copy of it (the makeSessionActions
 * rule).
 *
 * ADOPTING THE REPLY IS NOT A SECOND TRUTH. The reply's `{state, simTick}` is the daemon's own
 * answer relayed through the Shell's `PlaybarModel.adopt` — the same provenance as the
 * `session.state` poll (origin 0: a relay, not a client-caused change as far as this sink can
 * tell), arriving sooner. The poll then confirms it: the Shell's `session.state` generation moves
 * on locally driven transitions too (editor_main.cpp sums the control generation in), so the next
 * tick re-reads and lands on the same value. An unreadable reply token adopts NOTHING — the
 * `toPlayState` rule — and the sink keeps its last known state.
 */
export function makePlayActions(
    control: SessionControlSender,
    session: DaemonSessionState,
    strip: PlaybarHolder,
): PlayCommandActions {
    const drive = async (
        verb: SessionControlVerb,
        transition: string,
    ): Promise<CommandOutcome> => {
        const report = await control.send(verb);
        if (!report.served) {
            return { ok: false, note: `session.control unavailable: ${report.diagnostic}` };
        }
        if (report.playState !== null) {
            session.applyFact({
                event: PLAY_STATE_EVENT,
                origin: 0,
                state: report.stateToken,
                // An unreadable reply tick is OMITTED, not re-spelled as 0 — the sink's tick-less
                // fact rule then keeps the last known value (when.ts).
                ...(report.simTick !== null ? { simTick: report.simTick } : {}),
            });
            strip.current?.applySession(session.playState, session.simTick);
        }
        if (report.errorCode !== "") {
            return { ok: false, note: `${transition} refused: ${report.errorCode}` };
        }
        if (!report.changed) {
            // The Shell's honest "nothing to do": a benign no-op, or no daemon link behind the
            // Shell — deliberately indistinguishable (session_bridge.h SessionControlOutcome).
            return { ok: false, note: `${transition}: nothing to do (no change, no live session?)` };
        }
        return {
            ok: true,
            note: `${transition}: now ${report.stateToken} (t+${String(report.simTick ?? session.simTick)})`,
        };
    };
    return {
        play: () => drive(SESSION_CONTROL_VERB_PLAY, "play"),
        pause: () => drive(SESSION_CONTROL_VERB_PAUSE, "pause"),
        stop: () => drive(SESSION_CONTROL_VERB_STOP, "stop"),
        step: () => drive(SESSION_CONTROL_VERB_STEP, "step"),
    };
}
