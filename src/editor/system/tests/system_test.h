// Tiny zero-dependency test harness for the system ctest executables (mirrors the sibling modules'
// tests/*_test.h — the repo carries no C++ test framework, so each test is a plain executable that
// CHECK()s its invariants and returns non-zero on any failure).

#pragma once

#include <cstdio>

namespace systemtest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace systemtest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            systemtest::fail(__FILE__, __LINE__, #cond);                                           \
    } while (false)

#define SYSTEM_TEST_MAIN_END() return systemtest::g_failures == 0 ? 0 : 1
