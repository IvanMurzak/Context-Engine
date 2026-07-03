// Tiny zero-dependency test harness for the editorkernel ctest executables (mirrors the kernel /
// bridge / derivation pattern — the repo carries no C++ test framework, so each test is a plain
// executable that CHECK()s its invariants and returns non-zero on any failure; ctest treats non-zero
// as a fail).

#pragma once

#include <cstdio>

namespace ektest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace ektest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            ektest::fail(__FILE__, __LINE__, #cond);                                              \
    } while (false)

#define EDITORKERNEL_TEST_MAIN_END() return ektest::g_failures == 0 ? 0 : 1
