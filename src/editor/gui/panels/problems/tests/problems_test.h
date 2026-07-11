// Tiny zero-dependency test harness for the gui/panels/problems ctest executables (mirrors the
// sibling src/editor/gui/panels/scenetree/tests/scenetree_test.h — the repo carries no C++ test
// framework, so each test is a plain executable that CHECK()s its invariants and returns non-zero on
// any failure).

#pragma once

#include <cstdio>

namespace problemstest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace problemstest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            problemstest::fail(__FILE__, __LINE__, #cond);                                         \
    } while (false)

#define PROBLEMS_TEST_MAIN_END() return problemstest::g_failures == 0 ? 0 : 1
