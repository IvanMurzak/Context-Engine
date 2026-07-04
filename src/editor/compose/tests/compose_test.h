// Tiny zero-dependency test harness for the compose ctest executables (mirrors the sibling
// tests/derivation_test.h / tests/schema_test.h — the repo carries no C++ test framework, so each
// test is a plain executable that CHECK()s its invariants and returns non-zero on any failure).

#pragma once

#include <cstdio>

namespace cmptest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace cmptest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            cmptest::fail(__FILE__, __LINE__, #cond);                                              \
    } while (false)

#define COMPOSE_TEST_MAIN_END() return cmptest::g_failures == 0 ? 0 : 1
