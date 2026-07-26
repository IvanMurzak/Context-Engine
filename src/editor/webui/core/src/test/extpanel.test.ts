// T1 for the THIRD-PARTY PACKAGE PANEL host (M9 e13a-2, design 04 §5 / 08 §1-§2): the
// `context-ext://` entry grammar (extpanel.ts) and the `iframe` content type in PanelHost.
//
// TWO TIERS IN ONE FILE, on purpose.
//
//   1. THE GRAMMAR, driven directly. `parseExtPanelEntry` is the ONLY layer at which a manifest
//      entry of `javascript:…`, `data:…` or `https://evil.example/` can be refused at all — the
//      Shell's `ExtAssetResolver` never sees a URL the browser did not route to it, so a non-
//      `context-ext:` entry would simply be LOADED, from somewhere else, with no refusal anywhere.
//      The table below therefore drives the same shapes the C++ suite drives against
//      `is_valid_package_id` (test_ext_scheme.cpp), so the two mirrors are kept honest against each
//      other rather than each against itself.
//
//   2. THE RENDERED FRAME, against REAL Dockview in a REAL browser. The DoD line is "`sandbox` +
//      CSP asserted on the rendered frame (NOT merely set in source)", and that distinction is the
//      whole reason these cases mount a live PanelHost instead of unit-testing a renderer class:
//      an assertion on `IFRAME_SANDBOX` proves what someone wrote, while an assertion on the
//      element's `DOMTokenList` proves what the browser will ENFORCE. The two differ exactly when
//      it matters — a token appended at a call site, an attribute set in the wrong order, a
//      `sandbox` the element never received at all.
//
// WHAT THIS TIER STRUCTURALLY CANNOT REACH, so it is not attempted here and is not claimed: whether
// the package's document actually LOADS over the scheme, and whether the response CSP the Shell
// serves permits the panel's own module graph. Both require the real `context-ext://` handler, which
// exists only in the CEF binding — they are `editor-cef-smoke-shell-iframe`'s (T2), and that smoke's
// served-URL assertions are what settle them.

import { assert, assertEqual, type TestCase } from "./harness.js";
import { ShellBridge, type BridgeQuery, type BridgeQueryFunction } from "../bridge.js";
import { detectDockview } from "../dockview.js";
import {
    EXT_PACKAGE_ID_MAX_LENGTH,
    EXT_SCHEME,
    EXT_URL_PREFIX,
    IFRAME_SANDBOX,
    isSandboxSafe,
    isValidPackageId,
    parseExtPanelEntry,
} from "../extpanel.js";
import { IFRAME_PANEL_CLASS, PanelHost } from "../panelhost.js";
import type { PanelVerbFactory } from "../panelhost.js";
import { PANEL_PORT_STATE_ATTRIBUTE } from "../panelport.js";
import { PANEL_LIST_METHOD, PanelClient, parsePanelManifest } from "../panels.js";

// --------------------------------------------------------------------------- roster + Shell mocks

interface ManifestOverrides {
    readonly id?: string;
    readonly contentType?: string;
    readonly entry?: string;
    readonly hosted?: boolean;
}

/** One `panel.list` entry in the WIRE shape `PanelHost::list()` emits (panel_host.cpp). */
function manifestJson(overrides: ManifestOverrides = {}): Record<string, unknown> {
    return {
        id: overrides.id ?? "pkg.hello",
        kind: "panel",
        title: "Hello",
        icon: "",
        contractVersion: 2,
        dock: { zone: "right", singleton: true, minWidth: 0, minHeight: 0 },
        content: {
            type: overrides.contentType ?? "iframe",
            entry: overrides.entry ?? "context-ext://pkg.hello/index.html",
        },
        state: { schemaVersion: 1 },
        capabilities: [],
        commands: [],
        // An iframe panel has NO Shell provider by construction (its bytes are served over
        // `context-ext://`, not over `panel.render`), so the Shell reports `hosted: false` for it —
        // exactly as it does for the one `local` panel. Defaulted false here for that reason: a
        // fixture that quietly set it true would be testing a manifest the Shell cannot produce.
        hosted: overrides.hosted ?? false,
        gestures: false,
        persists: false,
        revision: 1,
    };
}

