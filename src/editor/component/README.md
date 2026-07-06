# `src/editor/component/` — declarative component-type authoring (R-LANG-010)

Schema-first component definitions: **one** declarative document (file/CLI-driven, no GUI) derives
everything a component type needs, at **load time**, with **no native engine rebuild** (L-60).

From one definition the compiler (`component_type.h`) derives:

- the **ECS storage layout** — field byte-offsets + total size + alignment, *interpreted* from the M2
  `x-ctx-storage` widths (`f32`, `i64`, … × pinned lane counts `2/3/4/9/16`);
- the **canonical-JSON payload encoding** — round-tripped through the M2 serializer
  (`component_registry.h`: `encode_payload` / `read_payload` / `serialize_payload`);
- the **published, versioned schema** `context describe` introspects
  (`component_type_introspection_json`, projected into the contract registry's `componentTypes`
  section — R-CLI-005);
- a stable **layout hash** (`layout_hash_of`) — a platform-independent digest of the ordered
  `(name, storage)` list; the compatibility key a WASM module's declared access is later checked
  against.

## Data-driven archetype storage

`ComponentTypeRegistry` (`component_registry.h`) registers a compiled type into the kernel `World`'s
**data-driven** archetype/SoA storage: it allocates a runtime `kernel::ComponentId` and derives
`kernel::pod_ops` (a trivially-relocatable POD record — memcpy relocate, no destructor), then adds
the component through the World's non-template raw API (`World::add_raw` / `get_raw` / `has_raw` /
`remove_raw`, `src/kernel/`). So a project or npm package defines a new component type declaratively
and it lands in the **same** archetype storage the built-in compile-time components use — no rebuild.

## Scope + seams (this module lands the mechanism core; the following are clean follow-up seams)

- **TS accessors** (typed get/set over the archetype storage, runnable in the 2b-i V8/TS host) — the
  derived-accessor codegen + in-V8 bridge is a follow-up (the V8 dependency path is CI-only; see the
  profile `setup.md`). The published schema + layout here are its input.
- **WASM-load declared-access check** — the `layout_hash` is the compatibility key; the load-time
  check + its reserved R-CLI-008 error code land with the WASM package-migration runner.
- **Restart-class reload** on a live shape change (mirrors R-PLAY-003: discard runtime state +
  re-instantiate + a loud `session` event) — the `session` event topic already reserves the
  announcement; wiring is a follow-up.
- **Migration hooks** (R-DATA-004) — the per-payload migration mechanism (`src/editor/migrate/`) is
  the seam a component-type version bump routes through; noted, not wired here.

## Layering

Depends on `context_schema` (the `x-ctx-*` vocabulary law), `context_serializer` (parse + canonical
publish), and `context_kernel` (raw World storage). Deliberately contract-free — the contract
registry consumes this module, never the reverse (the same rule `src/editor/schema/` follows).
The engine ships **no** built-in declarative component types in v1 (`engine_component_types()` is
empty): they are project/package-defined, which is R-LANG-010's whole point. The built-in
compile-time simulation components live in `src/runtime/session/`.
