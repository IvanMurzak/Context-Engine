// The versioned token schema for @context-engine/editor-tokens *.theme.json files (M9 e06a, design
// 06 § 1 "Token schema — themes are data, D11"). Themes are DATA: a `*.theme.json` file carries a
// `$schema` id, a `version`, and the seven token groups (colors / typography / shape / elevation /
// motion / iconography / viewport). Components reference ONLY semantic tokens; raw values live in
// themes alone (06 § 1).
//
// This module is the SINGLE SOURCE OF TRUTH for both halves of the contract:
//   * `validateTheme(value)` — the runtime validator the T1 tier + the e06b theme engine call. It is
//     a hand-written, dependency-free validator (the webui workspace acquires every third-party input
//     through a SHA-pinned fail-closed channel and the license allowlist is dockview-core ONLY — a
//     JSON-Schema library would be a new npm dependency, forbidden). UNKNOWN KEYS ARE REJECTED
//     LOUDLY at every level (T1) — `additionalProperties:false` in schema terms.
//   * `toJsonSchema()` — emits the published `../theme.schema.json` (standard JSON Schema 2020-12).
//     `scripts/gen-schema.mjs` regenerates the file from this function, and the T1 drift test asserts
//     the committed file is byte-identical, so the two representations can never diverge.

/** The token-schema version every `*.theme.json` declares and this validator enforces. */
export const SCHEMA_VERSION = 1;

/** The `$schema` id every built-in and user theme carries; bumped in lockstep with a breaking change. */
export const SCHEMA_ID = "https://context-engine.dev/schemas/theme/v1.json";

/** A leaf or nested field in the theme schema — the mini-schema `validateTheme` interprets. */
export type FieldSpec =
    | { readonly kind: "string" }
    | { readonly kind: "color" }
    | { readonly kind: "number" }
    | { readonly kind: "integer" }
    | { readonly kind: "boolean" }
    | { readonly kind: "constNumber"; readonly value: number }
    | { readonly kind: "constString"; readonly value: string }
    | { readonly kind: "enum"; readonly values: readonly string[] }
    | ObjectSpec;

/** A nested object field. `optional` lists keys that MAY be absent; everything else is required. */
export interface ObjectSpec {
    readonly kind: "object";
    readonly fields: ObjectFields;
    readonly optional?: readonly string[];
}

/** The field map of an {@link ObjectSpec}. Key order is significant — it fixes the schema/theme order. */
export interface ObjectFields {
    readonly [key: string]: FieldSpec;
}

/** The verdict of {@link validateTheme}: `valid` is true only when `errors` is empty. */
export interface ValidationResult {
    readonly valid: boolean;
    readonly errors: readonly string[];
}

// --- color grammar --------------------------------------------------------------------------------
// Themes carry CSS colors as strings. The legal forms are hex (#rgb / #rrggbb / #rrggbbaa) and the
// functional rgb()/rgba() notation — the two forms the ported d1 mockup uses (mockups/TOKENS.md).
const HEX_COLOR = /^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/;
const RGB_COLOR = /^rgba?\(\s*[\d.]+\s*,\s*[\d.]+\s*,\s*[\d.]+\s*(?:,\s*[\d.]+\s*)?\)$/;

function isColor(value: unknown): boolean {
    return typeof value === "string" && (HEX_COLOR.test(value) || RGB_COLOR.test(value));
}

function childPath(path: string, key: string): string {
    return path === "" ? key : `${path}.${key}`;
}

function isPlainObject(value: unknown): value is Record<string, unknown> {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}

function validateField(spec: FieldSpec, value: unknown, path: string, errors: string[]): void {
    switch (spec.kind) {
        case "string":
            if (typeof value !== "string") {
                errors.push(`${path}: expected a string`);
            }
            break;
        case "color":
            if (!isColor(value)) {
                errors.push(`${path}: expected a color (hex #rgb/#rrggbb/#rrggbbaa or rgb()/rgba())`);
            }
            break;
        case "number":
            if (typeof value !== "number" || !Number.isFinite(value)) {
                errors.push(`${path}: expected a finite number`);
            }
            break;
        case "integer":
            if (typeof value !== "number" || !Number.isInteger(value)) {
                errors.push(`${path}: expected an integer`);
            }
            break;
        case "boolean":
            if (typeof value !== "boolean") {
                errors.push(`${path}: expected a boolean`);
            }
            break;
        case "constNumber":
            if (value !== spec.value) {
                errors.push(`${path}: expected the constant ${spec.value}`);
            }
            break;
        case "constString":
            if (value !== spec.value) {
                errors.push(`${path}: expected the constant "${spec.value}"`);
            }
            break;
        case "enum":
            if (typeof value !== "string" || !spec.values.includes(value)) {
                errors.push(`${path}: expected one of ${spec.values.join(" | ")}`);
            }
            break;
        case "object":
            validateObject(spec, value, path, errors);
            break;
    }
}

