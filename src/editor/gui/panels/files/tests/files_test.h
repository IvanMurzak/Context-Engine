// Tiny zero-dependency test harness for the gui/panels/files ctest executables (mirrors the sibling
// gui/panels/scenetree/tests/scenetree_test.h — the repo carries no C++ test framework, so each test
// is a plain executable that CHECK()s its invariants and returns non-zero on any failure).

#pragma once

#include <cstdio>

namespace filestest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace filestest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            filestest::fail(__FILE__, __LINE__, #cond);                                            \
    } while (false)

#define FILES_TEST_MAIN_END() return filestest::g_failures == 0 ? 0 : 1
