// T1 for the browser-side daemon SESSION feed (M9 e08d, design 05 §4 / §6).
//
// This tier owns the RELAY half — reply -> sink — and `boot.test.ts` owns the WIRING half (that the
// live editor's when-context is resolved from that sink). Split deliberately: a feed that parses
// perfectly is worth nothing if boot still filters on a frozen baseline, which is precisely the
// state e08b shipped and this task removes.
//
// The properties pinned here are the ones whose failure is INVISIBLE at runtime — the editor is up,
// nothing errors, and `playState` is simply wrong forever:
//
//   * a served reply moves the sink (the whole point);
//   * a REFUSED relay is honest and STOPS the poll rather than refusing once per tick forever;
//   * an unchanged generation short-circuits, but a MISSING one does NOT (a Shell that cannot count
//     must still be believed about the state, or a relay bug presents as a freeze);
//   * the method name is the one the Shell routes.

import { assert, assertEqual, type TestCase } from "./harness.js";
import { ShellBridge, type BridgeQuery, type BridgeQueryFunction } from "../bridge.js";
import {
    SESSION_STATE_METHOD,
    SessionFeed,
    describeSessionRead,
    type SessionScheduler,
} from "../session.js";
import { DaemonSessionState } from "../when.js";

/** Records every method the bundle asked for, and answers from a scripted table. */
interface Recorder {
    readonly query: BridgeQueryFunction;
    readonly methods: string[];
}

type Answer = { result: unknown } | { error: { code: number; message: string; reason: string } };

function recordingQuery(answers: Record<string, Answer>): Recorder {
    const methods: string[] = [];
    const query: BridgeQueryFunction = (request: BridgeQuery): number => {
        const parsed = JSON.parse(request.request) as { id: number; method: string };
        methods.push(parsed.method);
        const answer = answers[parsed.method];
        const envelope =
            answer === undefined
                ? {
                      jsonrpc: "2.0",
                      id: parsed.id,
                      error: {
                          code: -32601,
                          message: "unknown method",
                          data: { reason: "bridge.unknown_method" },
                      },
                  }
                : "result" in answer
                  ? { jsonrpc: "2.0", id: parsed.id, result: answer.result }
                  : {
                        jsonrpc: "2.0",
                        id: parsed.id,
                        error: {
                            code: answer.error.code,
                            message: answer.error.message,
                            data: { reason: answer.error.reason },
                        },
                    };
        request.onSuccess(JSON.stringify(envelope));
        return methods.length;
    };
    return { query, methods };
}

/** The Shell's reply shape — the daemon's own `play-state` fact plus the two relay facts. */
function reply(state: string, attached: boolean, generation: number): Answer {
    return { result: { event: "play-state", state, origin: 0, attached, generation } };
}

/** A scheduler whose ticks the test fires by hand, so no case depends on wall-clock timing. */
class ManualScheduler implements SessionScheduler {
    #next = 1;
    readonly ticks = new Map<number, () => void>();

    setInterval(callback: () => void, _delayMs: number): number {
        const handle = this.#next++;
        this.ticks.set(handle, callback);
        return handle;
    }

    clearInterval(handle: number): void {
        this.ticks.delete(handle);
    }

    fire(): void {
        for (const tick of [...this.ticks.values()]) {
            tick();
        }
    }
}

export const sessionTests: readonly TestCase[] = [
    {
        name: "session: a served reply moves the sink to the daemon's play state",
        run: async () => {
            const recorder = recordingQuery({ [SESSION_STATE_METHOD]: reply("playing", true, 3) });
            const state = new DaemonSessionState();
            const feed = new SessionFeed(new ShellBridge(recorder.query), state, undefined);
            assertEqual(state.playState, "edit", "the sink starts on the boot baseline");

            const report = await feed.refresh();
            assert(report.served, "the relay was served");
            assert(report.attached, "the Shell reported a live daemon link");
            assert(report.changed, "the read moved the sink");
            assertEqual(state.playState, "playing", "the sink now holds the daemon's play state");
            assertEqual(recorder.methods, [SESSION_STATE_METHOD], "exactly one relay read");
        },
    },
    {
        name: "session: an unchanged generation short-circuits, a MISSING one does not",
        run: async () => {
            const state = new DaemonSessionState();
            // Same generation, but a state the sink has not seen: the short-circuit must be what
            // skips it, so `applied` proves the skip happened rather than a coincidence.
            const same = recordingQuery({ [SESSION_STATE_METHOD]: reply("playing", true, 7) });
            const feed = new SessionFeed(new ShellBridge(same.query), state, undefined);
            await feed.refresh();
            assertEqual(state.applied, 1, "the first read applies");
            const second = await feed.refresh();
            assert(!second.changed, "an unchanged generation skips the apply");
            assertEqual(state.applied, 1, "the sink was not touched again");

            // A reply with NO generation is still believed — a Shell that cannot count must not be
            // able to freeze the browser side.
            const countless = recordingQuery({
                [SESSION_STATE_METHOD]: { result: { event: "play-state", state: "paused", origin: 0 } },
            });
            const blind = new SessionFeed(new ShellBridge(countless.query), state, undefined);
            const third = await blind.refresh();
            assert(third.changed, "a generation-less reply is applied on its own merits");
            assertEqual(state.playState, "paused", "the sink followed it");
        },
    },
    {
        name: "session: an unserved relay is honest and leaves the sink on its last known state",
        run: async () => {
            const recorder = recordingQuery({});
            const state = new DaemonSessionState();
            const feed = new SessionFeed(new ShellBridge(recorder.query), state, undefined);
            const report = await feed.refresh();
            assert(!report.served, "an older Shell refuses the method");
            assert(report.diagnostic.includes("unknown_method"), "the refusal reason is carried");
            assertEqual(state.playState, "edit", "the sink keeps the boot baseline");
            assert(
                describeSessionRead(report).includes("unavailable"),
                "the report names the unavailability rather than implying a live read",
            );
        },
    },
    {
        name: "session: the poll re-reads on every tick and stops itself on a refusal",
        run: async () => {
            const scheduler = new ManualScheduler();
            const state = new DaemonSessionState();
            const answers: Record<string, Answer> = {
                [SESSION_STATE_METHOD]: reply("playing", true, 1),
            };
            const recorder = recordingQuery(answers);
            const feed = new SessionFeed(new ShellBridge(recorder.query), state, scheduler);
            feed.start(5);
            assert(feed.polling, "the poll is running");
            scheduler.fire();
            await Promise.resolve();
            await Promise.resolve();
            assert(feed.reads >= 1, "a tick performed a read");

            // Now make the Shell refuse: the poll must stop rather than refuse once per tick for the
            // life of the window.
            delete answers[SESSION_STATE_METHOD];
            scheduler.fire();
            for (let i = 0; i < 8; i += 1) {
                await Promise.resolve();
            }
            assert(!feed.polling, "a refused tick stops the poll");
        },
    },
    {
        name: "session: a malformed reply is tolerated, never thrown",
        run: async () => {
            const recorder = recordingQuery({ [SESSION_STATE_METHOD]: { result: "not a record" } });
            const state = new DaemonSessionState();
            const feed = new SessionFeed(new ShellBridge(recorder.query), state, undefined);
            const report = await feed.refresh();
            assert(report.served, "the Shell answered");
            assert(!report.changed, "nothing was applied");
            assertEqual(state.playState, "edit", "the sink is untouched");
            assert(report.diagnostic !== "", "and the oddity is reported rather than swallowed");
        },
    },
];
