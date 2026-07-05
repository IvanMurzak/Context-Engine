// Small inline helpers for building + reading serializer::JsonValue trees (session/replay docs).
//
// The session, session-state, and replay serializers all assemble canonical JSON through the
// serializer::JsonValue DOM (the SAME tree the canonical writer round-trips — R-FILE-001 / L-32),
// so a replay artifact and a session-state file ARE canonical engine files. These helpers keep that
// assembly terse without dragging a third-party JSON library into the runtime tier.

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace context::runtime::session::jb
{

using ::context::editor::serializer::JsonMember;
using ::context::editor::serializer::JsonValue;

[[nodiscard]] inline JsonValue str(std::string_view s)
{
    JsonValue v;
    v.type = JsonValue::Type::string;
    v.string_value = std::string(s);
    return v;
}

[[nodiscard]] inline JsonValue integer(std::int64_t n)
{
    JsonValue v;
    v.type = JsonValue::Type::integer;
    v.int_value = n;
    return v;
}

// A non-negative count. Values above i64max serialize as unsigned; the canonical writer preserves
// integer literals losslessly (json_tree.h number domain).
[[nodiscard]] inline JsonValue uinteger(std::uint64_t n)
{
    JsonValue v;
    if (n <= static_cast<std::uint64_t>(INT64_MAX))
    {
        v.type = JsonValue::Type::integer;
        v.int_value = static_cast<std::int64_t>(n);
    }
    else
    {
        v.type = JsonValue::Type::unsigned_integer;
        v.uint_value = n;
    }
    return v;
}

[[nodiscard]] inline JsonValue boolean(bool b)
{
    JsonValue v;
    v.type = JsonValue::Type::boolean;
    v.boolean_value = b;
    return v;
}

[[nodiscard]] inline JsonValue object()
{
    JsonValue v;
    v.type = JsonValue::Type::object;
    return v;
}

[[nodiscard]] inline JsonValue array()
{
    JsonValue v;
    v.type = JsonValue::Type::array;
    return v;
}

inline void set(JsonValue& obj, std::string_view key, JsonValue value)
{
    obj.type = JsonValue::Type::object;
    obj.members.push_back(JsonMember{std::string(key), std::move(value)});
}

inline void push(JsonValue& arr, JsonValue value)
{
    arr.type = JsonValue::Type::array;
    arr.elements.push_back(std::move(value));
}

// --- reads (defensive: a wrong-typed / missing field returns the fallback) ----------------------

[[nodiscard]] inline const JsonValue* member(const JsonValue& obj, std::string_view key)
{
    if (obj.type != JsonValue::Type::object)
        return nullptr;
    for (const JsonMember& m : obj.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

[[nodiscard]] inline std::int64_t as_int(const JsonValue* v, std::int64_t fallback = 0)
{
    if (v == nullptr)
        return fallback;
    if (v->type == JsonValue::Type::integer)
        return v->int_value;
    if (v->type == JsonValue::Type::unsigned_integer)
        return static_cast<std::int64_t>(v->uint_value);
    return fallback;
}

[[nodiscard]] inline std::uint64_t as_uint(const JsonValue* v, std::uint64_t fallback = 0)
{
    if (v == nullptr)
        return fallback;
    if (v->type == JsonValue::Type::unsigned_integer)
        return v->uint_value;
    if (v->type == JsonValue::Type::integer && v->int_value >= 0)
        return static_cast<std::uint64_t>(v->int_value);
    return fallback;
}

[[nodiscard]] inline std::string as_str(const JsonValue* v, std::string_view fallback = {})
{
    if (v != nullptr && v->type == JsonValue::Type::string)
        return v->string_value;
    return std::string(fallback);
}

[[nodiscard]] inline bool as_bool(const JsonValue* v, bool fallback = false)
{
    if (v != nullptr && v->type == JsonValue::Type::boolean)
        return v->boolean_value;
    return fallback;
}

} // namespace context::runtime::session::jb
