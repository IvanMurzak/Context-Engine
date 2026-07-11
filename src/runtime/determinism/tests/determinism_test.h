// Tiny zero-dependency test harness for the determinism ctest executables (the sibling
// kernel/session/save pattern: the repo carries no C++ test framework — adding one would drag a
// vcpkg dep + a license-allowlist entry). Each test is a plain executable that CHECK()s its
// invariants and returns non-zero on any failure; ctest treats a non-zero exit as a failing test.

#pragma once

#include <cstdio>

namespace dtest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace dtest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            dtest::fail(__FILE__, __LINE__, #cond);                                                \
    } while (false)

#define DETERMINISM_TEST_MAIN_END() return dtest::g_failures == 0 ? 0 : 1
