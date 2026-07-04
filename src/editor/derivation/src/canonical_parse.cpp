// The canonical parse node — a thin adapter over the real canonical-JSON serializer (M2, #42).

#include "context/editor/derivation/canonical_parse.h"

#include "context/editor/serializer/canonical.h"

#include <utility>

namespace context::editor::derivation
{

std::uint64_t canonical_hash_of(std::string_view bytes) noexcept
{
    return serializer::canonical_hash_of(bytes);
}

CanonicalForm canonical_parse(std::string_view source_bytes)
{
    serializer::CanonicalizeResult result = serializer::canonicalize(source_bytes);
    CanonicalForm form;
    form.bytes = std::move(result.bytes);
    form.canonical_hash = result.canonical_hash;
    form.is_json = result.is_json;
    form.diagnostics = std::move(result.diagnostics);
    return form;
}

} // namespace context::editor::derivation
