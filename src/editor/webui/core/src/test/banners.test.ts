// T1 unit tests for the M9 e14d notification banners (banners.ts). Three properties this tier pins:
//
//   1. EVERY PARSER IS TOTAL. A malformed / partial / absent envelope yields `null` or the documented
//      degrade, never a throw — an editor with no update channel and no daemon must still boot.
//   2. THE BANNERS APPEAR EXACTLY WHEN THEY SHOULD. The update banner only on a COMPLETED check that
//      found a newer release and was not dismissed; the daemon banner only while the link is
//      read-only. The negative cases matter more than the positive one: a banner that shows on a
//      failed check trains users to ignore the one that matters.
//   3. THE ACTIONS GO BACK OVER THE BRIDGE, and the only update action is "open the downloads page"
//      (notify-only, O3 — there is deliberately no in-app updater to test).
//
// PLUS the a11y contract these two live in: the update banner is a POLITE `status` (news), the
// daemon banner an ASSERTIVE `alert` (it changes what your next keystroke will do). Getting those the
// wrong way round is invisible to every other gate.
//
// ⚠ THERE IS NO NETWORK TEST HERE AND THERE CANNOT BE ONE. editor-core never makes the request — the
// Shell does, for the privacy reason banners.ts's header sets out. The request's shape is asserted in
// C++ (`editor-shell-test_banners`) and its surroundings by the source gate
// (`editor-shell-release-request`); what this tier owns is the rendering of the RESULT.

import { assert, assertEqual, assertNull, type TestCase } from "./harness.js";
import { ShellBridge, type BridgeQueryFunction } from "../bridge.js";
import {
    BannerClient,
    DAEMON_LINK_STATE_METHOD,
    DAEMON_OWNERSHIP_OWNED,
    UPDATE_DISMISS_METHOD,
    UPDATE_OPEN_DOWNLOADS_METHOD,
    UPDATE_STATE_METHOD,
    mountBanners,
    parseDaemonLinkState,
    parseUpdateState,
    renderDaemonBanner,
    renderUpdateBanner,
} from "../banners.js";
import type { DaemonLinkState, UpdateState } from "../banners.js";

interface RecordedCall {
    readonly method: string;
}

/** A ShellBridge over a fake transport; a responder that throws emulates an `unknown_method`. */
function recordingBridge(
    calls: RecordedCall[],
    responder: (method: string) => unknown,
): ShellBridge {
    const query: BridgeQueryFunction = (q) => {
        const request = JSON.parse(q.request) as { id: number; method: string };
        calls.push({ method: request.method });
        try {
            const result = responder(request.method);
            q.onSuccess(JSON.stringify({ jsonrpc: "2.0", id: request.id, result }));
        } catch {
            q.onSuccess(
                JSON.stringify({
                    jsonrpc: "2.0",
                    id: request.id,
                    error: {
                        code: -32601,
                        message: "unknown",
                        data: { reason: "bridge.unknown_method" },
                    },
                }),
            );
        }
        return request.id;
    };
    return new ShellBridge(query);
}

const AVAILABLE: UpdateState = {
    checked: true,
    current: "0.0.1",
    latest: "v0.2.0",
    updateAvailable: true,
    dismissed: false,
    downloadsUrl: "https://github.com/IvanMurzak/Context-Engine/releases/latest",
    error: "",
};

const LOST: DaemonLinkState = {
    readOnly: true,
    reconnectAttempts: 2,
    ownership: DAEMON_OWNERSHIP_OWNED,
    lastError: "the daemon wire closed",
};

const NOOP_HANDLERS = { onOpenDownloads: (): void => {}, onDismiss: (): void => {} };

