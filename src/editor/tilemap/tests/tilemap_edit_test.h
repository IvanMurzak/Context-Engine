// Tiny zero-dependency test harness for the editor/tilemap ctest executables (mirrors the sibling
// module harnesses, e.g. src/editor/kinds/tests/kinds_test.h — the repo carries no C++ test
// framework, so each test is a plain executable that CHECK()s its invariants and returns non-zero on
// any failure).

#pragma once

#include <cstdio>

namespace tilemaptest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace tilemaptest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            tilemaptest::fail(__FILE__, __LINE__, #cond);                                          \
    } while (false)

#define TILEMAP_TEST_MAIN_END() return tilemaptest::g_failures == 0 ? 0 : 1
