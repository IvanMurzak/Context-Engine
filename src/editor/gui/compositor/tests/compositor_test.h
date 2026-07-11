// Tiny zero-dependency test harness for the gui/compositor ctest executables (mirrors the sibling
// gui/uitree + src/editor/contract + src/editor/bridge harnesses).

#pragma once

#include <cstdio>

namespace ctest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace ctest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            ctest::fail(__FILE__, __LINE__, #cond);                                                \
    } while (false)

#define UITREE_TEST_MAIN_END() return ctest::g_failures == 0 ? 0 : 1
