// Tiny zero-dependency harness for the L-50 concurrency-validation ctest executables (issue #284,
// a16-l50). Mirrors the sibling modules' tests/*_test.h — the repo carries no C++ test framework, so
// each test is a plain executable that CHECK()s its invariants and returns non-zero on any failure.
// Plus the serializer JsonValue builders + structural equality + parse/canonical helpers the two
// scenarios build their in-memory worlds from.

#pragma once

#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"
#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace conctest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

using context::editor::serializer::JsonValue;

// --- value builders ---------------------------------------------------------------------------
inline JsonValue jint(std::int64_t n)
{
    JsonValue v;
    v.type = JsonValue::Type::integer;
    v.int_value = n;
    return v;
}
inline JsonValue jstr(std::string s)
{
    JsonValue v;
    v.type = JsonValue::Type::string;
    v.string_value = std::move(s);
    return v;
}
inline JsonValue jbool(bool b)
{
    JsonValue v;
    v.type = JsonValue::Type::boolean;
    v.boolean_value = b;
    return v;
}

// Structural JSON equality, matching the canonical form's identity: objects are order-INSENSITIVE by
// key, arrays are order-SENSITIVE, scalars match exactly including the integer/unsigned/number domain
// (json_tree.h). Used to assert the no-lost-update invariant on the co-edit world.
inline bool json_eq(const JsonValue& a, const JsonValue& b)
{
    if (a.type != b.type)
        return false;
    switch (a.type)
    {
    case JsonValue::Type::null_value:
        return true;
    case JsonValue::Type::boolean:
        return a.boolean_value == b.boolean_value;
    case JsonValue::Type::integer:
        return a.int_value == b.int_value;
    case JsonValue::Type::unsigned_integer:
        return a.uint_value == b.uint_value;
    case JsonValue::Type::number:
        return a.number_value == b.number_value;
    case JsonValue::Type::string:
        return a.string_value == b.string_value;
    case JsonValue::Type::array:
    {
        if (a.elements.size() != b.elements.size())
            return false;
        for (std::size_t i = 0; i < a.elements.size(); ++i)
            if (!json_eq(a.elements[i], b.elements[i]))
                return false;
        return true;
    }
    case JsonValue::Type::object:
    {
        if (a.members.size() != b.members.size())
            return false;
        for (const auto& ma : a.members)
        {
            const JsonValue* found = nullptr;
            for (const auto& mb : b.members)
                if (mb.key == ma.key)
                {
                    found = &mb.value;
                    break;
                }
            if (found == nullptr || !json_eq(ma.value, *found))
                return false;
        }
        return true;
    }
    }
    return false;
}

// Parse strict JSON into a tree (test inputs are authored valid). Flags a failure on malformed input.
inline JsonValue parse(std::string_view text)
{
    context::editor::serializer::ParseResult parsed = context::editor::serializer::parse_json(text);
    if (!parsed.ok)
        std::fprintf(stderr, "concurrency test input did not parse as strict JSON: %.*s\n",
                     static_cast<int>(text.size()), text.data());
    return std::move(parsed.root);
}

// Canonical bytes of a tree (total for finite test trees).
inline std::string canon(const JsonValue& root)
{
    std::string out;
    if (!context::editor::serializer::serialize_canonical(root, out))
        std::fprintf(stderr, "concurrency test tree did not serialize canonically\n");
    return out;
}
} // namespace conctest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                              \
            conctest::fail(__FILE__, __LINE__, #cond);                                            \
    } while (false)

#define CONC_TEST_MAIN_END() return conctest::g_failures == 0 ? 0 : 1
