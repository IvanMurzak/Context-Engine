// Per-kind schema validation of authored documents — the validate half of R-DATA-006, feeding the
// derivation validate node's R-FILE-003 diagnostics (JSON-pointer + line/column).

#pragma once

#include "context/editor/schema/kind_schema.h"
#include "context/editor/serializer/json_tree.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::schema
{

// The typed-reference meta-lookup seam (R-DATA-006): resolves a `$ref` GUID to the kind of the
// asset it names, so a reference to the WRONG kind is a validate error, not a runtime surprise.
// The asset database implements this when it lands (L-36); until then callers pass nullptr and the
// validator enforces reference SHAPE only — the vocabulary + seam ship now, the lookup later.
class RefTargetResolver
{
public:
    virtual ~RefTargetResolver() = default;
    // The kind id of the asset `guid` names, or nullopt when unknown (unknown = not enforced).
    [[nodiscard]] virtual std::optional<std::string> kind_of(std::string_view guid) const = 0;
};

// One machine-readable validation finding (the R-FILE-003 shape): a stable dotted code, a
// human/AI-readable message, the JSON pointer of the offending value, and its 1-based line/column
// in the SOURCE bytes (0/0 when the pointer could not be located, e.g. header-level findings).
struct ValidationDiagnostic
{
    std::string code;
    std::string message;
    std::string pointer;
    std::size_t line = 0;
    std::size_t column = 0;
};

// The outcome of validating one authored document against the registered kind schemas.
//
//   schema_bound == false — the document declares no `$schema` (validation does not apply — but a
//                           PRESENT-but-malformed `$schema` is an attempted binding and surfaces
//                           its blocking "header.*" findings rather than silently skipping), or
//                           declares an UNREGISTERED kind/version (non-blocking informational
//                           diagnostics "schema.unknown_kind" / "schema.version_unregistered" —
//                           kinds register incrementally, so unknown ids must not block derive).
//   ok == false           — a BLOCKING finding: the payload failed its kind schema, its header is
//                           malformed ("schema.version_missing", header.* shapes), or it is
//                           stamped NEWER than the registered schema ("schema.newer_than_engine",
//                           the L-37 rule) — the derivation node retains last-good state.
struct ValidationReport
{
    bool schema_bound = false;
    std::string schema_id;
    std::int64_t version = 0;
    bool ok = true;
    std::vector<ValidationDiagnostic> diagnostics;
};

// Validate a parsed authored document. `root` is the parse tree of `source_bytes` (the derivation
// parse node already has both — validation never re-parses); `source_bytes` is used only to locate
// diagnostics (line/column), so the two MUST describe the same bytes.
[[nodiscard]] ValidationReport validate_document(const serializer::JsonValue& root,
                                                 std::string_view source_bytes,
                                                 const SchemaSet& schemas,
                                                 const RefTargetResolver* resolver = nullptr);

// Locate the value a JSON pointer addresses inside strict-JSON `bytes`: 1-based line/column of the
// value's first byte. False when the pointer does not resolve (line/column untouched). Exposed for
// tests and for future diagnostic surfaces that carry only a pointer.
[[nodiscard]] bool locate_pointer(std::string_view bytes, std::string_view pointer,
                                  std::size_t& line, std::size_t& column);

} // namespace context::editor::schema
