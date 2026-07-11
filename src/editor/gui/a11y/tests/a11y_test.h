// Tiny zero-dependency test harness for the gui/a11y ctest executables (mirrors the sibling
// src/editor/gui/uitree/tests/uitree_test.h + src/editor/gui/contract/tests/contract_test.h). The
// repo carries no C++ test framework — each test is a plain executable that CHECK()s its invariants
// and returns non-zero on any failure; ctest reads a non-zero exit as a fail.

#pragma once

#include <cstdio>

namespace ctest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace ctest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            ctest::fail(__FILE__, __LINE__, #cond);                                                \
    } while (false)

#define A11Y_TEST_MAIN_END() return ctest::g_failures == 0 ? 0 : 1
