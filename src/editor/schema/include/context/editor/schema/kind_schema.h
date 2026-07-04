// Per-kind versioned schema publication (R-DATA-006, L-32): the compiled kind schemas + the
// registration set the ONE contract registry enumerates through `context describe` (R-CLI-005/013).

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::schema
{

// The engine-owned kind ids (M2 wave 2). `$schema` in an authored file names the kind; the sibling
// "version" header field selects the registered schema version (L-32 header block).
inline constexpr std::string_view kSceneKindId = "ctx:scene";
inline constexpr std::string_view kProjectKindId = "ctx:project";

// One compiled per-kind versioned schema. `doc` is the schema DOCUMENT (the dialect below);
// `canonical_doc` is its canonical-JSON serialization — the published form (R-DATA-006: published,
// versioned JSON Schema per kind).
//
// Schema-document dialect (a deliberately small JSON-Schema-flavored subset — enough for the
// engine vocabulary; it grows with the component-schema work):
//   "$id" (string, the kind id) + "version" (integer >= 1) at the root;
//   per node: "type" ("object"|"array"|"string"|"number"|"integer"|"boolean"),
//             "properties" (object of sub-schemas), "required" (array of property names),
//             "additionalProperties" (bool, default true), "items" (sub-schema),
//             "enum" (array of strings),
//             and the x-ctx-* vocabulary keys (vocabulary.h): "x-ctx-type", "x-ctx-units",
//             "x-ctx-storage", "x-ctx-ref", "x-ctx-union" (object: tag -> variant sub-schema).
// LAW enforcement happens at compile time (compile_kind_schema): a schema declaring a non-SI
// x-ctx-units id, an unknown x-ctx-type, a malformed x-ctx-storage layout, or a malformed
// x-ctx-union tag is REJECTED — the vocabulary cannot be forked per package. Every kind's root
// must declare the blessed `notes` property (L-32: annotations live in schema-blessed notes).
struct KindSchema
{
    std::string id;            // the kind id ("$id"), e.g. "ctx:scene"
    std::int64_t version = 0;  // the schema version ("version"), >= 1
    serializer::JsonValue doc; // the parsed schema document
    std::string canonical_doc; // the published canonical-JSON form of `doc`
};

// Compile `schema_json` (the schema document text) into a KindSchema, enforcing the vocabulary
// law. On any violation returns nullopt and appends human-readable problems (each prefixed with
// the offending schema-document JSON pointer).
[[nodiscard]] std::optional<KindSchema> compile_kind_schema(std::string_view schema_json,
                                                            std::vector<std::string>& problems);

// The per-kind versioned registration set. Engine kinds register at startup (engine_schemas());
// package-contributed kinds join through the same add() as the package system lands — the set is
// what the contract registry's `describe` enumerates live (R-CLI-005: introspection is a live
// capability, never a hand-maintained list).
class SchemaSet
{
public:
    // Registers one compiled schema. Replaces an existing (id, version) entry (idempotent re-add).
    void add(KindSchema schema);

    // Exact (id, version) lookup; nullptr when absent.
    [[nodiscard]] const KindSchema* find(std::string_view id, std::int64_t version) const noexcept;
    // The highest registered version of `id`; nullptr when the kind is unknown.
    [[nodiscard]] const KindSchema* latest(std::string_view id) const noexcept;

    [[nodiscard]] const std::vector<KindSchema>& all() const noexcept { return schemas_; }

private:
    std::vector<KindSchema> schemas_;
};

// The engine-owned schema set: the scene kind (the M1 placeholder migrated onto this mechanism)
// and the project-manifest kind, each at version 1. Built once; process-wide.
[[nodiscard]] const SchemaSet& engine_schemas();

// The published introspection entry for one kind, as a canonical-JSON string:
//   {"fields": [{"pointer", "type", "x-ctx-*"...}...], "id", "schema": <doc>, "version"}
// `fields` is a flattened per-field index (JSON pointer -> declared type + x-ctx annotations,
// including x-ctx-units — the units-law introspection surface R-DATA-006 mandates) DERIVED from
// the schema document, so the two can never drift. The contract registry parses this into its
// `describe` fileKinds section (R-CLI-005/013) — the schema module stays contract-DOM-free.
[[nodiscard]] std::string introspection_json(const KindSchema& schema);

} // namespace context::editor::schema
