// T1 unit tests for @context-engine/editor-tokens (M9 e06a, design 06 / D11 themes-as-data). This is
// the DoD gate "versioned token schema validates all built-ins; malformed / unknown-key themes
// rejected loudly (T1)". It runs in the SAME headless-Chromium `webui-ts-unit` tier as the rest of
// editor-core (e07a) — the tokens package's validator is pure TS with no DOM/dependency, so it needs
// no separate harness. The four built-in `*.theme.json` files are imported DIRECTLY (esbuild inlines
// them via the json loader; tsgo resolves them via resolveJsonModule), so the tests validate the
// EXACT shipped bytes — there is no in-test copy that could drift from what e06b/e06c consume.

import { assert, assertEqual, type TestCase } from "./harness.js";
import {
    SCHEMA_ID,
    SCHEMA_VERSION,
    THEME_SCHEMA,
    toJsonSchema,
    validateTheme,
} from "../../../tokens/src/schema.js";

import darkTheme from "../../../tokens/themes/dark.theme.json";
import lightTheme from "../../../tokens/themes/light.theme.json";
import hcDarkTheme from "../../../tokens/themes/high-contrast-dark.theme.json";
import hcLightTheme from "../../../tokens/themes/high-contrast-light.theme.json";
import publishedSchema from "../../../tokens/theme.schema.json";

interface BuiltIn {
    readonly file: string;
    readonly theme: unknown;
    readonly appearance: "dark" | "light";
    readonly highContrast: boolean;
}

const BUILTINS: readonly BuiltIn[] = [
    { file: "dark", theme: darkTheme, appearance: "dark", highContrast: false },
    { file: "light", theme: lightTheme, appearance: "light", highContrast: false },
    { file: "high-contrast-dark", theme: hcDarkTheme, appearance: "dark", highContrast: true },
    { file: "high-contrast-light", theme: hcLightTheme, appearance: "light", highContrast: true },
];

const SEMANTIC_KEYS = ["good", "warn", "bad", "wait", "idle"] as const;

// The Pulse-of-Work state → (hue, rhythm, duration) map — the e06 handoff table (mockups/TOKENS.md
// § 5): animation speed tracks activity level; colour is always the reserved status hue.
const FLOURISH_EXPECTED = {
    idle: { hue: "idle", rhythm: "breathe", duration: "7s" },
    running: { hue: "good", rhythm: "breathe", duration: "2.6s" },
    compiling: { hue: "warn", rhythm: "pulse", duration: "0.95s" },
    error: { hue: "bad", rhythm: "pulse", duration: "1.4s" },
    paused: { hue: "idle", rhythm: "none", duration: "0s" },
} as const;

/** Read a nested member from a validated theme (safe: only called after validateTheme passed). */
function group(theme: unknown, ...path: readonly string[]): Record<string, unknown> {
    let node: unknown = theme;
    for (const key of path) {
        assert(
            typeof node === "object" && node !== null && !Array.isArray(node),
            `expected object at ${path.join(".")}`,
        );
        node = (node as Record<string, unknown>)[key];
    }
    assert(
        typeof node === "object" && node !== null && !Array.isArray(node),
        `expected object at ${path.join(".")}`,
    );
    return node as Record<string, unknown>;
}

function stripAlpha(color: string): boolean {
    // A "solid" ring is a bare hex with no alpha channel: #rgb / #rrggbb, never rgba() or #rrggbbaa.
    return /^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6})$/.test(color);
}

