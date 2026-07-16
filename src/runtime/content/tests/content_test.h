// Tiny zero-dependency CHECK() harness for the runtime-content ctest executables (mirrors the sibling
// pack/tests/pack_test.h + session/tests/session_test.h — the repo carries no C++ test framework, so
// each test is a plain executable that CHECK()s its invariants and returns non-zero on any failure).

#pragma once

#include <cstdio>

namespace content_test
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace content_test

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            content_test::fail(__FILE__, __LINE__, #cond);                                         \
    } while (false)

#define CONTENT_TEST_MAIN_END() return content_test::g_failures == 0 ? 0 : 1
