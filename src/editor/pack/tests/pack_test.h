// Tiny zero-dependency test harness for the pack ctest executables (mirrors the sibling
// compose/tests/compose_test.h — the repo carries no C++ test framework, so each test is a plain
// executable that CHECK()s its invariants and returns non-zero on any failure).

#pragma once

#include <cstdio>

namespace packtest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace packtest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            packtest::fail(__FILE__, __LINE__, #cond);                                             \
    } while (false)

#define PACK_TEST_MAIN_END() return packtest::g_failures == 0 ? 0 : 1
