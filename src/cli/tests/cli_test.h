// Tiny zero-dependency test harness for the CLI ctest executables (mirrors the kernel/contract
// pattern). Each test is a plain executable that CHECK()s invariants and returns non-zero on any
// failure; ctest reads a non-zero exit as a fail.

#pragma once

#include <cstdio>

namespace clitest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace clitest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            clitest::fail(__FILE__, __LINE__, #cond);                                              \
    } while (false)

#define CLI_TEST_MAIN_END() return clitest::g_failures == 0 ? 0 : 1
