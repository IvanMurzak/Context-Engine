// Tiny zero-dependency test harness for the file-sync ctest executables (mirrors the kernel's
// tests/kernel_test.h — the repo carries no C++ test framework, so each test is a plain executable
// that CHECK()s its invariants and returns non-zero on any failure; ctest treats non-zero as a fail).

#pragma once

#include <cstdio>

namespace fstest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace fstest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            fstest::fail(__FILE__, __LINE__, #cond);                                               \
    } while (false)

#define FILESYNC_TEST_MAIN_END() return fstest::g_failures == 0 ? 0 : 1
