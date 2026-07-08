// Tiny zero-dependency test harness for the src/editor/pkg/ ctest executables (mirrors the sibling
// contract/kernel/cli tests/*_test.h). Each test is a plain executable that CHECK()s its invariants
// and returns non-zero on any failure; ctest reads a non-zero exit as a fail.

#pragma once

#include <cstdio>

namespace pkgtest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace pkgtest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            pkgtest::fail(__FILE__, __LINE__, #cond);                                              \
    } while (false)

#define PKG_TEST_MAIN_END() return pkgtest::g_failures == 0 ? 0 : 1
