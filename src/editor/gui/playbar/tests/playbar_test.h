// Tiny zero-dependency test harness for the gui/playbar ctest executables (mirrors the sibling
// gui/viewport/tests/viewport_test.h — the repo carries no C++ test framework, so each test is a plain
// executable that CHECK()s its invariants and returns non-zero on any failure).

#pragma once

#include <cstdio>

namespace playbartest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace playbartest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            playbartest::fail(__FILE__, __LINE__, #cond);                                          \
    } while (false)

#define PLAYBAR_TEST_MAIN_END() return playbartest::g_failures == 0 ? 0 : 1
