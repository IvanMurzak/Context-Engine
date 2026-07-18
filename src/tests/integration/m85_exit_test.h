// Shared harness for the M8.5 EXIT GATE integration tests (ROADMAP §1-M8.5 exit / a23-m85-exit; issue-
// tracked as the blocking m85-exit-* ctests). The wedge-hardening milestone-closing mirror of
// m2_exit_test.h / m5_exit_test.h / m7_exit_test.h / m8_exit_test.h: a tiny assertion harness (a CHECK
// macro + a failure counter + a uniform reporter) the two NEW m85-exit gates (the a15 profiling-JSON
// e2e + the a21 density-commitment audit) share, so each gate reads like the m6/m7/m8-exit family (one
// assertion per claim, fail-closed, legible per OS leg). Header-only, test-only. The other four M8.5
// exit clauses ride the landed a16/a17/a18/a19/a20 machinery via ctest ALIASES (the m8-exit-3/-4a/-4b
// alias precedent — same executable, a milestone ctest name) and need no harness.

#pragma once

#include <cstdio>

namespace context::tests::m85
{

inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

// Uniform terminal report: prints the failure count + returns 1 when anything failed, else prints the
// gate's success line + returns 0. Call it as `return context::tests::m85::report("m85-exit-N-...", "...");`.
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

} // namespace context::tests::m85

// One assertion. Records file/line/expression on failure (fail-closed, never aborts — every claim runs
// so a whole gate's failures surface in one pass), mirroring the m6/m7/m8-exit family's CHECK.
#define CHECK(cond)                                                                                    \
    do                                                                                                 \
    {                                                                                                  \
        if (!(cond))                                                                                   \
            ::context::tests::m85::fail(__FILE__, __LINE__, #cond);                                    \
    } while (false)