export const tokensTests: readonly TestCase[] = [
    // ------------------------------------------------------- the schema validates every built-in
    {
        name: "validateTheme: all four built-in themes validate cleanly",
        run: () => {
            for (const { file, theme } of BUILTINS) {
                const result = validateTheme(theme);
                assertEqual(result.errors, [], `${file}.theme.json validates with no errors`);
                assertEqual(result.valid, true, `${file}.theme.json is valid`);
            }
        },
    },
    {
        name: "every built-in carries the versioned $schema / version envelope",
        run: () => {
            for (const { file, theme, appearance, highContrast } of BUILTINS) {
                const root = group(theme);
                assertEqual(root["$schema"], SCHEMA_ID, `${file}: $schema id`);
                assertEqual(root["version"], SCHEMA_VERSION, `${file}: schema version`);
                assertEqual(root["appearance"], appearance, `${file}: appearance`);
                assertEqual(root["highContrast"], highContrast, `${file}: highContrast flag`);
                assert(typeof root["name"] === "string" && root["name"].length > 0, `${file}: name`);
            }
        },
    },

    // ------------------------------------------------------- status hues bound 1:1, distinct chroma
    {
        name: "status hues: all five reserved semantics present and mutually distinct per theme",
        run: () => {
            for (const { file, theme } of BUILTINS) {
                const semantic = group(theme, "colors", "semantic");
                const seen = new Set<string>();
                for (const key of SEMANTIC_KEYS) {
                    const hue = semantic[key];
                    assert(typeof hue === "string" && hue.length > 0, `${file}: semantic.${key} present`);
                    seen.add(hue as string);
                }
                assertEqual(seen.size, SEMANTIC_KEYS.length, `${file}: the five status hues are distinct`);
            }
        },
    },

    // ------------------------------------------------------- Pulse of Work (mockups/TOKENS.md § 5)
    {
        name: "Pulse-of-Work: state-linked flourish with the exact state→hue→rhythm→duration map",
        run: () => {
            for (const { file, theme } of BUILTINS) {
                const flourish = group(theme, "motion", "flourish");
                assertEqual(flourish["kind"], "state-linked", `${file}: flourish is the state-linked pick`);
                // Bloom is a ::before radial gradient, inset -5px, blur 4px, with NO box-shadow (§ 5 / § 4).
                assertEqual(flourish["bloomInset"], "-5px", `${file}: bloom inset (halved per owner)`);
                assertEqual(flourish["blur"], "4px", `${file}: blur`);
                const states = group(theme, "motion", "flourish", "states");
                for (const [name, expected] of Object.entries(FLOURISH_EXPECTED)) {
                    const state = group(theme, "motion", "flourish", "states", name);
                    assertEqual(state["hue"], expected.hue, `${file}: ${name} hue = reserved ${expected.hue}`);
                    assertEqual(state["rhythm"], expected.rhythm, `${file}: ${name} rhythm`);
                    assertEqual(state["duration"], expected.duration, `${file}: ${name} duration`);
                }
                assertEqual(Object.keys(states).length, 5, `${file}: exactly the five play states`);
            }
        },
    },
    {
        name: "Pulse-of-Work introduces ZERO new colour tokens (every state hue is a reserved semantic)",
        run: () => {
            const reserved = new Set<string>(SEMANTIC_KEYS);
            for (const { file, theme } of BUILTINS) {
                const states = group(theme, "motion", "flourish", "states");
                for (const name of Object.keys(states)) {
                    const state = group(theme, "motion", "flourish", "states", name);
                    assert(
                        reserved.has(state["hue"] as string),
                        `${file}: ${name} hue "${String(state["hue"])}" is a reserved status hue, not a new token`,
                    );
                }
            }
        },
    },

    // ------------------------------------------------------- high-contrast pair (06 § 2, R-A11Y-001)
    {
        name: "high-contrast pair: pure ink, fully-opaque 2px focus ring, flagged highContrast",
        run: () => {
            const hc = BUILTINS.filter((b) => b.highContrast);
            assertEqual(hc.length, 2, "a high-contrast pair exists (dark + light)");
            for (const { file, theme, appearance } of hc) {
                const colors = group(theme, "colors");
                const shape = group(theme, "shape");
                const expectedInk = appearance === "dark" ? "#ffffff" : "#000000";
                assertEqual(colors["ink"], expectedInk, `${file}: ink is pure ${expectedInk} for max contrast`);
                assert(
                    stripAlpha(colors["focusRing"] as string),
                    `${file}: focus ring is a SOLID colour (no alpha), per the HC 2px-solid-ink spec`,
                );
                assertEqual(shape["focusRingWidth"], "2px", `${file}: 2px focus ring`);
            }
        },
    },
    {
        name: "non-high-contrast themes keep the soft alpha focus ring (the HC assertion is non-vacuous)",
        run: () => {
            for (const { file, theme } of BUILTINS.filter((b) => !b.highContrast)) {
                const colors = group(theme, "colors");
                assert(
                    !stripAlpha(colors["focusRing"] as string),
                    `${file}: standard themes use a translucent focus ring (rgba)`,
                );
            }
        },
    },

    // ------------------------------------------------------- unknown-key + malformed rejection (T1)
    {
        name: "validateTheme: an unknown key is rejected loudly at the top level",
        run: () => {
            const bad = { ...(darkTheme as Record<string, unknown>), surpriseKey: 1 };
            const result = validateTheme(bad);
            assertEqual(result.valid, false, "unknown top-level key fails");
            assert(
                result.errors.some((e) => e.includes("surpriseKey") && e.includes("unknown key")),
                `the error names the offending key: ${result.errors.join("; ")}`,
            );
        },
    },
    {
        name: "validateTheme: an unknown key nested inside a group is rejected loudly",
        run: () => {
            const base = darkTheme as { colors: Record<string, unknown> };
            const bad = { ...base, colors: { ...base.colors, notAToken: "#fff" } };
            const result = validateTheme(bad);
            assertEqual(result.valid, false, "unknown nested key fails");
            assert(
                result.errors.some((e) => e.includes("colors.notAToken")),
                `the error names the nested path: ${result.errors.join("; ")}`,
            );
        },
    },
    {
        name: "validateTheme: the wrong schema version is rejected",
        run: () => {
            const bad = { ...(darkTheme as Record<string, unknown>), version: SCHEMA_VERSION + 1 };
            assertEqual(validateTheme(bad).valid, false, "a future version is not silently accepted");
        },
    },
    {
        name: "validateTheme: a missing required token is rejected and named",
        run: () => {
            const base = darkTheme as { colors: Record<string, unknown> };
            const withoutInk = { ...base.colors };
            delete withoutInk["ink"];
            const bad = { ...base, colors: withoutInk };
            const result = validateTheme(bad);
            assertEqual(result.valid, false, "a theme missing colors.ink fails");
            assert(
                result.errors.some((e) => e.includes("colors.ink") && e.includes("missing")),
                `the error names the missing key: ${result.errors.join("; ")}`,
            );
        },
    },
    {
        name: "validateTheme: a malformed colour and a bad enum are rejected",
        run: () => {
            const base = darkTheme as { colors: Record<string, unknown>; appearance: string };
            const badColour = { ...base, colors: { ...base.colors, ink: "not-a-colour" } };
            assertEqual(validateTheme(badColour).valid, false, "a non-colour ink value fails");
            const badEnum = { ...(darkTheme as Record<string, unknown>), appearance: "sepia" };
            assertEqual(validateTheme(badEnum).valid, false, "an out-of-vocabulary appearance fails");
        },
    },
    {
        name: "validateTheme: non-object inputs fail closed (total, never throws)",
        run: () => {
            assertEqual(validateTheme(null).valid, false, "null fails");
            assertEqual(validateTheme([]).valid, false, "an array fails");
            assertEqual(validateTheme("theme").valid, false, "a string fails");
            assertEqual(validateTheme(42).valid, false, "a number fails");
        },
    },
    {
        name: "validateTheme: collects EVERY error, not just the first",
        run: () => {
            const bad = {
                ...(darkTheme as Record<string, unknown>),
                version: 99,
                appearance: "sepia",
                mystery: true,
            };
            const result = validateTheme(bad);
            assert(result.errors.length >= 3, `at least three errors collected: ${result.errors.length}`);
        },
    },

    // ------------------------------------------------------- published JSON Schema drift gate
    {
        name: "toJsonSchema() is byte-identical to the committed theme.schema.json (no drift)",
        run: () => {
            assertEqual(
                toJsonSchema(),
                publishedSchema,
                "the published theme.schema.json matches the THEME_SCHEMA source of truth",
            );
        },
    },
    {
        name: "the published schema rejects unknown keys everywhere (additionalProperties:false)",
        run: () => {
            const schema = toJsonSchema() as { additionalProperties: unknown; properties: Record<string, unknown> };
            assertEqual(schema.additionalProperties, false, "root rejects unknown keys");
            const colours = schema.properties["colors"] as { additionalProperties: unknown };
            assertEqual(colours.additionalProperties, false, "the colors group rejects unknown keys");
        },
    },
    {
        name: "THEME_SCHEMA covers exactly the seven design-06 token groups plus the envelope",
        run: () => {
            const keys = Object.keys(THEME_SCHEMA.fields);
            for (const group2 of ["colors", "typography", "shape", "elevation", "motion", "iconography", "viewport"]) {
                assert(keys.includes(group2), `schema declares the ${group2} group`);
            }
            for (const envelope of ["$schema", "version", "name", "appearance", "highContrast"]) {
                assert(keys.includes(envelope), `schema declares the ${envelope} envelope field`);
            }
        },
    },
];
