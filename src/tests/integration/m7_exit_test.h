// Shared harness for the M7 EXIT GATE integration tests (design 2026-07-13-m7-runtime-ui / a12-m7-exit;
// issue-tracked as the five blocking m7-exit-* ctests). The milestone-closing mirror of m2_exit_test.h /
// m5_exit_test.h: a tiny assertion harness (a CHECK macro + a failure counter + a uniform reporter) the
// five test_m7exit*.cpp gates share, so each gate reads like the m6-exit family (one assertion per claim,
// fail-closed, legible per OS leg). Header-only, test-only. No GPU, no renderer — the M7 runtime-UI
// authoring/logic loop is CI-assertable headless (R-UI-006 / D6).

#pragma once

#include <cstdio>

namespace context::tests::m7
{

inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

// Uniform terminal report: prints the failure count + returns 1 when anything failed, else prints the
// gate's success line + returns 0. Call it as `return context::tests::m7::report("m7-exit-N-...", "...");`.
[[nodiscard]] inline int report(const char* gate, const char* ok_message)
{
    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("%s: %s\n", gate, ok_message);
    return 0;
}

} // namespace context::tests::m7

// One assertion. Records file/line/expression on failure (fail-closed, never aborts — every claim runs
// so a whole gate's failures surface in one pass), mirroring the m6-exit family's CHECK.
#define CHECK(cond)                                                                                    \
    do                                                                                                 \
    {                                                                                                  \
        if (!(cond))                                                                                   \
            ::context::tests::m7::fail(__FILE__, __LINE__, #cond);                                     \
    } while (false)
