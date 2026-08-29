// T1 for the play-bar strip (editor-window-chrome d1, target design 02 §7).
//
// This tier owns the STRIP — anatomy, the honest 3->5 `data-play-state` mapping (with the two
// unreachable states pinned unreachable, not skipped), transport enablement, the `t+` timer, the
// declared-future Target chip — and the `makePlayActions` write path over a scripted
// `session.control` sender. The end-to-end wiring (a real boot mounting the strip and a command
// press flowing through the live registry) is boot.test.ts's, and the C++ halves (the bridge relay,
// the `SessionFeed` chain, echo suppression) are pinned by editor-shell-test_session_bridge /
// editor-shell-test_session_feed.

import { assert, assertEqual, type TestCase } from "./harness.js";
import {
    FLOURISH_CLASS,
    LABEL_PAUSE,
    LABEL_PLAY,
    LABEL_RESUME,
    PLAYBAR_ATTRIBUTE,
    PLAYBAR_CONTROL_ATTRIBUTE,
    PLAYBAR_DIVIDER_CLASS,
    PLAYBAR_DOT_CLASS,
    PLAYBAR_TARGET_CLASS,
    PLAYBAR_TIMER_CLASS,
    PLAYBAR_TRANSPORT_CLASS,
    PLAY_STATE_ATTRIBUTE,
    flourishState,
    makePlayActions,
    mountPlaybar,
    statusLabel,
    statusTone,
    type PlaybarHolder,
    type PlaybarMount,
} from "../playbar.js";
import { PAUSE_COMMAND_ID, PLAY_COMMAND_ID, STOP_COMMAND_ID } from "../commands.js";
import type { SessionControlReport, SessionControlSender, SessionControlVerb } from "../session.js";
import { DaemonSessionState, type PlayState } from "../when.js";

// --------------------------------------------------------------------------- the DOM harness

interface Harness {
    readonly slot: HTMLElement;
    readonly mount: PlaybarMount;
    readonly executed: string[];
    dispose(): void;
}

function mountHarness(): Harness {
    const slot = document.createElement("div");
    slot.id = "playbar-test-slot";
    document.body.append(slot);
    const executed: string[] = [];
    const mount = mountPlaybar(slot, {
        executeCommand: (commandId: string): void => {
            executed.push(commandId);
        },
    });
    return {
        slot,
        mount,
        executed,
        dispose: (): void => {
            slot.remove();
            document.documentElement.removeAttribute(PLAYBAR_ATTRIBUTE);
        },
    };
}

function control(slot: HTMLElement, which: string): HTMLButtonElement {
    const element = slot.querySelector<HTMLButtonElement>(
        `[${PLAYBAR_CONTROL_ATTRIBUTE}="${which}"]`,
    );
    if (element === null) {
        throw new Error(`no ${which} transport button`);
    }
    return element;
}

// --------------------------------------------------------------------------- a scripted sender

interface ScriptedSender extends SessionControlSender {
    readonly sent: SessionControlVerb[];
}

function scriptedSender(reply: Partial<SessionControlReport>): ScriptedSender {
    const sent: SessionControlVerb[] = [];
    return {
        sent,
        send: (verb: SessionControlVerb): Promise<SessionControlReport> => {
            sent.push(verb);
            return Promise.resolve({
                served: true,
                changed: false,
                playState: null,
                stateToken: "",
                simTick: 0,
                errorCode: "",
                diagnostic: "",
                ...reply,
            });
        },
    };
}

