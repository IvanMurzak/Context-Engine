// Tiny zero-dependency test harness for the particles ctest executables (mirrors the sibling
// modules' tests/*_test.h — the repo carries no C++ test framework, so each test is a plain
// executable that CHECK()s its invariants and returns non-zero on any failure).

#pragma once

#include <cstdio>

namespace particlestest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace particlestest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            particlestest::fail(__FILE__, __LINE__, #cond);                                        \
    } while (false)

#define PARTICLES_TEST_MAIN_END() return particlestest::g_failures == 0 ? 0 : 1
