// Tiny zero-dependency test harness for the simmath ctest executables (mirrors the sibling modules'
// tests/*_test.h — the repo carries no C++ test framework, so each test is a plain executable that
// CHECK()s its invariants and returns non-zero on any failure).

#pragma once

#include "context/packages/simmath/fixed.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace simmathtest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

// Convert a Fixed to double — TEST-ONLY (never on the sim path), so a fixed-point result can be
// compared to a libm reference within a tolerance. The library itself never touches float.
[[nodiscard]] inline double to_double(context::packages::simmath::Fixed f)
{
    return static_cast<double>(f.raw) / static_cast<double>(context::packages::simmath::kFixedOneRaw);
}

// True when `actual` (a Fixed) is within `tol` of the double reference `expected`.
[[nodiscard]] inline bool close(context::packages::simmath::Fixed actual, double expected, double tol)
{
    return std::fabs(to_double(actual) - expected) <= tol;
}
} // namespace simmathtest

// Hoist the tolerance oracles into the global namespace so tests can call them unqualified (they are
// not found by ADL — their Fixed argument lives in context::packages::simmath, not simmathtest).
using simmathtest::close;
using simmathtest::to_double;

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            simmathtest::fail(__FILE__, __LINE__, #cond);                                          \
    } while (false)

#define SIMMATH_TEST_MAIN_END() return simmathtest::g_failures == 0 ? 0 : 1
