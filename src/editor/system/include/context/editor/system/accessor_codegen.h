// Derived TS accessor codegen (R-LANG-010 deferred item 1): from a compiled declarative component
// type, generate a small, deterministic TypeScript module that gives authored TS typed get/set access
// to that component's records — over the zero-copy byte view the (query, executor) tier binds through
// runSystemView (R-LANG-009). The generated class wraps a DataView over the view's ArrayBuffer, so it
// reads/writes the EXACT bytes the host packs into the VmBuffer (no copy), and every scalar family
// (f32/f64, i8..i64, u8..u64, plus 2/3/4/9/16-lane vectors/matrices) is addressed by the field's
// derived byte offset. Deterministic: the same ComponentTypeSchema always yields byte-identical TS.

#pragma once

#include "context/editor/component/component_type.h"

#include <string>

namespace context::editor::system
{

// Generate a TypeScript accessor module for `type` (component_type.h). The emitted module `export`s a
// class named after the component id (its non-alphanumeric characters mapped to '_') whose instance
// wraps a `Uint8Array` view and exposes, per field:
//   * a scalar field  -> get<Field>(row): T           / set<Field>(row, v: T): void
//   * a lanes>1 field  -> get<Field>(row, lane): T     / set<Field>(row, lane, v: T): void
// plus a `count` getter (view byteLength / record stride) and a static `stride` (the record size).
// `T` is `number` for the float/≤32-bit-int bases and `bigint` for i64/u64 (DataView BigInt accessors).
//
// The record layout is little-endian — every CI/host target is little-endian (x86_64 + ARM64), and the
// host gathers native scalar bytes into the VmBuffer, so the LE DataView reads/writes reproduce the
// host's native records bit-for-bit (the L-54 determinism law holds across the 3-OS matrix). A future
// big-endian port would flip the endianness flag here and in the gather/scatter memcpy — documented in
// README.md § Endianness, out of scope for this task.
//
// PRECONDITION — field names must be UNIQUE under the PascalCase method mapping. Each field `f` emits a
// `get<Pascal(f)>` / `set<Pascal(f)>` accessor pair, where Pascal() splits on '_' and capitalizes each
// run's first letter. Two distinct schema-valid names (`[a-z][a-z0-9_]*`) that differ only in underscore
// runs — e.g. "max_hp" and "max__hp" — both map to "MaxHp", so a schema carrying both would emit two
// identically-named members and the JS class would silently keep only the last (one field's accessor is
// unreachable). Uniqueness is the component compiler's job (it already owns field-name validation for
// the derived accessor surface, see component_type.cpp); this codegen assumes a validated, collision-free
// field set and does NOT re-check it.
[[nodiscard]] std::string generate_component_accessor_ts(const component::ComponentTypeSchema& type);

// The class identifier `generate_component_accessor_ts` emits for `id` (a component "$id" such as
// "demo:mover"): every character outside [A-Za-z0-9_] is replaced by '_', and a leading digit is
// prefixed with '_' so the result is always a valid JS identifier. Exposed so a caller (e.g. the
// authored-system bundler, or a test) can name the generated class without re-deriving the rule.
[[nodiscard]] std::string accessor_class_name(std::string_view id);

} // namespace context::editor::system
