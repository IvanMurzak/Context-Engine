// Declarative component-type authoring (R-LANG-010): schema-first component definitions compiled
// into a runtime storage layout + a published, versioned schema.
//
// ONE declarative definition (a small JSON-Schema-flavored document reusing the M2 x-ctx-* schema
// vocabulary) derives everything a component type needs WITHOUT a native rebuild: the ECS storage
// layout (field offsets + total size + alignment interpreted from x-ctx-storage widths), the
// canonical-JSON payload encoding, the published schema `context describe` introspects (R-CLI-005),
// and a stable layout hash (the day-one contract a WASM module's declared access is later checked
// against — task follow-up). Runtime-registered → data-driven archetype storage (component_registry.h),
// so defining or changing a component type requires NO native engine rebuild in v1 (L-60).

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::component
{

// --- numeric storage scalars (the x-ctx-storage base grammar, vocabulary.h) ----------------------

// The scalar base of a field's storage layout. Mirrors the pinned x-ctx-storage bases exactly:
// f32/f64 floats, i8..i64 signed, u8..u64 unsigned. The declarative compiler derives byte width +
// alignment from the base; a `lanes` count (vocabulary.h: 2/3/4/9/16) packs vectors/quats/matrices.
enum class ScalarKind
{
    f32,
    f64,
    i8,
    i16,
    i32,
    i64,
    u8,
    u16,
    u32,
    u64,
};

// The pinned x-ctx-storage id of a scalar base ("f32", "i64", ...).
[[nodiscard]] std::string_view scalar_kind_id(ScalarKind kind) noexcept;

// Byte width of one scalar of `kind` (1/2/4/8). Doubles as the field's natural alignment.
[[nodiscard]] std::size_t scalar_byte_width(ScalarKind kind) noexcept;

// A parsed storage layout: a scalar base repeated `lanes` times (lanes == 1 is a plain scalar; 2/3/4
// are vectors/quaternions, 9/16 are 3x3/4x4 matrices — the pinned x-ctx-storage lane set).
struct StorageLayout
{
    ScalarKind base = ScalarKind::f32;
    unsigned lanes = 1;

    // Total bytes occupied (scalar width * lanes) and natural alignment (the scalar width).
    [[nodiscard]] std::size_t byte_size() const noexcept
    {
        return scalar_byte_width(base) * lanes;
    }
    [[nodiscard]] std::size_t align() const noexcept { return scalar_byte_width(base); }
};

// Parse an x-ctx-storage layout string ("<base>" or "<base>x<lanes>") into a StorageLayout. Returns
// nullopt for any string is_storage_layout() (vocabulary.h) would reject — the two stay in lockstep
// so a field the schema validator accepts always parses here.
[[nodiscard]] std::optional<StorageLayout> parse_storage_layout(std::string_view layout) noexcept;

// --- a compiled component type -------------------------------------------------------------------

// One field of a declared component type: its authored name, its storage layout, and the byte offset
// the compiler placed it at within the packed component record (each field aligned to its scalar
// width; the record's size/alignment is the max field alignment, rounded up).
struct ComponentField
{
    std::string name;
    StorageLayout storage;
    std::size_t offset = 0;
};

// A compiled declarative component type — the runtime-registerable derivation of ONE declarative
// definition. `doc` is the normalized schema document; `canonical_doc` its canonical-JSON publication
// (the R-CLI-005 published form). `size`/`align`/`fields[].offset` are the derived ECS storage layout;
// `layout_hash` is a stable digest of the ordered (name, storage) list (a change to any field name,
// base, lane count, or order changes it — the WASM-load compatibility key, wired in a follow-up task).
struct ComponentTypeSchema
{
    std::string id;                        // the "<ns>:<type>" id ("$id")
    std::int64_t version = 0;              // the schema version ("version"), >= 1
    std::vector<ComponentField> fields;    // ordered, with derived offsets
    std::size_t size = 0;                  // total record bytes (>= 1; padded to `align`)
    std::size_t align = 1;                 // record alignment (max field alignment)
    std::uint64_t layout_hash = 0;         // stable digest of the ordered (name, storage) list
    serializer::JsonValue doc;             // the normalized schema document
    std::string canonical_doc;             // `doc` serialized canonically (the published form)

    // The field named `name`, or nullptr. Linear scan (field counts are small, typically < 32).
    [[nodiscard]] const ComponentField* field(std::string_view name) const noexcept;
    [[nodiscard]] std::size_t field_count() const noexcept { return fields.size(); }
};

// Compile `schema_json` (the declarative definition text) into a ComponentTypeSchema, enforcing the
// declarative dialect + the M2 vocabulary law. On any violation returns nullopt and appends
// human-readable problems (each prefixed with the offending JSON pointer, mirroring the kind-schema
// compiler).
//
// Declarative dialect (a deliberately small subset — enough to derive storage; it shares the
// x-ctx-* vocabulary so a component field and a file-kind field annotate identically):
//   root object: "$id" (string "<ns>:<type>"), "version" (integer >= 1), "fields" (non-empty array),
//                optional "notes" (the L-32 blessed annotation), optional "description" (string).
//   each field:  "name" (unique, non-empty, [a-z][a-z0-9_]* ), "x-ctx-storage" (a valid layout),
//                optional "x-ctx-units" (a pinned SI unit — the units LAW, metadata only),
//                optional "x-ctx-type" (a pinned semantic type id), optional "description"/"notes".
// A non-SI x-ctx-units, a malformed x-ctx-storage, an unknown x-ctx-type, a duplicate/malformed field
// name, or a bad id/version is REJECTED — the vocabulary cannot be forked per package.
[[nodiscard]] std::optional<ComponentTypeSchema>
compile_component_type(std::string_view schema_json, std::vector<std::string>& problems);

// The stable layout digest of an ordered field list (FNV-1a 64-bit over each field's name + storage
// base + lane count, in order). Deterministic + platform-independent (the L-54 determinism law): the
// same fields in the same order always hash identically, and ANY change to a name/base/lanes/order
// changes it. This is the compatibility key a WASM module's declared component access is checked
// against at module load (mismatch => a machine-readable load error — reserved for the follow-up task).
[[nodiscard]] std::uint64_t layout_hash_of(const std::vector<ComponentField>& fields) noexcept;

// The published introspection entry for one component type, as a canonical-JSON string (keys are
// emitted in canonical/sorted order):
//   {"align", "fields": [{"lanes", "name", "offset", "size", "x-ctx-storage"}...],
//    "id", "layoutHash", "schema": <doc>, "size", "version"}
// The `fields` index is DERIVED from the compiled layout (offset/size/lanes the schema text alone
// does not carry), so the storage layout and the published schema can never drift; the full authored
// definition — including any x-ctx-units / x-ctx-type annotations — rides along under "schema". The
// contract registry projects this into its `describe` componentTypes section (R-CLI-005), staying
// contract-DOM-free.
[[nodiscard]] std::string component_type_introspection_json(const ComponentTypeSchema& type);

} // namespace context::editor::component
