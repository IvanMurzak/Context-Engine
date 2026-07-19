// Tiny zero-dependency test harness for the render ctest executables (mirrors the sibling modules'
// tests/*_test.h — the repo carries no C++ test framework, so each test is a plain executable that
// CHECK()s its invariants and returns non-zero on any failure).

#pragma once

#include <cstdio>
#include <string>

namespace rendertest
{
inline int g_failures = 0;

// Substring assertion for diagnostic strings — the one thing several present-path tests each had
// their own private copy of. Lives here so a test asserting "the report names the reason" reads the
// same way in every file.
[[nodiscard]] inline bool mentions(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace rendertest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            rendertest::fail(__FILE__, __LINE__, #cond);                                           \
    } while (false)

#define RENDER_TEST_MAIN_END() return rendertest::g_failures == 0 ? 0 : 1