export const playbarTests: readonly TestCase[] = [
    {
        name: "playbar: the strip renders the mockup anatomy from kit parts, honestly reduced",
        run: () => {
            const harness = mountHarness();
            try {
                const { slot } = harness;
                // Transport: exactly the three mockup buttons, every one a KIT button (the a2 rule:
                // every control is a kit component) carrying its command id for the a11y scan.
                const buttons = slot.querySelectorAll(`.${PLAYBAR_TRANSPORT_CLASS} button`);
                assertEqual(buttons.length, 3, "play / pause / stop — the mockup transport");
                for (const button of buttons) {
                    assert(
                        button.classList.contains("ctx-widget-button"),
                        "a transport control is a kit button, not a bespoke one",
                    );
                }
                assertEqual(
                    control(slot, "play").getAttribute("data-command"),
                    PLAY_COMMAND_ID,
                    "the play button names its command",
                );
                assertEqual(
                    control(slot, "pause").getAttribute("data-command"),
                    PAUSE_COMMAND_ID,
                    "the pause button names its command",
                );
                assertEqual(
                    control(slot, "stop").getAttribute("data-command"),
                    STOP_COMMAND_ID,
                    "the stop button names its command",
                );

                // The status label is a LIVE kit badge (a play-state change is announce-worthy),
                // beside the decorative dot.
                const label = slot.querySelector('[role="status"]');
                assert(label !== null, "the status label is the live kit badge");
                assertEqual(label?.textContent, "Edit", "boot renders the L-51 edit state, plainly");
                const dot = slot.querySelector(`.${PLAYBAR_DOT_CLASS}`);
                assertEqual(
                    dot?.getAttribute("aria-hidden"),
                    "true",
                    "the dot is decoration — the badge carries the semantics",
                );

                // The `t+` timer starts at the boot baseline.
                assertEqual(
                    slot.querySelector(`.${PLAYBAR_TIMER_CLASS}`)?.textContent,
                    "t+0",
                    "no session, tick 0 — the honest boot timer",
                );

                // HONESTY (02 §7): no fps readout anywhere (nothing measures it until e11), and the
                // Target chip is static + non-interactive — declared future surface, not a fake
                // control.
                assert(!(slot.textContent ?? "").includes("fps"), "no fps — nothing measures it");
                const target = slot.querySelector(`.${PLAYBAR_TARGET_CLASS}`);
                assertEqual(target?.textContent, "Target: Scene", "the static target chip");
                assertEqual(
                    target?.querySelectorAll("button").length,
                    0,
                    "…with no interactive parts: it does nothing yet, so it must not look pressable",
                );

                // Two dividers, per the mockup's grouping.
                assertEqual(
                    slot.querySelectorAll(`.${PLAYBAR_DIVIDER_CLASS}`).length,
                    2,
                    "transport | status | timer",
                );

                // The `<html>` report is live from the first render.
                assertEqual(
                    document.documentElement.getAttribute(PLAYBAR_ATTRIBUTE),
                    "state edit; simTick 0",
                    "the boot report names the rendered state",
                );
            } finally {
                harness.dispose();
            }
        },
    },
    {
        name: "playbar: data-play-state carries the honest 3->5 mapping; compiling/error are UNREACHABLE",
        run: () => {
            // The mapping itself, exhaustively: three inputs, three reachable outputs. `compiling`
            // and `error` exist in the CSS vocabulary (app.css paints five states) but NOTHING
            // measures compilation yet — so the mapping must be INCAPABLE of emitting them, and
            // this assertion is what turns "we don't render those" from a habit into a pin.
            const states: readonly PlayState[] = ["edit", "playing", "paused"];
            assertEqual(flourishState("edit"), "idle", "edit -> idle");
            assertEqual(flourishState("playing"), "running", "playing -> running");
            assertEqual(flourishState("paused"), "paused", "paused -> paused");
            for (const state of states) {
                const flourish = flourishState(state);
                assert(
                    flourish !== "compiling" && flourish !== "error",
                    `no session state may claim the ${flourish} hue — nothing measures it yet`,
                );
            }

            // ...and the DOM writer applies it to the flourish host (the Play button).
            const harness = mountHarness();
            try {
                const play = control(harness.slot, "play");
                assert(
                    play.classList.contains(FLOURISH_CLASS),
                    "the Play button is the ctx-flourish host (O1: the flourish IS the status)",
                );
                assertEqual(play.getAttribute(PLAY_STATE_ATTRIBUTE), "idle", "boot renders idle");
                harness.mount.applySession("playing", 7);
                assertEqual(
                    play.getAttribute(PLAY_STATE_ATTRIBUTE),
                    "running",
                    "playing renders running",
                );
                harness.mount.applySession("paused", 7);
                assertEqual(
                    play.getAttribute(PLAY_STATE_ATTRIBUTE),
                    "paused",
                    "paused renders paused",
                );
                harness.mount.applySession("edit", 0);
                assertEqual(play.getAttribute(PLAY_STATE_ATTRIBUTE), "idle", "edit renders idle");
            } finally {
                harness.dispose();
            }
        },
    },
    {
        name: "playbar: transport enablement + labels track the session state; the timer tracks the tick",
        run: () => {
            const harness = mountHarness();
            try {
                const { slot, mount } = harness;
                const play = control(slot, "play");
                const pause = control(slot, "pause");
                const stop = control(slot, "stop");
                const timer = slot.querySelector(`.${PLAYBAR_TIMER_CLASS}`);
                const label = slot.querySelector('[role="status"]');
                const dot = slot.querySelector(`.${PLAYBAR_DOT_CLASS}`);

                // EDIT: only play can do something (the same truth the palette's `when` guards
                // project — one state machine, two projections).
                assert(!play.disabled && pause.disabled && stop.disabled, "edit: play only");
                assertEqual(play.getAttribute("aria-label"), LABEL_PLAY, "the action is Play");

                mount.applySession("playing", 12);
                assert(play.disabled && !pause.disabled && !stop.disabled, "playing: pause/stop");
                assertEqual(label?.textContent, statusLabel("playing"), "the label says Playing");
                assertEqual(dot?.getAttribute("data-tone"), statusTone("playing"), "dot: good");
                assertEqual(timer?.textContent, "t+12", "the timer renders the daemon's tick");
                assertEqual(pause.getAttribute("aria-label"), LABEL_PAUSE, "pause is itself");

                mount.applySession("paused", 12);
                assert(!play.disabled && pause.disabled && !stop.disabled, "paused: resume/stop");
                assertEqual(
                    play.getAttribute("aria-label"),
                    LABEL_RESUME,
                    "paused flips the play button's ACTION to Resume (the max/restore rule)",
                );
                assertEqual(dot?.getAttribute("data-tone"), "wait", "paused is the wait hue");
                assertEqual(
                    document.documentElement.getAttribute(PLAYBAR_ATTRIBUTE),
                    "state paused; simTick 12",
                    "the report tracks every render",
                );
            } finally {
                harness.dispose();
            }
        },
    },
    {
        name: "playbar: a button press dispatches its command id — the strip has NO private write path",
        run: () => {
            const harness = mountHarness();
            try {
                control(harness.slot, "play").click();
                harness.mount.applySession("playing", 1);
                control(harness.slot, "pause").click();
                harness.mount.applySession("paused", 1);
                control(harness.slot, "stop").click();
                assertEqual(
                    harness.executed,
                    [PLAY_COMMAND_ID, PAUSE_COMMAND_ID, STOP_COMMAND_ID],
                    "every press goes through the ONE command registry — palette parity for free",
                );
            } finally {
                harness.dispose();
            }
        },
    },
    {
        name: "playbar: makePlayActions adopts the daemon's reply into the ONE sink and re-renders",
        run: async () => {
            const harness = mountHarness();
            const session = new DaemonSessionState();
            const holder: PlaybarHolder = { current: harness.mount };
            try {
                const sender = scriptedSender({
                    changed: true,
                    playState: "playing",
                    stateToken: "playing",
                    simTick: 3,
                });
                const actions = makePlayActions(sender, session, holder);
                const outcome = await actions.play();
                assertEqual(sender.sent, ["play"], "play sends its verb");
                assert(outcome.ok, "a changed transition is ok");
                assert(outcome.note.includes("playing"), "the note names the resulting state");
                assertEqual(session.playState, "playing", "the sink adopted the reply immediately");
                assertEqual(session.simTick, 3, "…tick included");
                assertEqual(
                    control(harness.slot, "play").getAttribute(PLAY_STATE_ATTRIBUTE),
                    "running",
                    "the strip re-rendered inside the same press, not on the next poll",
                );

                // A daemon REFUSAL: the reserved code is surfaced; the reply's state is still the
                // daemon's truth, so it is adopted too (an unchanged one is a no-op adopt).
                const refusing = scriptedSender({
                    changed: false,
                    playState: "playing",
                    stateToken: "playing",
                    simTick: 3,
                    errorCode: "play.not_running",
                });
                const refused = await makePlayActions(refusing, session, holder).step();
                assertEqual(refusing.sent, ["step"], "step sends its verb");
                assert(!refused.ok, "a refusal is not ok");
                assert(refused.note.includes("play.not_running"), "…and names the daemon's code");

                // An UNSERVED write (an older Shell): reported, sink untouched.
                const dead: SessionControlSender = {
                    send: (): Promise<SessionControlReport> =>
                        Promise.resolve({
                            served: false,
                            changed: false,
                            playState: null,
                            stateToken: "",
                            simTick: 0,
                            errorCode: "",
                            diagnostic: "bridge.unknown_method: nope",
                        }),
                };
                const unserved = await makePlayActions(dead, session, holder).pause();
                assert(!unserved.ok, "an unserved write is not ok");
                assert(unserved.note.includes("unavailable"), "…and says the surface is missing");
                assertEqual(session.playState, "playing", "the sink kept its last known state");

                // An UNKNOWN reply token adopts NOTHING (the toPlayState rule) — the strip keeps
                // rendering the last known state rather than inventing one.
                const foreign = scriptedSender({
                    changed: true,
                    playState: null,
                    stateToken: "rewinding",
                    simTick: 9,
                });
                await makePlayActions(foreign, session, holder).stop();
                assertEqual(session.playState, "playing", "an unreadable token moves nothing");
                assertEqual(session.simTick, 3, "…tick included");
            } finally {
                harness.dispose();
            }
        },
    },
];
