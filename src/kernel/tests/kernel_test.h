// Tiny zero-dependency test harness for the kernel ctest executables.
//
// The repo carries no C++ test framework (adding one would drag in a vcpkg dep + a license-allowlist
// entry, R-QA-013 notwithstanding), so each kernel test is a plain executable that CHECK()s its
// invariants and returns non-zero on any failure. ctest treats a non-zero exit as a failing test.

#pragma once

#include <cstdio>

namespace ktest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace ktest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            ktest::fail(__FILE__, __LINE__, #cond);                                                \
    } while (false)

#define KERNEL_TEST_MAIN_END() return ktest::g_failures == 0 ? 0 : 1
