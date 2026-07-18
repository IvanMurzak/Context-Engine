// Shared harness for the M8 EXIT GATE integration tests (ROADMAP §1-M8 exit / a14-m8-exit; issue-tracked
// as the blocking m8-exit-* ctests). The milestone-closing mirror of m2_exit_test.h / m5_exit_test.h /
// m7_exit_test.h: a tiny assertion harness (a CHECK macro + a failure counter + a uniform reporter) the
// m8-exit gates share, so each gate reads like the m6/m7-exit family (one assertion per claim,
// fail-closed, legible per OS leg). Header-only, test-only. The M8 build-pipeline exit criteria are
// CI-assertable headless: the a05 orchestrator + a06/a10/a11/a13 export adapters + the a08 verify-before-
// use trust chain + the a10/a13 signing hooks are all pure, in-memory, GPU-free surfaces.

#pragma once

#include <cstdio>

namespace context::tests::m8
{

inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

// Uniform terminal report: prints the failure count + returns 1 when anything failed, else prints the
// gate's success line + returns 0. Call it as `return context::tests::m8::report("m8-exit-N-...", "...");`.
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

} // namespace context::tests::m8

// One assertion. Records file/line/expression on failure (fail-closed, never aborts — every claim runs
// so a whole gate's failures surface in one pass), mirroring the m6/m7-exit family's CHECK.
#define CHECK(cond)                                                                                    \
    do                                                                                                 \
    {                                                                                                  \
        if (!(cond))                                                                                   \
            ::context::tests::m8::fail(__FILE__, __LINE__, #cond);                                     \
    } while (false)
