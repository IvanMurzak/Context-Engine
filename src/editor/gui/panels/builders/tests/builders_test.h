// Tiny zero-dependency test harness for the context_gui_panel_builders ctest executables (mirrors
// the sibling panel modules' tests/*_test.h — the repo carries no C++ test framework, so each test
// is a plain executable that CHECK()s its invariants and returns non-zero on any failure).

#pragma once

#include <cstdio>

namespace builderstest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

} // namespace builderstest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            builderstest::fail(__FILE__, __LINE__, #cond);                                         \
    } while (false)

#define BUILDERS_TEST_MAIN_END() return builderstest::g_failures == 0 ? 0 : 1