function validateObject(spec: ObjectSpec, value: unknown, path: string, errors: string[]): void {
    if (!isPlainObject(value)) {
        errors.push(`${path === "" ? "(root)" : path}: expected an object`);
        return;
    }
    const optional = new Set(spec.optional ?? []);
    // Unknown keys are rejected LOUDLY (T1) — the additionalProperties:false half of the contract.
    for (const key of Object.keys(value)) {
        if (!Object.prototype.hasOwnProperty.call(spec.fields, key)) {
            errors.push(`${childPath(path, key)}: unknown key (unknown keys are rejected)`);
        }
    }
    for (const key of Object.keys(spec.fields)) {
        const fieldSpec = spec.fields[key];
        if (fieldSpec === undefined) {
            continue; // unreachable (key came from Object.keys) — satisfies noUncheckedIndexedAccess
        }
        if (!Object.prototype.hasOwnProperty.call(value, key)) {
            if (!optional.has(key)) {
                errors.push(`${childPath(path, key)}: required key is missing`);
            }
            continue;
        }
        validateField(fieldSpec, value[key], childPath(path, key), errors);
    }
}

// --- the schema (source of truth) -----------------------------------------------------------------
// The `motion.flourish` block is the "Pulse of Work" signature flourish (owner pick O1, 2026-07-19;
// mockups/TOKENS.md § 5): a state-linked glow whose hue + rhythm mirror the Play button's real state,
// reusing the reserved status hues (ZERO new colour tokens). Each of the five states names one of the
// reserved semantic hues + a rhythm (breathe/pulse/none) + a duration.
const FLOURISH_STATE: ObjectSpec = {
    kind: "object",
    fields: {
        hue: { kind: "enum", values: ["good", "warn", "bad", "wait", "idle"] },
        opacity: { kind: "number" },
        rhythm: { kind: "enum", values: ["breathe", "pulse", "none"] },
        duration: { kind: "string" },
    },
};

const COLORS: ObjectSpec = {
    kind: "object",
    fields: {
        canvas: { kind: "color" },
        panel: { kind: "color" },
        panel2: { kind: "color" },
        ink: { kind: "color" },
        muted: { kind: "color" },
        muted2: { kind: "color" },
        accent: { kind: "color" },
        accent2: { kind: "color" },
        line: { kind: "color" },
        chip: { kind: "color" },
        // The status hues — the only chroma outside the viewport exception, bound 1:1 to reserved
        // semantics (06 § 2): good=success, warn=active-work, bad=errors, wait=awaiting-human, idle.
        semantic: {
            kind: "object",
            fields: {
                good: { kind: "color" },
                warn: { kind: "color" },
                bad: { kind: "color" },
                wait: { kind: "color" },
                idle: { kind: "color" },
            },
        },
        focusRing: { kind: "color" },
    },
};

const TYPOGRAPHY: ObjectSpec = {
    kind: "object",
    fields: {
        fontUi: { kind: "string" },
        fontMono: { kind: "string" },
        tracking: { kind: "string" },
        lineHeight: { kind: "number" },
        numericVariant: { kind: "string" },
        size: {
            kind: "object",
            fields: {
                "2xs": { kind: "string" },
                xs: { kind: "string" },
                sm: { kind: "string" },
                base: { kind: "string" },
                md: { kind: "string" },
                lg: { kind: "string" },
                xl: { kind: "string" },
            },
        },
        weight: {
            kind: "object",
            fields: {
                regular: { kind: "integer" },
                medium: { kind: "integer" },
                semibold: { kind: "integer" },
            },
        },
    },
};

const SHAPE: ObjectSpec = {
    kind: "object",
    fields: {
        radius: {
            kind: "object",
            fields: {
                sm: { kind: "string" },
                md: { kind: "string" },
                pill: { kind: "string" },
            },
        },
        borderWidth: { kind: "string" },
        focusRingWidth: { kind: "string" },
        selectionBarWidth: { kind: "string" },
        density: {
            kind: "object",
            fields: {
                compact: { kind: "string" },
                default: { kind: "string" },
                comfortable: { kind: "string" },
            },
        },
    },
};

const ELEVATION: ObjectSpec = {
    kind: "object",
    // "border-step model vs shadows" (06 § 1): the monochrome built-ins use borders; a theme MAY
    // define shadows, so the shadow map is optional.
    fields: {
        model: { kind: "enum", values: ["border-step", "shadow"] },
        shadows: {
            kind: "object",
            fields: {
                sm: { kind: "string" },
                md: { kind: "string" },
                lg: { kind: "string" },
            },
        },
    },
    optional: ["shadows"],
};

