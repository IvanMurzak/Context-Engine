// Small shared read helpers over the serializer's JSON tree (used by the vocabulary checkers, the
// kind-schema compiler, and the document validator).

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <cmath>
#include <string_view>

namespace context::editor::schema
{

// The value of `object`'s member `key`; nullptr when absent or `object` is not an object.
[[nodiscard]] inline const serializer::JsonValue* find_member(const serializer::JsonValue& object,
                                                              std::string_view key) noexcept
{
    if (object.type != serializer::JsonValue::Type::object)
        return nullptr;
    for (const serializer::JsonMember& m : object.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

// True for the tree's three numeric kinds, excluding non-finite doubles (R-FILE-001 bans them,
// but a programmatically built tree could still carry one — validators stay total).
[[nodiscard]] inline bool is_finite_number(const serializer::JsonValue& v) noexcept
{
    switch (v.type)
    {
    case serializer::JsonValue::Type::integer:
    case serializer::JsonValue::Type::unsigned_integer:
        return true;
    case serializer::JsonValue::Type::number:
        return std::isfinite(v.number_value);
    default:
        return false;
    }
}

// The numeric value as a double (0.0 for non-numbers; check is_finite_number first).
[[nodiscard]] inline double as_double(const serializer::JsonValue& v) noexcept
{
    switch (v.type)
    {
    case serializer::JsonValue::Type::integer:
        return static_cast<double>(v.int_value);
    case serializer::JsonValue::Type::unsigned_integer:
        return static_cast<double>(v.uint_value);
    case serializer::JsonValue::Type::number:
        return v.number_value;
    default:
        return 0.0;
    }
}

} // namespace context::editor::schema
