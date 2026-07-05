// Internal JSON helpers shared by the importers (build a canonical descriptor tree; read glTF).
// Not a public header — included via a relative path from the importer .cpp files only. Wraps the
// engine's serializer::JsonValue so the importers never hand-format JSON (their descriptors go out
// as canonical bytes for byte-determinism, R-ASSET-001).

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace context::editor::import::detail
{
using serializer::JsonMember;
using serializer::JsonValue;

// --- builders (construct a JsonValue tree an importer serializes canonically) -------------------

[[nodiscard]] inline JsonValue jstr(std::string s)
{
    JsonValue v;
    v.type = JsonValue::Type::string;
    v.string_value = std::move(s);
    return v;
}

[[nodiscard]] inline JsonValue jint(std::int64_t n)
{
    JsonValue v;
    v.type = JsonValue::Type::integer;
    v.int_value = n;
    return v;
}

[[nodiscard]] inline JsonValue juint(std::uint64_t n)
{
    JsonValue v;
    v.type = JsonValue::Type::unsigned_integer;
    v.uint_value = n;
    return v;
}

[[nodiscard]] inline JsonValue jbool(bool b)
{
    JsonValue v;
    v.type = JsonValue::Type::boolean;
    v.boolean_value = b;
    return v;
}

[[nodiscard]] inline JsonValue jobject()
{
    JsonValue v;
    v.type = JsonValue::Type::object;
    return v;
}

[[nodiscard]] inline JsonValue jarray()
{
    JsonValue v;
    v.type = JsonValue::Type::array;
    return v;
}

// Append a member to an object value. The canonical writer sorts keys, so authoring order here is
// irrelevant to the output bytes — but it stays deterministic regardless.
inline void put(JsonValue& obj, std::string key, JsonValue value)
{
    obj.members.push_back(JsonMember{std::move(key), std::move(value)});
}

inline void append(JsonValue& arr, JsonValue value)
{
    arr.elements.push_back(std::move(value));
}

// --- readers (navigate a parsed glTF tree) ------------------------------------------------------

// The object member named `key`, or nullptr (non-object or absent).
[[nodiscard]] inline const JsonValue* member(const JsonValue& obj, std::string_view key)
{
    if (obj.type != JsonValue::Type::object)
        return nullptr;
    for (const JsonMember& m : obj.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

// A signed-integer read from a value that may be integer or unsigned_integer; `fallback` otherwise.
[[nodiscard]] inline std::int64_t as_int64(const JsonValue* v, std::int64_t fallback)
{
    if (v == nullptr)
        return fallback;
    if (v->type == JsonValue::Type::integer)
        return v->int_value;
    if (v->type == JsonValue::Type::unsigned_integer)
        return static_cast<std::int64_t>(v->uint_value);
    return fallback;
}

} // namespace context::editor::import::detail