/** A `uitree` panel WITH a provider — the "docks like a built-in" comparison subject. */
function uitreeManifestJson(id: string): Record<string, unknown> {
    return {
        ...manifestJson({ id, contentType: "uitree", entry: "", hosted: true }),
        title: id,
    };
}

interface MockShell {
    readonly bridge: ShellBridge;
    /** Every method the host actually called, in order — the non-vacuity witness. */
    readonly methods: string[];
}

/** A mock Shell serving one roster; every other method is refused as the real router's default does. */
function mockShell(panels: readonly Record<string, unknown>[]): MockShell {
    const methods: string[] = [];
    let served = 0;
    const query: BridgeQueryFunction = (request: BridgeQuery): number => {
        const parsed = JSON.parse(request.request) as { id: number; method: string };
        methods.push(parsed.method);
        served += 1;
        const envelope =
            parsed.method === PANEL_LIST_METHOD
                ? { jsonrpc: "2.0", id: parsed.id, result: { contractMajor: 2, panels } }
                : {
                      jsonrpc: "2.0",
                      id: parsed.id,
                      error: {
                          code: -32601,
                          message: "unknown method",
                          data: { reason: "bridge.unknown_method" },
                      },
                  };
        request.onSuccess(JSON.stringify(envelope));
        return served;
    };
    return { bridge: new ShellBridge(query), methods };
}

interface Mounted {
    readonly host: PanelHost;
    readonly container: HTMLElement;
    readonly shell: MockShell;
    dispose(): void;
}

/**
 * Start a REAL PanelHost over REAL Dockview with the given roster.
 *
 * Real Dockview, not a stand-in, for the same reason `theme_dom.test.ts` mounts one: the engine's
 * own `createComponent` call, its DOM insertion and its lifecycle hooks are what actually decide
 * whether the frame this file asserts on ever exists. A hand-made element would pass while the
 * shipped app failed.
 */
async function mountHost(
    panels: readonly Record<string, unknown>[],
    panelVerbs?: PanelVerbFactory,
): Promise<Mounted> {
    const dockview = detectDockview();
    assert(
        dockview !== undefined,
        "the pinned dockview-core UMD global is loaded — harness.html must load " +
            "dockview-core.min.js before the test bundle, or this whole file passes vacuously",
    );
    const dv = dockview as NonNullable<typeof dockview>;
    const container = window.document.createElement("div");
    container.style.width = "800px";
    container.style.height = "600px";
    window.document.body.appendChild(container);
    const shell = mockShell(panels);
    const host = new PanelHost({
        container,
        client: new PanelClient(shell.bridge),
        dockview: dv,
        ...(panelVerbs === undefined ? {} : { panelVerbs }),
    });
    await host.start();
    return {
        host,
        container,
        shell,
        dispose: (): void => {
            host.dispose();
            container.remove();
        },
    };
}

/** The rendered frame for a panel id, or null when the host mounted none. */
function frameFor(mounted: Mounted, panelId: string): HTMLIFrameElement | null {
    return mounted.container.querySelector<HTMLIFrameElement>(
        `.${IFRAME_PANEL_CLASS} > iframe[data-panel-id="${panelId}"]`,
    );
}

// ------------------------------------------------------------------------------- the grammar table
//
// Each entry is (entry string, expected-valid, why). The INVALID half is the load-bearing one, and
// every row of it names a real escape route rather than a typo.

