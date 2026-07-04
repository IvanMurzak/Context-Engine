// Tiny zero-dependency test harness for the serializer ctest executables (mirrors the kernel's
// tests/kernel_test.h and derivation's tests/derivation_test.h — the repo carries no C++ test
// framework, so each test is a plain executable that CHECK()s its invariants and returns non-zero
// on any failure; ctest treats non-zero as a fail).

#pragma once

#include <cstdio>

namespace sertest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace sertest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            sertest::fail(__FILE__, __LINE__, #cond);                                              \
    } while (false)

#define SERIALIZER_TEST_MAIN_END() return sertest::g_failures == 0 ? 0 : 1
