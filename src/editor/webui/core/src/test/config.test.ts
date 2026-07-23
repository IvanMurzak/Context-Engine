// T1 unit tests for the per-user config surface (config.ts, M9 e06d).
//
// Two properties this tier pins. (1) Every parser is TOTAL: a malformed, partial or hostile
// `config.get` envelope yields the documented empty snapshot, never a throw — this runs at boot,
// before anything is on screen, so a throw here would cost the editor its theme AND its panels.
// (2) `startupThemeId` implements the C-F22 order exactly: pin > persisted > `prefers-color-scheme` >
// Dark.
//
// ⚠ THE FIRST-RUN RULE IS ASSERTED UNDER BOTH SCHEMES, ALWAYS. A probe is injected into every case
// rather than read from the host, because this tier runs in a real browser and a host that prefers
// dark would make a light-scheme assertion pass for the wrong reason — the exact ambient-state
// false-positive that made e06b's local number say 99.67% while CI stayed red. A test whose expected
// value depends on the machine it runs on is not a test.

import { assert, assertEqual, type TestCase } from "./harness.js";
import {
    CONFIG_GET_METHOD,
    CONFIG_SET_METHOD,
    CONFIG_THEME_KEY,
    EMPTY_CONFIG_SNAPSHOT,
    configuredThemeId,
    parseConfigSnapshot,
    startupThemeId,
    type UserConfigSnapshot,
} from "../config.js";
import { BUILTIN_DARK, BUILTIN_LIGHT, type MediaQueryLike } from "../theme.js";

/** A probe that answers ONE scheme, whatever the host prefers. `null` = the query is undetectable. */
function probeFor(scheme: "light" | "dark" | "undetectable"): (query: string) => MediaQueryLike | null {
    return (query: string): MediaQueryLike | null => {
        if (scheme === "undetectable") {
            return null;
        }
        return { matches: query.includes("prefers-color-scheme: light") && scheme === "light" };
    };
}

const KNOWN = new Set([BUILTIN_DARK, BUILTIN_LIGHT, "user.mine"]);
const isKnown = (id: string): boolean => KNOWN.has(id);

function snapshotWith(config: Record<string, unknown>): UserConfigSnapshot {
    return { ...EMPTY_CONFIG_SNAPSHOT, writable: true, config };
}

export const configTests: readonly TestCase[] = [
    {
        name: "config: the wire vocabulary is the Shell's",
        run: (): void => {
            // Pinned here as well as in the C++ header because `webui-panel-contract` compares these
            // two spellings out of the BUILT bundle — a rename that missed one side would otherwise
            // leave the theme applying and never persisting, with nothing reporting it.
            assertEqual(CONFIG_GET_METHOD, "config.get", "the read method");
            assertEqual(CONFIG_SET_METHOD, "config.set", "the write-request method");
            assertEqual(CONFIG_THEME_KEY, "theme", "the one settable key");
        },
    },
    {
        name: "config: parseConfigSnapshot is total over hostile input",
        run: (): void => {
            for (const hostile of [undefined, null, 7, "nope", [], true]) {
                assertEqual(
                    parseConfigSnapshot(hostile),
                    EMPTY_CONFIG_SNAPSHOT,
                    `a ${typeof hostile} envelope degrades to the empty snapshot`,
                );
            }
            // A partial envelope keeps what it CAN read and defaults the rest — it never throws and
            // never invents a value.
            const partial = parseConfigSnapshot({ generation: 4, config: { theme: "user.mine" } });
            assertEqual(partial.generation, 4, "the generation survives");
            assert(!partial.writable, "an absent writable reads as NOT writable (fail-closed)");
            assertEqual(partial.path, "", "an absent path reads as empty");
            assertEqual(configuredThemeId(partial), "user.mine", "the theme survives");
        },
    },
    {
        name: "config: a non-string theme member records NO preference",
        run: (): void => {
            // Coercing `3` to `"3"` would invent a choice the user never made, and then fail to
            // resolve it — a worse outcome than honestly having none.
            assertEqual(configuredThemeId(snapshotWith({ theme: 3 })), "", "a number is not a theme");
            assertEqual(configuredThemeId(snapshotWith({})), "", "an absent member is not a theme");
            assertEqual(
                configuredThemeId(snapshotWith({ theme: "builtin.dark" })),
                "builtin.dark",
                "a string member is the recorded theme",
            );
        },
    },
    {
        name: "config: first run follows prefers-color-scheme, under BOTH schemes",
        run: (): void => {
            assertEqual(
                startupThemeId("", "", probeFor("light"), isKnown),
                BUILTIN_LIGHT,
                "a light host boots Light when nothing is persisted",
            );
            assertEqual(
                startupThemeId("", "", probeFor("dark"), isKnown),
                BUILTIN_DARK,
                "a dark host boots Dark when nothing is persisted",
            );
            assertEqual(
                startupThemeId("", "", probeFor("undetectable"), isKnown),
                BUILTIN_DARK,
                "an UNDETECTABLE preference boots Dark (06 §4) — the CI-runner case",
            );
        },
    },
    {
        name: "config: a persisted choice beats the host preference (both directions)",
        run: (): void => {
            // The point of persistence: the machine's own preference stops deciding once the user has.
            assertEqual(
                startupThemeId(BUILTIN_DARK, "", probeFor("light"), isKnown),
                BUILTIN_DARK,
                "Dark persisted on a light host stays Dark",
            );
            assertEqual(
                startupThemeId(BUILTIN_LIGHT, "", probeFor("dark"), isKnown),
                BUILTIN_LIGHT,
                "Light persisted on a dark host stays Light",
            );
            assertEqual(
                startupThemeId("user.mine", "", probeFor("dark"), isKnown),
                "user.mine",
                "a persisted USER theme is honoured like a built-in",
            );
        },
    },
    {
        name: "config: an unresolvable persisted theme falls back rather than unstyling the window",
        run: (): void => {
            // The deleted-theme-file case. `ThemeEngine.apply` refuses an id the registry does not
            // hold, and at boot there is no previous theme for that refusal to fall back onto — so
            // honouring it blindly would leave the editor with no tokens at all.
            assertEqual(
                startupThemeId("user.deleted", "", probeFor("light"), isKnown),
                BUILTIN_LIGHT,
                "an unknown persisted id falls back to the host preference",
            );
            assertEqual(
                startupThemeId("user.deleted", "", probeFor("undetectable"), isKnown),
                BUILTIN_DARK,
                "...and to Dark when there is no host preference either",
            );
        },
    },
    {
        name: "config: the smoke pin outranks the persisted choice",
        run: (): void => {
            // A live pixel test must be able to CHOOSE the appearance it asserts on; a config left
            // behind on a CI host must not be able to override that (theme.ts THEME_PIN_FLAG).
            assertEqual(
                startupThemeId(BUILTIN_DARK, "?ctx-smoke-theme=builtin.light", probeFor("dark"), isKnown),
                BUILTIN_LIGHT,
                "the pin wins over the config",
            );
            assertEqual(
                startupThemeId(BUILTIN_DARK, "?ctx-smoke-theme=user.deleted", probeFor("light"), isKnown),
                BUILTIN_DARK,
                "an UNKNOWN pin does not win — the persisted choice still applies",
            );
        },
    },
];
