// @context-engine/editor-tokens — the token DATA layer of themes-as-data (M9 e06a, design 06 / D11).
// The public surface consumed by the e06b theme engine, the e06c component kit, and the e07a T1 tier:
// the versioned schema + validator, and the schema/theme-envelope identifiers. The built-in theme DATA
// lives in `../themes/*.theme.json` and the vendored fonts in `../fonts/` (see README.md); this entry
// re-exports only the CODE surface.

export {
    SCHEMA_ID,
    SCHEMA_VERSION,
    THEME_SCHEMA,
    toJsonSchema,
    validateTheme,
} from "./schema.js";
export type {
    FieldSpec,
    JsonSchemaNode,
    ObjectFields,
    ObjectSpec,
    ValidationResult,
} from "./schema.js";
