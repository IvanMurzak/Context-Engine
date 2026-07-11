// Tiny zero-dependency test harness for the gui/panels/scenetree ctest executables (mirrors the
// sibling src/editor/gui/uitree/tests/uitree_test.h — the repo carries no C++ test framework, so each
// test is a plain executable that CHECK()s its invariants and returns non-zero on any failure).

#pragma once

#include <cstdio>

namespace scenetreetest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace scenetreetest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            scenetreetest::fail(__FILE__, __LINE__, #cond);                                        \
    } while (false)

#define SCENETREE_TEST_MAIN_END() return scenetreetest::g_failures == 0 ? 0 : 1
