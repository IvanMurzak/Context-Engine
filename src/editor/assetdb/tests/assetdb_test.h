// Tiny zero-dependency test harness for the assetdb ctest executables (mirrors the sibling
// modules' tests/*_test.h — the repo carries no C++ test framework, so each test is a plain
// executable that CHECK()s its invariants and returns non-zero on any failure).

#pragma once

#include <cstdio>

namespace assetdbtest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace assetdbtest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            assetdbtest::fail(__FILE__, __LINE__, #cond);                                          \
    } while (false)

#define ASSETDB_TEST_MAIN_END() return assetdbtest::g_failures == 0 ? 0 : 1