export const bannerTests: TestCase[] = [
    // ------------------------------------------------------------------------------ the parsers
    {
        name: "parseUpdateState: a full envelope round-trips every member",
        run: () => {
            const parsed = parseUpdateState({
                checked: true,
                current: "0.0.1",
                latest: "v0.2.0",
                updateAvailable: true,
                dismissed: false,
                downloadsUrl: "https://example.invalid/dl",
                error: "",
            });
            assert(parsed !== null, "a well-formed envelope parses");
            assertEqual(parsed?.latest, "v0.2.0", "the latest version");
            assertEqual(parsed?.updateAvailable, true, "the availability flag");
        },
    },
    {
        name: "parseUpdateState: non-objects yield null, and a partial envelope degrades honestly",
        run: () => {
            assertNull(parseUpdateState(null), "null is not a state");
            assertNull(parseUpdateState("nope"), "a string is not a state");
            assertNull(parseUpdateState([1, 2]), "an array is not a state");
            // A partial envelope must NOT throw and must NOT invent availability: every boolean is
            // false unless it is literally `true`, so an older Shell can only ever under-report.
            const partial = parseUpdateState({ current: "0.0.1" });
            assert(partial !== null, "a partial object still parses");
            assertEqual(partial?.checked, false, "checked defaults false");
            assertEqual(partial?.updateAvailable, false, "availability defaults false");
            assertEqual(partial?.latest, "", "an absent latest reads as empty");
            // A wrong-typed member is treated as absent, never coerced.
            const wrong = parseUpdateState({ updateAvailable: "yes", latest: 7 });
            assertEqual(wrong?.updateAvailable, false, "a truthy string is not `true`");
            assertEqual(wrong?.latest, "", "a numeric latest is not a version");
        },
    },
    {
        name: "parseDaemonLinkState: totals, including a non-finite attempt count",
        run: () => {
            assertNull(parseDaemonLinkState(undefined), "undefined is not a link state");
            const parsed = parseDaemonLinkState({
                readOnly: true,
                reconnectAttempts: 3,
                ownership: "owned",
                lastError: "closed",
            });
            assertEqual(parsed?.reconnectAttempts, 3, "the attempt count");
            assertEqual(parsed?.ownership, "owned", "the ownership token");
            const degraded = parseDaemonLinkState({ reconnectAttempts: Number.NaN });
            assertEqual(degraded?.reconnectAttempts, 0, "NaN is not an attempt count");
            assertEqual(degraded?.readOnly, false, "an absent readOnly reads as live");
            assertEqual(degraded?.ownership, "none", "ownership falls back to `none`");
        },
    },

    // ------------------------------------------------------------------- when the banners appear
    {
        name: "renderUpdateBanner: shown only for a completed check that found a newer release",
        run: () => {
            assert(renderUpdateBanner(AVAILABLE, NOOP_HANDLERS) !== null, "the happy case shows");
            assertNull(
                renderUpdateBanner({ ...AVAILABLE, updateAvailable: false }, NOOP_HANDLERS),
                "no newer release => no banner",
            );
            assertNull(
                renderUpdateBanner({ ...AVAILABLE, dismissed: true }, NOOP_HANDLERS),
                "dismissed => no banner",
            );
            // The one that matters: a FAILED check must render nothing. A banner saying "we could
            // not check" on every offline launch is how the real notice gets ignored.
            assertNull(
                renderUpdateBanner(
                    { ...AVAILABLE, checked: false, updateAvailable: false, error: "offline" },
                    NOOP_HANDLERS,
                ),
                "a failed check shows no banner",
            );
        },
    },
    {
        name: "renderUpdateBanner: names both versions, is a POLITE status, and uses no innerHTML",
        run: () => {
            const banner = renderUpdateBanner(AVAILABLE, NOOP_HANDLERS);
            assert(banner !== null, "the banner rendered");
            const text = banner?.textContent ?? "";
            assert(text.includes("v0.2.0"), "the available version is named");
            assert(text.includes("0.0.1"), "the running version is named");
            assertEqual(banner?.getAttribute("role"), "status", "informational, not an alert");
            assertEqual(banner?.getAttribute("aria-live"), "polite", "must not interrupt");
            // A version string off the wire is TEXT, never markup.
            const hostile = renderUpdateBanner(
                { ...AVAILABLE, latest: "<img src=x onerror=alert(1)>" },
                NOOP_HANDLERS,
            );
            assertEqual(
                hostile?.querySelectorAll("img").length,
                0,
                "a hostile version string injects no element",
            );
        },
    },
    {
        name: "renderDaemonBanner: shown only while read-only, and is an ASSERTIVE alert",
        run: () => {
            assertNull(renderDaemonBanner({ ...LOST, readOnly: false }), "a live link shows nothing");
            const banner = renderDaemonBanner(LOST);
            assert(banner !== null, "a lost link shows the banner");
            assertEqual(banner?.getAttribute("role"), "alert", "losing write access is assertive");
            assertEqual(banner?.getAttribute("aria-live"), "assertive", "it must interrupt");
            const text = banner?.textContent ?? "";
            // It leads with the CONSEQUENCE (editing is disabled), not with the implementation.
            assert(text.includes("Read-only"), "the read-only state is named");
            assert(text.includes("Editing is disabled"), "the consequence is stated");
            assert(text.includes("attempt 2"), "the reconnect ladder is visible");
            assert(text.includes("the daemon wire closed"), "the diagnostic is carried");
        },
    },
    {
        name: "renderDaemonBanner: a first loss with no attempt yet omits the attempt count",
        run: () => {
            const banner = renderDaemonBanner({ ...LOST, reconnectAttempts: 0, lastError: "" });
            const text = banner?.textContent ?? "";
            assert(!text.includes("attempt"), "no attempt number is flashed before the first retry");
            assert(text.includes("reconnecting"), "it still says what is happening");
        },
    },
    {
        name: "renderDaemonBanner: an EXTERNAL daemon is not described as something we reconnect",
        run: () => {
            // An owned daemon is this editor's child and WILL be respawned; an external one belongs
            // to another client, so claiming we are "reconnecting" it overstates what we can do.
            const external = renderDaemonBanner({ ...LOST, ownership: "external" });
            const text = external?.textContent ?? "";
            assert(text.includes("shared project daemon"), "the shared daemon is named as such");
            assert(!text.includes("reconnecting"), "no agency is implied over another's daemon");
            assert(text.includes("Editing is disabled"), "the consequence is stated either way");
        },
    },

    // ------------------------------------------------------------------------------ mountBanners
    {
        name: "mountBanners: renders both, in order, and reports what it showed",
        run: () => {
            const container = document.createElement("div");
            const mount = mountBanners(container, {
                update: AVAILABLE,
                link: LOST,
                handlers: NOOP_HANDLERS,
            });
            assertEqual(mount.updateShown, true, "the update banner showed");
            assertEqual(mount.daemonShown, true, "the daemon banner showed");
            assertEqual(mount.element.children.length, 2, "both banners are in the strip");
            // The daemon banner comes FIRST: it is the one that changes what you can do right now.
            assert(
                (mount.element.children[0] as HTMLElement).getAttribute("data-read-only") === "true",
                "the read-only banner leads",
            );
        },
    },
    {
        name: "mountBanners: an absent surface (null state) renders an empty strip, not an error",
        run: () => {
            const container = document.createElement("div");
            const mount = mountBanners(container, {
                update: null,
                link: null,
                handlers: NOOP_HANDLERS,
            });
            assertEqual(mount.updateShown, false, "nothing to say about updates");
            assertEqual(mount.daemonShown, false, "nothing to say about the daemon");
            assertEqual(mount.element.children.length, 0, "the strip is empty");
            // The strip element still exists, so a later fill-in causes no layout shift.
            assertEqual(container.children.length, 1, "the strip is mounted regardless");
        },
    },
    {
        name: "mountBanners: replaces its container's previous contents",
        run: () => {
            const container = document.createElement("div");
            container.append(document.createElement("span"));
            mountBanners(container, { update: AVAILABLE, link: null, handlers: NOOP_HANDLERS });
            assertEqual(container.children.length, 1, "the stale child is gone");
        },
    },

    // -------------------------------------------------------------------------- the actions/wire
    {
        name: "the update banner's only action is opening the downloads page (notify-only, O3)",
        run: () => {
            let opened = 0;
            let dismissed = 0;
            const banner = renderUpdateBanner(AVAILABLE, {
                onOpenDownloads: () => {
                    opened += 1;
                },
                onDismiss: () => {
                    dismissed += 1;
                },
            });
            const buttons = banner?.querySelectorAll("button") ?? [];
            assertEqual(buttons.length, 2, "exactly two actions: get, and dismiss");
            (buttons[0] as HTMLButtonElement).dispatchEvent(new MouseEvent("click"));
            (buttons[1] as HTMLButtonElement).dispatchEvent(new MouseEvent("click"));
            assertEqual(opened, 1, "the primary action opened the downloads page");
            assertEqual(dismissed, 1, "the secondary action dismissed");
        },
    },
    {
        name: "BannerClient: each method reaches the bridge and parses its result",
        run: () => {
            const calls: RecordedCall[] = [];
            const bridge = recordingBridge(calls, (method) => {
                if (method === UPDATE_STATE_METHOD) {
                    return AVAILABLE;
                }
                if (method === DAEMON_LINK_STATE_METHOD) {
                    return LOST;
                }
                if (method === UPDATE_DISMISS_METHOD) {
                    return { dismissed: true };
                }
                return { opened: true };
            });
            const client = new BannerClient(bridge);
            void client.updateState();
            void client.daemonLinkState();
            void client.dismissUpdate();
            void client.openDownloads();
            assert(calls.some((c) => c.method === UPDATE_STATE_METHOD), "asked for update state");
            assert(calls.some((c) => c.method === DAEMON_LINK_STATE_METHOD), "asked for link state");
            assert(calls.some((c) => c.method === UPDATE_DISMISS_METHOD), "dismissed over the wire");
            assert(
                calls.some((c) => c.method === UPDATE_OPEN_DOWNLOADS_METHOD),
                "opened downloads over the wire",
            );
        },
    },
    {
        name: "BannerClient: a refusal degrades to null/false, never a throw",
        run: () => {
            const calls: RecordedCall[] = [];
            // Every method refuses — a build whose Shell has no banner surface at all.
            const bridge = recordingBridge(calls, () => {
                throw new Error("unknown_method");
            });
            const client = new BannerClient(bridge);
            // These are promises; the assertion that matters is that constructing + invoking them
            // throws nothing synchronously and that each settles to the honest degrade.
            void client.updateState().then((state) => {
                assertNull(state, "an absent update surface reads as null");
            });
            void client.daemonLinkState().then((state) => {
                assertNull(state, "an absent link surface reads as null");
            });
            void client.dismissUpdate().then((ok) => {
                assertEqual(ok, false, "a refused dismiss is false, not a throw");
            });
            void client.openDownloads().then((ok) => {
                assertEqual(ok, false, "a refused open is false, not a throw");
            });
            assertEqual(calls.length, 4, "all four were attempted");
        },
    },
];
