# `src/editor/kinds/` — content-kind semantics (`context_kinds`)

The first content asset **kinds'** behaviour (M2 wave 4 — R-2D-003 tilemap + R-I18N-001
string-table, issue #61): the **content rules the small schema dialect cannot express**. The per-kind
JSON **schemas** live in [`src/editor/schema/`](../schema) (`engine_schemas()`, auto-registered by
the contract registry loop and enforced by the derivation validate node); this module is the layer
**on top** of the registered schemas.

The split is deliberate:

- **Shape** — required fields, types, `x-ctx-*` vocabulary, the `x-ctx-sidecar` reference shape — is
  validated by `schema::validate_document` against the published kind schema (R-DATA-006).
- **Content** — the rules a JSON-Schema-flavoured dialect can't state — lives here, over the parsed
  tree, emitting [`KindDiagnostic`](include/context/editor/kinds/diagnostic.h) findings (the
  R-FILE-003 shape: catalog code + JSON pointer + message).

## Tilemap (`ctx:tilemap`, R-2D-003)

[`tilemap.h`](include/context/editor/kinds/tilemap.h) — the 2D backbone. Heavy per-cell tile-id grids
live **out of the JSON** in binary sidecars (L-33, the `x-ctx-sidecar` day-one consumer), so
`analyze_tilemap` adds:

- **`tilemap.chunk_oversize`** — the L-33 ~1 MB **split-nudge** (advisory): a chunk whose packed cell
  payload exceeds `kTilemapSplitCeilingBytes`. Uses the sidecar's **measured** on-disk size when the
  caller supplies it, else estimates from `region area × 4 bytes/cell`.
- **`tilemap.region_invalid`** — a non-positive chunk region extent.
- **`tilemap.id_duplicate`** — a repeated stable `id` within the tile-sets collection, or within the
  layers collection (L-33 ids are unique per collection, not across the two).

## String-table (`ctx:string-table`, R-I18N-001)

[`string_table.h`](include/context/editor/kinds/string_table.h) — locale variants, fallback chains,
and ICU/CLDR plural rules; downstream systems bind string **keys** from day one.

- **`plural_category(locale, count)`** — CLDR **cardinal** category selection, implemented for a
  curated set spanning the rule families (English-like one/other; French 0-or-1 singular; East-Slavic
  `ru`/`uk`; Polish; Arabic's full six-way; East-Asian no-plural), with an English-like default for
  unrecognized locales. Integer-only in v1 (fractional CLDR operands are out of scope).
- **`resolve_fallback_chain` / `resolve_string`** — the ordered lookup (locale → declared fallback
  chain → implicit `sourceLocale`), cycle-safe, with plural selection and resolution **provenance**.
- **`validate_string_table`** — the semantic findings: `locale_duplicate`, `key_duplicate`,
  `fallback_unknown`, `fallback_cycle`, `value_invalid` (a value is exactly one of `text`/`plural`),
  `plural_incomplete` (a plural set omits the required `other`).

All the diagnostic codes are registered in the R-CLI-008 error catalog
(`src/editor/contract/src/error_catalog.cpp`), so they surface with proper exit classes through
`context describe`.

## Tests

`ctest` executables (`kinds-tilemap`, `kinds-string-table`) — R-QA-013 (behaviour ships with tests in
the same PR). They validate fixtures against the **real** `engine_schemas()` registration, exercise
every diagnostic class, evaluate the plural families, and prove the binary-sidecar round-trip through
the real filesync codec (`test_tilemap`).
