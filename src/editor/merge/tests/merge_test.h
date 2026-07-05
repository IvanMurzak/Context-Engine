// Tiny zero-dependency test harness for the merge ctest executables (mirrors the sibling modules'
// tests/*_test.h — the repo carries no C++ test framework, so each test is a plain executable that
// CHECK()s its invariants and returns non-zero on any failure). Plus the JSON parse/canonical helpers
// the merge tests build their inputs from (string literals in, canonical bytes out).

#pragma once

#include "context/editor/merge/conflict.h"
#include "context/editor/merge/three_way_merge.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"
#include "context/editor/serializer/json_tree.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace mergetest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

using context::editor::serializer::JsonValue;

// Parse strict JSON into a tree (test inputs are authored valid). Flags a failure on malformed input.
inline JsonValue parse(std::string_view text)
{
    context::editor::serializer::ParseResult parsed = context::editor::serializer::parse_json(text);
    if (!parsed.ok)
        std::fprintf(stderr, "test input did not parse as strict JSON: %.*s\n",
                     static_cast<int>(text.size()), text.data());
    return std::move(parsed.root);
}

// Canonical bytes of a tree (total for finite test trees).
inline std::string canon(const JsonValue& root)
{
    std::string out;
    if (!context::editor::serializer::serialize_canonical(root, out))
        std::fprintf(stderr, "test tree did not serialize canonically\n");
    return out;
}

// The conflict at `path` in a merge result, or nullptr when none. (Whole-file classes use path "".)
inline const context::editor::merge::Conflict*
conflict_at(const context::editor::merge::MergeResult& result, std::string_view path)
{
    for (const context::editor::merge::Conflict& c : result.conflicts)
        if (c.path == path)
            return &c;
    return nullptr;
}
} // namespace mergetest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            mergetest::fail(__FILE__, __LINE__, #cond);                                            \
    } while (false)

#define MERGE_TEST_MAIN_END() return mergetest::g_failures == 0 ? 0 : 1
