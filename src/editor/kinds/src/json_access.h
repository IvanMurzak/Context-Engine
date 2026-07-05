// Small shared read helpers over the serializer's JSON tree, used by every content-kind analyzer
// (tilemap, string_table, …). Factored out of the individual kind .cpp files so the identical
// member() / array_member() / string_member() accessors cannot drift between kinds (M2 polish audit,
// issue #70). Header-only + internal to the kinds module — mirrors merge/src/pointer_format.h and
// schema/json_access.h. Deliberately serializer-only (the kinds module depends on nothing heavier;
// see kinds/CMakeLists.txt), so this is NOT schema::json_access — the two do not cross layers.

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <string_view>

namespace context::editor::kinds
{

// The value of `object`'s member `key`; nullptr when absent or `object` is not an object.
[[nodiscard]] inline const serializer::JsonValue* member(const serializer::JsonValue& object,
                                                         std::string_view key)
{
    if (object.type != serializer::JsonValue::Type::object)
        return nullptr;
    for (const serializer::JsonMember& m : object.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

// The ARRAY value at object[key], or nullptr when absent / not an array.
[[nodiscard]] inline const serializer::JsonValue* array_member(const serializer::JsonValue& object,
                                                               std::string_view key)
{
    const serializer::JsonValue* v = member(object, key);
    return (v != nullptr && v->type == serializer::JsonValue::Type::array) ? v : nullptr;
}

// The STRING value at object[key], or nullptr when absent / not a string.
[[nodiscard]] inline const serializer::JsonValue* string_member(const serializer::JsonValue& object,
                                                                std::string_view key)
{
    const serializer::JsonValue* v = member(object, key);
    return (v != nullptr && v->type == serializer::JsonValue::Type::string) ? v : nullptr;
}

} // namespace context::editor::kinds
