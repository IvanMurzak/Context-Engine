// Tiny zero-dependency test harness for the context_editor_panels ctest executables (mirrors the
// sibling modules' tests/*_test.h — the repo carries no C++ test framework, so each test is a plain
// executable that CHECK()s its invariants and returns non-zero on any failure).
//
// Deliberately NOT a reuse of ../../tests/shell_test.h: that header includes context/render/rhi.h
// for its compositor fixtures, and nothing in this suite builds against the render tier.

#pragma once

#include <cstdio>
#include <string>

namespace panelstest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

[[nodiscard]] inline bool mentions(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace panelstest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            panelstest::fail(__FILE__, __LINE__, #cond);                                           \
    } while (false)

#define PANELS_TEST_MAIN_END() return panelstest::g_failures == 0 ? 0 : 1
