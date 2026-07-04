# `src/editor/compose/` — scene composition, the READ path (M2)

The M2 centerpiece read path (**L-35** / **R-DATA-002**): a scene instances other scenes with
per-instance, per-field override entries; composition **flattens in the derivation graph at zero
runtime cost** — the runtime consumes flattened output and never resolves an override. There is no
separate prefab concept: one uniform mechanism.

Library target: **`context_compose`** (links `context_serializer` for the JSON tree + canonical
emitters and `context_schema` for the scene kind id + read helpers). Namespace:
`context::editor::compose`. The derivation graph (`src/editor/derivation/`) wires this module as
its compose node; see that README for the graph-side semantics.

## The authored model (the frozen M2 schema members, `ctx:scene` — L-33/L-35)

- **Stable intra-file ids (L-33)** — every entity/instance carries `"id"`: collision-resistant
  random lowercase hex, ≥ 64-bit (16..32 chars), **file-scoped, never sequential**
  (`stable_id.h`). Child collections are **id-keyed arrays-of-objects-with-`id`** (the R-FILE-001
  serializer form; map-keyed encoding is forbidden). Duplicate ids in one file are a **blocking**
  `compose.duplicate_id`; id-less/invalid entries are excluded with an advisory
  (`compose.missing_id` / `compose.invalid_id`) — minting ids for old content is the schema-
  migration task's job.
- **`instances`** — `{"id", "scene"}`: compose the scene at project-root-relative path `scene`
  under the stable id `id`. Instance paths match derivation node keys **verbatim** (no
  relative-to-referrer resolution in v1); the path becomes a typed `x-ctx-ref` when the asset
  database lands (L-34/L-36).
- **`overrides`** — id-path addressed entries (`"path": [instanceId, …, entityId]`, from an
  instance id of the owning scene inward), one kind each:
  - `"pointer"` + `"value"` — a per-field override inside the addressed entity;
  - `"add": {entity}` — structural add under the addressed **instance subtree**;
  - `"remove": true` — structural remove of the addressed entity (or whole instance subtree).
  `/id`, `/$schema`, `/version` are immutable under composition (L-37: composed identity survives
  re-derivation and upgrade).
- **`root`** — the scene-root entity (L-35): scene-level state = singleton components on it.
  **Inert by default** when the scene is instanced as a sub-scene; `"composable": true` opts it
  into the parent flatten. Addressable in override paths by its explicit id, or as **`$root`**
  when none is authored. Bakes are derived artifacts, never authored (the schema defines no baked
  member; `additionalProperties: false` rejects one).

## Precedence and provenance (R-CLI-006 read side)

Overrides resolve **innermost-out**: contributors sort by instancing level and the **outermost
instancing scene (level 0, the flatten root) wins**. Every composed value carries a
**winning-value-first provenance chain** — `[{source: schemaDefault|template|override, file,
pointer, level}]` — so an agent sees every contributor: which template supplied the value and
which instancing level overrode it (`provenance_for`, `provenance_json`). `schemaDefault` is
emitter-ready but not yet produced — the schema dialect declares no defaults today.

**Composed identity (L-37)** is the deterministic id-path `[rootScene, instanceId, …, entityId]`
hashed as `identity_hash_of()` — stable across re-derivation, upgrade, and restarts; the ONE
identity later shared by saves, net ids, and query results.

## Diagnostics (advisory findings never block the flatten; blocking ones flip `ok`)

| code | blocking | meaning |
| --- | --- | --- |
| `compose.missing_scene` | yes | an instanced path resolves to no known scene |
| `compose.cycle` | yes | the instanced scene is already on the expansion chain |
| `compose.depth_exceeded` | yes | nesting deeper than `ComposeLimits::max_depth` (R-FILE-011(e)) |
| `compose.duplicate_id` | yes | a stable id claimed twice in one file's id space |
| `compose.too_many_entities` | yes | flatten output past `max_entities` — expansion stopped |
| `compose.fan_in` | no | one file instanced ≥ `fan_in_threshold` times in one flatten |
| `compose.orphan_override` | no | an override path resolves to nothing — excluded from flatten (L-37) |
| `compose.missing_id` / `compose.invalid_id` | no | entry without a valid stable id — excluded |
| `compose.override_malformed` | no | entry shape violation — excluded |

These are R-FILE-003-shaped **report** findings (like the schema validator's), surfaced through
the derivation graph's `composed()` view — not R-CLI-008 envelope error codes; the envelope layer
maps a failed composed read onto the existing catalog when the query surface consumes it.

## Deliberate M2-read-path boundaries (documented, not accidental)

- **The composed WRITE path is a separate task** (`context set` default-outermost,
  `--edit-template` / `--at-instance` — R-CLI-006): this module only reads. `json_pointer.h` is
  written for reuse by that task.
- **Fan-out is synchronous inside the derivation pass** (the graph re-flattens dependents in the
  pass that derived the template) — the async-streamed fan-out of R-FILE-011(b) lands with the
  daemon's threading model, like the rest of the graph's determinism story.
- **Flatten boundaries = future content-unit boundaries**: the flatten output is the layer the
  R-ASSET-005 chunked pack format co-designs against (ROADMAP M2); the pack format itself is a
  later M2 task.
- **Component-level validation of composed output** (validating an overridden component against
  its kind schema post-compose) is the instantiate node's concern, not the flatten's.

## Tests

`ctest --preset dev` runs each `compose-*` executable (see `CMakeLists.txt`): stable-id form +
minting, JSON-pointer resolution/application, the scene model's id-space and override-shape rules,
flatten happy paths (precedence, chains, identity, roots, structural ops), flatten failure paths
(cycles, depth, fan-in, orphans, budget), and **`compose-test_corpus`** — the committed
R-QA-011 composition edge-case corpus under `tests/corpus/` (deep nesting, dense fan-in, cycles,
orphan overrides; each case one JSON file: a scene set + expected id-paths/diagnostics).
