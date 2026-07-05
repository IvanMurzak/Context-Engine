// Test-only helpers: parse an importer's canonical descriptor bytes back into a JsonValue and read
// fields, so the importer tests assert on structured values rather than brittle text substrings.
// Reuses the module's internal JSON detail header (white-box test include).

#pragma once

#include "context/editor/serializer/canonical.h"

#include "../src/detail/json_detail.h"

#include <cstdint>
#include <string>

namespace importtest
{
using context::editor::serializer::JsonValue;

inline JsonValue parse_descriptor(const std::string& bytes)
{
    return context::editor::serializer::canonicalize(bytes).root;
}

inline bool is_object(const JsonValue& v)
{
    return v.type == JsonValue::Type::object;
}

inline std::string sfield(const JsonValue& obj, const char* key)
{
    const JsonValue* v = context::editor::import::detail::member(obj, key);
    return (v != nullptr && v->type == JsonValue::Type::string) ? v->string_value : std::string();
}

inline std::int64_t ifield(const JsonValue& obj, const char* key)
{
    return context::editor::import::detail::as_int64(
        context::editor::import::detail::member(obj, key), -1);
}

inline bool bfield(const JsonValue& obj, const char* key)
{
    const JsonValue* v = context::editor::import::detail::member(obj, key);
    return v != nullptr && v->type == JsonValue::Type::boolean && v->boolean_value;
}

inline const JsonValue* ofield(const JsonValue& obj, const char* key)
{
    return context::editor::import::detail::member(obj, key);
}
} // namespace importtest