const MOTION: ObjectSpec = {
    kind: "object",
    fields: {
        duration: {
            kind: "object",
            fields: {
                fast: { kind: "string" },
                base: { kind: "string" },
                slow: { kind: "string" },
                theme: { kind: "string" },
            },
        },
        easing: {
            kind: "object",
            fields: {
                standard: { kind: "string" },
                spring: { kind: "string" },
            },
        },
        enableSpringPills: { kind: "boolean" },
        // The Pulse-of-Work signature flourish (06 § 2 amended 2026-07-19; mockups/TOKENS.md § 5).
        flourish: {
            kind: "object",
            fields: {
                kind: { kind: "enum", values: ["state-linked"] },
                enabled: { kind: "boolean" },
                bloomInset: { kind: "string" },
                blur: { kind: "string" },
                states: {
                    kind: "object",
                    fields: {
                        idle: FLOURISH_STATE,
                        running: FLOURISH_STATE,
                        compiling: FLOURISH_STATE,
                        error: FLOURISH_STATE,
                        paused: FLOURISH_STATE,
                    },
                },
            },
        },
    },
};

const ICONOGRAPHY: ObjectSpec = {
    kind: "object",
    fields: {
        set: { kind: "string" },
        stroke: { kind: "string" },
        box: { kind: "string" },
    },
};

// The legal chroma exception (D12) — axes, grid, selection, gizmos, camera widget (06 § 1, TOKENS.md
// § 7). gizmoHover is a runtime derivation rule ("brighten the hovered handle's own axis colour"),
// not a fixed token, so it is deliberately NOT a theme field.
const VIEWPORT: ObjectSpec = {
    kind: "object",
    fields: {
        axisX: { kind: "color" },
        axisY: { kind: "color" },
        axisZ: { kind: "color" },
        gridMajor: { kind: "color" },
        gridMinor: { kind: "color" },
        selectionOutline: { kind: "color" },
        gizmoActive: { kind: "color" },
        cameraWidget: { kind: "color" },
    },
};

/** The root theme schema — the source of truth for {@link validateTheme} and {@link toJsonSchema}. */
export const THEME_SCHEMA: ObjectSpec = {
    kind: "object",
    fields: {
        $schema: { kind: "constString", value: SCHEMA_ID },
        version: { kind: "constNumber", value: SCHEMA_VERSION },
        name: { kind: "string" },
        appearance: { kind: "enum", values: ["dark", "light"] },
        highContrast: { kind: "boolean" },
        colors: COLORS,
        typography: TYPOGRAPHY,
        shape: SHAPE,
        elevation: ELEVATION,
        motion: MOTION,
        iconography: ICONOGRAPHY,
        viewport: VIEWPORT,
    },
};

/**
 * Validate an unknown value as a `*.theme.json` document against {@link THEME_SCHEMA}. TOTAL and
 * fail-closed: never throws, collects EVERY error (not just the first), rejects unknown keys at every
 * level, and enforces the `$schema`/`version` envelope. `valid === true` iff `errors` is empty.
 */
export function validateTheme(value: unknown): ValidationResult {
    const errors: string[] = [];
    validateField(THEME_SCHEMA, value, "", errors);
    return { valid: errors.length === 0, errors };
}

// --- published JSON Schema emission ---------------------------------------------------------------
/** A standard JSON Schema node — the shape `toJsonSchema` emits and `theme.schema.json` stores. */
export interface JsonSchemaNode {
    readonly [key: string]: unknown;
}

function fieldToJsonSchema(spec: FieldSpec): JsonSchemaNode {
    switch (spec.kind) {
        case "string":
            return { type: "string" };
        case "color":
            return {
                type: "string",
                description: "CSS color: hex (#rgb/#rrggbb/#rrggbbaa) or rgb()/rgba()",
            };
        case "number":
            return { type: "number" };
        case "integer":
            return { type: "integer" };
        case "boolean":
            return { type: "boolean" };
        case "constNumber":
            return { const: spec.value };
        case "constString":
            return { const: spec.value };
        case "enum":
            return { type: "string", enum: [...spec.values] };
        case "object":
            return objectToJsonSchema(spec);
    }
}

function objectToJsonSchema(spec: ObjectSpec): JsonSchemaNode {
    const optional = new Set(spec.optional ?? []);
    const properties: Record<string, JsonSchemaNode> = {};
    const required: string[] = [];
    for (const key of Object.keys(spec.fields)) {
        const fieldSpec = spec.fields[key];
        if (fieldSpec === undefined) {
            continue;
        }
        properties[key] = fieldToJsonSchema(fieldSpec);
        if (!optional.has(key)) {
            required.push(key);
        }
    }
    return { type: "object", properties, required, additionalProperties: false };
}

/**
 * Emit the published JSON Schema (draft 2020-12) for `*.theme.json`, derived from {@link THEME_SCHEMA}.
 * `scripts/gen-schema.mjs` writes `../theme.schema.json` from this; the T1 drift test asserts the
 * committed file matches, so the runtime validator and the published schema can never drift.
 */
export function toJsonSchema(): JsonSchemaNode {
    const root = objectToJsonSchema(THEME_SCHEMA);
    return {
        $schema: "https://json-schema.org/draft/2020-12/schema",
        $id: SCHEMA_ID,
        title: "Context Engine editor theme",
        description:
            "Versioned token schema for @context-engine/editor-tokens *.theme.json themes (design 06 § 1). Unknown keys are rejected (additionalProperties:false at every level).",
        ...root,
    };
}