const ENTRY_CASES: readonly (readonly [string, boolean, string])[] = [
    ["context-ext://pkg.hello/index.html", true, "the ordinary shape"],
    ["context-ext://pkg", true, "a bare package resolves to the default document Shell-side"],
    ["context-ext://pkg/", true, "a trailing slash is the same request"],
    ["context-ext://a-b_c.d/deep/nested/panel.js", true, "the full id character set + a deep path"],
    ["context-ext://pkg/panel.html?v=2#top", true, "query + fragment are cut by the resolver"],

    ["javascript:alert(1)", false, "the classic — a manifest that scripts the EDITOR's document"],
    ["data:text/html,<script>1</script>", false, "an inline document with no origin at all"],
    ["blob:context-editor://app/1234", false, "a blob URL inherits an origin we do not control"],
    ["file:///c:/windows/win.ini", false, "the local filesystem; there is no file:// path anywhere"],
    ["https://evil.example/panel.html", false, "an external host — the exfiltration route 08 §2 names"],
    ["context-editor://app/index.html", false, "the EDITOR's own privileged origin, framed by itself"],
    ["//pkg.hello/index.html", false, "protocol-relative: inherits the embedder's scheme"],
    ["context-ext:/pkg/index.html", false, "one slash — not the scheme's authority form"],
    ["context-ext:///index.html", false, "an EMPTY authority names no package"],
    ["CONTEXT-EXT://pkg/index.html", false, "the SCHEME's own case: the prefix match is byte-exact, so a non-canonical spelling is refused rather than normalized into an accepted one (ONE row — every case-normalizing comparison accepts all-caps and mixed alike, so a second spelling would exercise the same predicate twice)"],
    ["context-ext://PKG/index.html", false, "upper case: Chromium lower-cases a host, so it is unreachable"],
    ["context-ext://a@b/index.html", false, "userinfo — would resolve against host `b`"],
    ["context-ext://pkg:9/index.html", false, "a port is not part of the id grammar"],
    ["context-ext://pkg/../../etc/passwd", false, "traversal, refused before a frame is ever created"],
    ["context-ext://../index.html", false, "the authority itself is a traversal"],
    ["context-ext://pkg\\index.html", false, "a backslash: a path separator to Windows, a slash to the URL parser"],
    ["context-ext://pkg /index.html", false, "embedded whitespace"],
    [" context-ext://pkg/index.html", false, "leading whitespace the URL parser would strip"],
    ["context-ext://pkg/index\u0000.html", false, "an embedded NUL"],
    ["", false, "the empty entry — what a `uitree` manifest carries"],
];

