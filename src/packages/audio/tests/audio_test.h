// Tiny zero-dependency test harness for the audio ctest executables (mirrors the sibling packages'
// tests/*_test.h — the repo carries no C++ test framework, so each test is a plain executable that
// CHECK()s its invariants and returns non-zero on any failure).

#pragma once

#include <cstdio>

namespace audiotest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace audiotest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            audiotest::fail(__FILE__, __LINE__, #cond);                                            \
    } while (false)

#define AUDIO_TEST_MAIN_END() return audiotest::g_failures == 0 ? 0 : 1
