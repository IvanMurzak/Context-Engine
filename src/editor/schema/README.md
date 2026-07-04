# `src/editor/schema/` — the engine schema vocabulary + units law (M2 wave 2)

The shared vocabulary every per-kind JSON Schema uses (**R-DATA-006**, issue #47), pinned BEFORE
the first component schemas freeze — the ecosystem-fracture retrofit this requirement exists to
prevent: without one vocabulary, every package invents its own encoding for the same dozen
semantic types, and mixed units (degrees here, radians there) become the classic silent-corruption
bug an agent cannot see in JSON.

Library target: **`context_schema`** (links `context_serializer` for the JSON tree — deliberately
contract-free: the contract registry consumes THIS module, never the reverse). Namespace:
`context::editor::schema`.

## The vocabulary (all pinned — the law rejects deviations at schema COMPILE time)

- **`x-ctx-type`** — engine semantic types beyond JSON's primitives: `quaternion` (`[x,y,z,w]`),
  `color` (**with a declared color space** — `{"space", "value"}`), `curve` (`{"keys":
  [{"t","v"}…]}`, strictly increasing t), `gradient` (`{"stops": [{"t","color"}…]}`, t ∈ [0,1]
  non-decreasing), `bit-flags` (unique flag-name strings).
- **`x-ctx-storage`** — numeric width/layout the declarative component compiler derives storage
  from (feeds the M3 R-LANG-010 layout derivation): `<base>` or `<base>x<lanes>` (base ∈
  f32/f64/i8…i64/u8…u64, lanes ∈ 2/3/4/9/16). A lane-suffixed layout on an array also pins the
  authored element count.
- **`x-ctx-ref`** — a reference field's REQUIRED target kind. Values use the L-34 dual form
  (`{"$ref": "<guid>", "path": …}` / `{"$entity": "<id>"}`); target-kind enforcement runs through
  the `RefTargetResolver` meta-lookup seam once the asset database lands (the vocabulary + shape
  check + seam ship now, so a `$ref` to the wrong kind is a VALIDATE error, never a runtime
  surprise).
- **ONE tagged-union convention** — polymorphic values are `{"type": "<ns>:<shape>", …}`,
  declared via `x-ctx-union`; per-package ad-hoc encodings are rejected.
- **The units law** — SI + radians EVERYWHERE in authored data; `x-ctx-units` is introspection
  METADATA (surfaced through `context describe`, R-CLI-005/013), never a conversion switch. A
  schema declaring `"deg"` does not compile.
- **`notes`** — the schema-blessed human/AI annotation field (L-32 bans JSON comments): a string
  or array of strings, accepted on every object level of every authored kind; each kind's root
  must declare it.

## Layout

- `include/context/editor/schema/`
  - `vocabulary.h` — the x-ctx-* keys, semantic-type ids + per-type instance checkers, the SI
    unit / storage-layout / union-tag / color-space grammars, the blessed `notes` shape.
  - `kind_schema.h` — `compile_kind_schema` (the vocabulary-law gate over the pinned schema
    dialect), the versioned `SchemaSet` registration mechanism, `engine_schemas()` (`ctx:scene@1`,
    `ctx:project@1` — the M1 scene placeholder migrated onto the mechanism), and
    `introspection_json` (the canonical per-kind publication the contract registry projects into
    `describe` fileKinds — id + version + the derived per-field x-ctx index + the full schema).
  - `validator.h` — `validate_document` (header binding via `$schema`/`version`, blocking vs
    informational findings, every diagnostic carrying a JSON pointer + 1-based line/column into
    the SOURCE bytes via `locate_pointer`), and the `RefTargetResolver` seam.
  - `json_access.h` — small shared read helpers over the serializer tree.

## Where it plugs in

- **Derivation validate node** (`src/editor/derivation/`): `DerivationGraph` validates schema-bound
  JSON on ingest — on the SAME parse the canonical node produced (one parse per file,
  R-FILE-011) — and a FAILING payload retains its last-good derived state while its diagnostics
  surface through `DerivationGraph::validation()` with L-31 stability (R-FILE-003). The real
  EditorKernel composition wires `engine_schemas()`; unknown `$schema` ids stay informational so
  incrementally-registered kinds never block derivation.
- **Contract registry** (`src/editor/contract/`): `Registry` registers the engine kinds at
  construction through `register_file_kind` and `describe()` enumerates them LIVE in `fileKinds`
  (R-CLI-005/013) — one source of truth for what a kind's schema is.
- **`context new`** (`src/cli/`): both scaffolded templates carry the L-32 `$schema` header from
  their first byte and validate clean (the template's camera fov is radians, per the units law).

## Tests (R-QA-013)

`schema-test_vocabulary` (every semantic type's valid + invalid classes, the unit/storage/tag/
color-space grammars, notes shapes), `schema-test_kind_schema` (the compile-time law: non-SI
units, unknown semantics, malformed layouts/tags rejected; versioned registration + selection;
the introspection projection), `schema-test_validator` (header binding, every diagnostic class
with pointer + line/column, the resolver seam's wrong-kind rejection, union enforcement, notes
preservation through the canonical round-trip, `locate_pointer`), plus
`derivation-test_validate_node` (last-good retention, self-correction, stability, the M1
no-schema default) and the extended `contract-test_registry_parity` / `cli-test_scaffold`.