export const extPanelTests: readonly TestCase[] = [
    // ------------------------------------------------------------------- tier 1: the pure grammar
    {
        name: "extpanel: the scheme vocabulary is the one the Shell serves",
        run: (): void => {
            assertEqual(EXT_SCHEME, "context-ext", "the scheme mirrors C++ kExtScheme");
            assertEqual(
                EXT_URL_PREFIX,
                `${EXT_SCHEME}://`,
                "the prefix is the scheme's authority form — the webui-scheme-contract ctest " +
                    "byte-compares both against ext_scheme.h",
            );
        },
    },
    {
        name: "extpanel: parseExtPanelEntry accepts context-ext:// URLs and refuses every other shape",
        run: (): void => {
            for (const [entry, valid, why] of ENTRY_CASES) {
                const parsed = parseExtPanelEntry(entry);
                assertEqual(
                    parsed !== null,
                    valid,
                    `parseExtPanelEntry(${JSON.stringify(entry)}) should be ` +
                        `${valid ? "accepted" : "REFUSED"} — ${why}`,
                );
            }
        },
    },
    {
        name: "extpanel: an accepted entry is returned VERBATIM, never re-normalized",
        run: (): void => {
            const entry = "context-ext://pkg.hello/deep/panel.html?v=2#top";
            const parsed = parseExtPanelEntry(entry);
            assert(parsed !== null, "the entry parses");
            assertEqual(
                (parsed as NonNullable<typeof parsed>).url,
                entry,
                "the URL handed to the frame is the string that was validated — a normalizing " +
                    "parser would make the validated value and the requested value two different " +
                    "strings produced by two different pieces of code",
            );
            assertEqual(
                (parsed as NonNullable<typeof parsed>).packageId,
                "pkg.hello",
                "the authority is taken textually, up to the first / ? or #",
            );
        },
    },
    {
        name: "extpanel: a percent-encoded traversal parses here and is the RESOLVER's refusal",
        run: (): void => {
            // Deliberately NOT refused in editor-core: `%2f` is not a path separator until the
            // resolver decodes it, and a validator that decoded on its own would be a SECOND decoder
            // free to disagree with the one that actually opens the file. `ExtAssetResolver` decodes
            // once and refuses (`split_safe_path_segments`), which is where the decision belongs.
            const parsed = parseExtPanelEntry("context-ext://pkg/..%2f..%2fetc");
            assert(
                parsed !== null,
                "editor-core accepts the ENCODED form (it is a well-formed context-ext URL); the " +
                    "Shell refuses it after decoding, and the smoke's refused-URL assertion is where " +
                    "that is proven end to end",
            );
        },
    },
    {
        name: "extpanel: isValidPackageId mirrors the C++ grammar, including its two correctness rules",
        run: (): void => {
            const valid = ["a", "pkg", "pkg2", "context.hello-panel_2", "a-b_c.d", "0abc"];
            for (const id of valid) {
                assert(isValidPackageId(id), `${id} is a valid package id`);
            }
            const invalid: readonly (readonly [string, string])[] = [
                ["", "empty"],
                ["PKG", "upper case can never be matched by a lower-cased host"],
                // INTERIOR upper case is a DISTINCT axis from a leading one, and this row exists
                // because planting "stop refusing upper case in the character loop" left the gate
                // GREEN: `PKG` is refused by the FIRST-character check alone, so it exercises the
                // loop not at all. Every character-class row below is doubled for the same reason —
                // the interior is the only position the loop is the sole decider.
                ["paCkage", "upper case INSIDE the id — refused by the character loop, not the ends"],
                ["pkäg", "non-ASCII inside"],
                ["pk%2eg", "percent-encoding inside"],
                ["pk:g", "a colon inside"],
                ["pk/g", "a slash inside"],
                ["pk\\g", "a backslash inside"],
                ["pk g", "a space inside"],
                ["-pkg", "leading '-' reads as a path fragment"],
                ["pkg-", "trailing '-'"],
                [".pkg", "leading '.'"],
                ["pkg.", "trailing '.'"],
                [".", "a bare dot"],
                ["..", "the parent directory"],
                ["a..b", "an interior traversal spelling"],
                ["12345", "an all-digit host goes to the IPv4 parser (0.0.48.57)"],
                ["pkg.2", "an all-digit LAST label fails host parsing outright"],
                ["pkg/sub", "a slash is not in the grammar"],
                ["pkg:9", "a port is not part of the id"],
                ["a@b", "userinfo"],
                ["pkg%2e", "percent-encoding"],
                ["päckage", "non-ASCII"],
                ["a".repeat(EXT_PACKAGE_ID_MAX_LENGTH + 1), "over the length bound"],
            ];
            for (const [id, why] of invalid) {
                assert(!isValidPackageId(id), `${JSON.stringify(id)} must be REFUSED — ${why}`);
            }
            assert(
                isValidPackageId("a".repeat(EXT_PACKAGE_ID_MAX_LENGTH)),
                "the bound itself is inclusive, matching kExtPackageIdMaxLength",
            );
        },
    },
    {
        name: "extpanel: isSandboxSafe refuses allow-same-origin by ANY route",
        run: (): void => {
            const frame = window.document.createElement("iframe");
            frame.setAttribute("sandbox", IFRAME_SANDBOX);
            assert(isSandboxSafe(frame), "the reviewed list is safe");
            frame.setAttribute("sandbox", "allow-scripts allow-same-origin");
            assert(
                !isSandboxSafe(frame),
                "allow-scripts + allow-same-origin lets the frame remove its OWN sandbox attribute " +
                    "from the embedding document — the single highest-value token to refuse",
            );
            frame.setAttribute("sandbox", "allow-scripts ALLOW-SAME-ORIGIN");
            assert(!isSandboxSafe(frame), "token matching is case-insensitive, as the browser's is");
            frame.setAttribute("sandbox", "");
            assert(!isSandboxSafe(frame), "an EMPTY sandbox list grants no scripts — not our policy");
            frame.removeAttribute("sandbox");
            assert(
                !isSandboxSafe(frame),
                "NO sandbox attribute is the WORST case (a fully privileged frame), not the safest",
            );
        },
    },
    {
        name: "extpanel: the manifest parser carries content.entry through, total and unvalidated",
        run: (): void => {
            const parsed = parsePanelManifest(manifestJson());
            assert(parsed !== null, "the fixture manifest parses");
            const manifest = parsed as NonNullable<typeof parsed>;
            assertEqual(manifest.contentType, "iframe", "the content type is read");
            assertEqual(
                manifest.contentEntry,
                "context-ext://pkg.hello/index.html",
                "content.entry reaches the manifest — without it PanelHost has no URL to mount",
            );
            const noContent = parsePanelManifest({ id: "x" });
            assert(noContent !== null, "a manifest with no content block still parses");
            assertEqual(
                (noContent as NonNullable<typeof noContent>).contentEntry,
                "",
                "a missing entry is the empty string, never undefined — the parser stays total",
            );
        },
    },

    // ----------------------------------------------------------- tier 2: the RENDERED frame (DOM)
    {
        name: "iframe panel: PanelHost mounts a real <iframe> on the package's own context-ext origin",
        run: async () => {
            const mounted = await mountHost([manifestJson()]);
            try {
                assertEqual(
                    mounted.host.mounted.length,
                    1,
                    "the iframe panel MOUNTED even though the Shell reports hosted:false — it has " +
                        "no Shell provider by construction, exactly like the one `local` panel",
                );
                const frame = frameFor(mounted, "pkg.hello");
                assert(frame !== null, "a real <iframe> element is in the docked panel's slot");
                const iframe = frame as HTMLIFrameElement;
                assertEqual(
                    iframe.getAttribute("src"),
                    "context-ext://pkg.hello/index.html",
                    "the frame points at the package's own origin, verbatim from the manifest",
                );
                assertEqual(
                    mounted.shell.methods.filter((m) => m === "panel.render").length,
                    0,
                    "NOTHING called panel.render for it: a package panel's bytes come over " +
                        "context-ext://, and a render call would mean it fell through to the " +
                        "innerHTML-mounting uitree renderer",
                );
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "iframe panel: the RENDERED frame is sandboxed allow-scripts, WITHOUT allow-same-origin",
        run: async () => {
            const mounted = await mountHost([manifestJson()]);
            try {
                const frame = frameFor(mounted, "pkg.hello");
                assert(frame !== null, "the frame was rendered");
                const iframe = frame as HTMLIFrameElement;
                // Asserted through the browser's OWN parsed token list, not the attribute string:
                // this is what will actually be enforced.
                const tokens = Array.from(iframe.sandbox);
                assertEqual(
                    tokens.join(" "),
                    "allow-scripts",
                    `the enforced sandbox list is exactly allow-scripts, got: ${tokens.join(" ")}`,
                );
                assert(
                    !iframe.sandbox.contains("allow-same-origin"),
                    "allow-same-origin is ABSENT — the frame's origin stays opaque, which is what " +
                        "isolates each package from every other one and from the editor",
                );
                assert(
                    !iframe.sandbox.contains("allow-top-navigation") &&
                        !iframe.sandbox.contains("allow-top-navigation-by-user-activation"),
                    "a package panel may not navigate the editor away",
                );
                assert(
                    !iframe.sandbox.contains("allow-popups") &&
                        !iframe.sandbox.contains("allow-modals") &&
                        !iframe.sandbox.contains("allow-downloads") &&
                        !iframe.sandbox.contains("allow-forms"),
                    "no capability nobody asked for",
                );
                assert(isSandboxSafe(iframe), "the element passes the host's own sandbox assertion");
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "iframe panel: the RENDERED frame delegates NO powerful feature and leaks no referrer",
        run: async () => {
            const mounted = await mountHost([manifestJson()]);
            try {
                const frame = frameFor(mounted, "pkg.hello");
                assert(frame !== null, "the frame was rendered");
                const iframe = frame as HTMLIFrameElement;
                assertEqual(
                    iframe.getAttribute("allow"),
                    "",
                    "the Permissions-Policy delegation is EMPTY and explicit — a cross-origin " +
                        "frame is denied every powerful feature unless the EMBEDDER delegates, " +
                        "which is why the Shell's response carries no Permissions-Policy header",
                );
                assertEqual(
                    iframe.getAttribute("referrerpolicy"),
                    "no-referrer",
                    "the URL names the package; a panel must not leak which package it is",
                );
                assertEqual(
                    iframe.getAttribute("title"),
                    "pkg.hello",
                    "the frame is a LABELLED landmark (R-A11Y-001) — a screen reader announces " +
                        "which panel focus moved into",
                );
                assertEqual(
                    iframe.getAttribute("loading"),
                    "eager",
                    "a docked panel is visible by construction, so it loads EAGERLY. This restates " +
                        "the HTML default on purpose and is asserted for the same reason `allow` " +
                        "is: an attribute set explicitly is a decision, and a decision nothing " +
                        "checks is a comment",
                );
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "iframe panel: docks alongside a built-in and FLOATS like one",
        run: async () => {
            const mounted = await mountHost([uitreeManifestJson("builtin.problems"), manifestJson()]);
            try {
                assertEqual(
                    mounted.host.mounted.join(","),
                    "builtin.problems,pkg.hello",
                    "both panels are docked, in roster order",
                );
                assert(
                    mounted.host.api?.getPanel("pkg.hello") !== undefined,
                    "Dockview owns the package panel exactly as it owns the built-in — the " +
                        "geometry layer never learns there is a trust tier",
                );
                assert(
                    mounted.host.floatPanel("pkg.hello"),
                    "the package panel FLOATS: dockview-core renders a floating group with an " +
                        "inline position:absolute, so this is the rendered result, not a selector",
                );
                assert(
                    frameFor(mounted, "pkg.hello") !== null,
                    "the frame SURVIVED the float — a rebuild here would reload the package's " +
                        "document and discard whatever the user was doing inside it",
                );
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "iframe panel: an entry editor-core may not load is UNAVAILABLE, never mounted",
        run: async () => {
            const hostile = [
                "javascript:alert(document.domain)",
                "data:text/html,<script>1</script>",
                "https://evil.example/panel.html",
                "context-editor://app/index.html",
                "",
            ];
            for (const entry of hostile) {
                const mounted = await mountHost([manifestJson({ entry })]);
                try {
                    assertEqual(
                        mounted.host.mounted.length,
                        0,
                        `an entry of ${JSON.stringify(entry)} must mount NOTHING`,
                    );
                    assertEqual(
                        mounted.container.querySelectorAll("iframe").length,
                        0,
                        `no frame exists for ${JSON.stringify(entry)} — the refusal is upstream of ` +
                            "the element, not a broken frame left in the DOM",
                    );
                    assertEqual(
                        mounted.shell.methods.filter((m) => m === "panel.render").length,
                        0,
                        "and it did NOT fall through to the uitree renderer's innerHTML sink",
                    );
                } finally {
                    mounted.dispose();
                }
            }
        },
    },
    {
        name: "iframe panel: openById cannot walk past the gate (the e10b seed path)",
        run: async () => {
            // THE REGRESSION THIS PINS: the content-type gate used to live inline in `start`'s loop,
            // so `openById` — e10b's tear-out seed path, which looks a manifest up from the roster
            // and calls `open` directly — reached `addPanel` with no gate at all. A seeded window
            // could therefore mount a panel `start` had just reported unavailable.
            const mounted = await mountHost([manifestJson({ entry: "javascript:alert(1)" })]);
            try {
                assert(
                    !mounted.host.openById("pkg.hello"),
                    "openById REFUSES a panel this build cannot mount",
                );
                assertEqual(mounted.container.querySelectorAll("iframe").length, 0, "nothing mounted");
                assertEqual(
                    mounted.shell.methods.filter((m) => m === "panel.render").length,
                    0,
                    "and nothing reached the uitree renderer either",
                );
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "iframe panel: an unknown content type is refused, not defaulted into a renderer",
        run: async () => {
            const mounted = await mountHost([
                manifestJson({ contentType: "webview", entry: "context-ext://pkg.hello/x.html" }),
            ]);
            try {
                assertEqual(
                    mounted.host.mounted.length,
                    0,
                    "`parsePanelManifest` fails closed to `unknown` for a token this build does " +
                        "not recognise, and `unknown` is never mountable — defaulting a drifted " +
                        "manifest into ANY renderer is how a future content type reaches today's sink",
                );
                assertEqual(mounted.container.querySelectorAll("iframe").length, 0, "no frame");
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "iframe panel: restoreLayout is the THIRD caller, and it cannot reach the sink either",
        run: async () => {
            // THE REGRESSION THIS PINS. `#mountable` moving into `open` closed `openById`, but a
            // third production caller never went through `open` at all: `restoreLayout` hands a
            // persisted arrangement straight to Dockview's `fromJSON`, which calls `createComponent`
            // → `#create` → `#renderer`. That path is reached at BOOT, from a blob captured by a
            // DIFFERENT build — so the manifest it looks up can have drifted to `unknown` since the
            // layout was written, and `#renderer`'s fall-through used to be `UitreePanelRenderer`,
            // the `innerHTML` hydration sink. `#renderer` is now fail-closed for every content type.
            const author = await mountHost([manifestJson()]);
            let layout: unknown;
            try {
                assert(frameFor(author, "pkg.hello") !== null, "the authoring host mounted a frame");
                layout = author.host.captureLayout();
                assert(layout !== null, "and produced a layout to restore");
            } finally {
                author.dispose();
            }

            // The SAME panel id, whose content type this build no longer understands.
            const drifted = await mountHost([
                manifestJson({ contentType: "webview", entry: "context-ext://pkg.hello/x.html" }),
            ]);
            try {
                const rendersBefore = drifted.shell.methods.filter(
                    (m) => m === "panel.render",
                ).length;
                assert(drifted.host.restoreLayout(layout), "the arrangement restores");
                // ANTI-VACUITY: without this the assertions below would also pass on a Dockview that
                // never created the panel at all, which would prove nothing about `#renderer`.
                assertEqual(
                    drifted.host.mounted.length,
                    1,
                    "Dockview DID drive createComponent for the persisted panel — this path really " +
                        "does reach `#renderer` without passing `open`",
                );
                assertEqual(
                    drifted.shell.methods.filter((m) => m === "panel.render").length - rendersBefore,
                    0,
                    "and the drifted manifest reached NO renderer that hydrates: a `panel.render` " +
                        "here is the uitree sink drawing untrusted-shaped content it was never " +
                        "gated for",
                );
                assertEqual(
                    drifted.container.querySelectorAll("iframe").length,
                    0,
                    "nor did it fall the other way into a frame",
                );
                assertEqual(
                    drifted.container.querySelectorAll("[data-panel-unavailable]").length,
                    1,
                    "and the slot is MARKED unavailable rather than merely empty — an empty slot " +
                        "is indistinguishable from a healthy panel that has not drawn yet, which " +
                        "would make a missing panel something a user discovers by its absence",
                );
            } finally {
                drifted.dispose();
            }
        },
    },
    {
        // THE e13b-1 WIRING, asserted on the RENDERED element — the same discipline this file applies
        // to the sandbox attribute, and for the same reason: the bridge's own behaviour is proven in
        // `panelport.test.ts` against a real opaque-origin child, but that nothing in production
        // FORGETS to build one (and to build it before `src`) is a property of `IframePanelRenderer`,
        // and the port state reflected onto the frame is the only thing here that can see it.
        //
        // Nothing local can load a `context-ext://` document, so the frame never handshakes — which is
        // exactly what makes `pending` the right assertion: it can only have been written by a bridge
        // that was constructed, and it can only be `pending` (rather than absent) if that happened
        // before the load this frame will never complete.
        name: "iframe panel: the RENDERED frame carries a port bridge from construction, and loses it on close",
        run: async () => {
            const mounted = await mountHost([manifestJson()]);
            try {
                const frame = frameFor(mounted, "pkg.hello");
                assert(frame !== null, "the frame was rendered");
                const iframe = frame as HTMLIFrameElement;
                assertEqual(
                    iframe.getAttribute(PANEL_PORT_STATE_ATTRIBUTE),
                    "pending",
                    "a port bridge exists for this frame and is awaiting the Shell-injected " +
                        "bootstrap's handshake. An ABSENT attribute would mean the renderer built no " +
                        "bridge at all, so every panel would be silently portless with no local signal",
                );
                // CLOSING the panel disposes the renderer, which must dispose the bridge — otherwise a
                // closed panel leaves a `message` listener on the editor's window for a frame that is
                // gone.
                assert(mounted.host.close("pkg.hello"), "the panel closed");
                assertEqual(
                    iframe.getAttribute(PANEL_PORT_STATE_ATTRIBUTE),
                    "revoked",
                    "and the bridge was DISPOSED with the renderer — asserted on the element captured " +
                        "before the close, since the element itself is removed from the DOM",
                );
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        // ⚠ PLANT: remove `this.#verbs.dispose()` from `IframePanelRenderer.dispose` (panelhost.ts)
        // and this case goes RED. It is the ONLY case that pins the WIRING — `panelverbs.test.ts`
        // proves the table's `dispose()` withdraws its commands, but calls it directly, so without
        // this the renderer could simply never call it and every suite would stay green.
        name: "iframe panel: closing the panel disposes its VERB TABLE, so its commands are withdrawn",
        run: async () => {
            const disposals: string[] = [];
            const mounted = await mountHost([manifestJson()], (binding) => {
                disposals.push(`built:${binding.panelId}`);
                return {
                    verbs: new Map(),
                    dispose: (): void => {
                        disposals.push(`disposed:${binding.panelId}`);
                    },
                };
            });
            try {
                assert(frameFor(mounted, "pkg.hello") !== null, "the frame is up");
                assertEqual(
                    JSON.stringify(disposals),
                    JSON.stringify(["built:pkg.hello"]),
                    "the host built the panel's verb table and has not torn it down yet",
                );
                assert(mounted.host.close("pkg.hello"), "the panel closed");
                // ⚠ AT LEAST once, not EXACTLY once — measured: `PanelHost.close` reaches
                // `IframePanelRenderer.dispose` TWICE (Dockview's own `removePanel` teardown, then
                // the explicit `renderer.dispose()` call), so the count is not the contract. That is
                // precisely why `PanelPortBridge.dispose` documents itself idempotent, and the verb
                // table's `dispose` is idempotent for the same reason (it clears `registered`, so the
                // second pass withdraws nothing). Pinning an exact count here would pin Dockview's
                // teardown shape, not ours.
                assert(
                    disposals.filter((entry) => entry === "disposed:pkg.hello").length >= 1,
                    "closing the panel disposed its verb table — the hook that withdraws the panel's " +
                        `runtime commands from the ONE registry, which outlives every panel: ${JSON.stringify(disposals)}`,
                );
            } finally {
                mounted.dispose();
            }
        },
    },
    {
        name: "iframe panel: disposing the host tears the package's document down",
        run: async () => {
            const mounted = await mountHost([manifestJson()]);
            assert(frameFor(mounted, "pkg.hello") !== null, "the frame is up");
            mounted.host.dispose();
            assertEqual(
                mounted.container.querySelectorAll("iframe").length,
                0,
                "the frame is GONE — removing the element is what actually ends the package's " +
                    "document, and a leaked frame is untrusted code still running",
            );
            mounted.container.remove();
        },
    },
];
