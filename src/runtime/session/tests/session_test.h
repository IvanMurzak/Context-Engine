// Tiny zero-dependency test harness for the session ctest executables (the sibling kernel/save
// pattern: the repo carries no C++ test framework — adding one would drag a vcpkg dep + a
// license-allowlist entry). Each test is a plain executable that CHECK()s its invariants and returns
// non-zero on any failure; ctest treats a non-zero exit as a failing test.

#pragma once

#include <cstdio>

namespace stest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace stest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            stest::fail(__FILE__, __LINE__, #cond);                                                \
    } while (false)

#define SESSION_TEST_MAIN_END() return stest::g_failures == 0 ? 0 : 1
