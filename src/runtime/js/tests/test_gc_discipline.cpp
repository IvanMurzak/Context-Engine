// R-QA-013 test for the R-SIM-008 GC-discipline seam on the V8 backend (M6 X1): the scheduled
// inter-tick GC window (force + growth-trigger policies), the allocation-free pause ring +
// in-window attribution, heap stats, and the invalid-options failure path. CI-only for its V8
// dependency path (the CONTEXT_JS_HAS_V8 split test_js_engine.cpp established); the local stub
// toolchain asserts the backend correctly reports unavailable so `ctest --preset dev` stays green.

#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>

#include "context/runtime/js/js_host.h"

namespace
{
int g_failures = 0;

void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            fail(__FILE__, __LINE__, #cond);                                                       \
    } while (false)

namespace cjs = context::runtime::js;

#ifdef CONTEXT_JS_HAS_V8

namespace
{
// Churn the JS heap: build + discard a pile of short-lived objects.
void churn(cjs::JsEngine& engine, int objects)
{
    char code[128];
    std::snprintf(code, sizeof(code),
                  "(function () { var a = []; for (var i = 0; i < %d; i++) { a.push({x: i}); } "
                  "return a.length; })()",
                  objects);
    std::string err;
    double n = 0.0;
    CHECK(engine.eval(code, &n, err));
    CHECK(n == static_cast<double>(objects));
}

// Drain everything pending, counting records and in-window records.
void drainAll(cjs::JsEngine& engine, std::size_t& total, std::size_t& inWindow)
{
    cjs::GcPause buffer[16]; // deliberately small: exercises the multi-round drain loop
    total = 0;
    inWindow = 0;
    for (;;)
    {
        const std::size_t n = engine.drainGcPauses(buffer, 16);
        if (n == 0)
            break;
        for (std::size_t i = 0; i < n; ++i)
        {
            CHECK(buffer[i].duration_ms >= 0.0);
            CHECK(buffer[i].kind != 0); // V8 always reports a GCType bit
            ++total;
            if (buffer[i].in_window)
                ++inWindow;
        }
    }
}

void test_invalid_budget(cjs::JsEngine& engine)
{
    cjs::GcWindowOptions options;
    cjs::GcWindowResult result;
    std::string err;

    options.budget_ms = 0.0;
    CHECK(!engine.gcWindow(options, result, err));
    CHECK(!err.empty());

    err.clear();
    options.budget_ms = -1.0;
    CHECK(!engine.gcWindow(options, result, err));
    CHECK(!err.empty());

    err.clear();
    options.budget_ms = std::numeric_limits<double>::quiet_NaN();
    CHECK(!engine.gcWindow(options, result, err));
    CHECK(!err.empty());

    err.clear();
    options.budget_ms = std::numeric_limits<double>::infinity();
    CHECK(!engine.gcWindow(options, result, err));
    CHECK(!err.empty());
}

void test_forced_window_collects_and_attributes(cjs::JsEngine& engine)
{
    // Discard any pauses earlier tests provoked so the attribution counts below are this test's.
    std::size_t total = 0;
    std::size_t inWindow = 0;
    drainAll(engine, total, inWindow);

    churn(engine, 20000);

    cjs::GcWindowOptions options;
    options.budget_ms = 8.0;
    options.force_collect = true;
    cjs::GcWindowResult result;
    std::string err;
    CHECK(engine.gcWindow(options, result, err));
    CHECK(result.collected);
    CHECK(result.window_ms >= 0.0);
    CHECK(result.heap_used_before > 0);
    CHECK(result.heap_used_after > 0);

    drainAll(engine, total, inWindow);
    CHECK(total >= 1);    // the forced full collection produced at least one bracket
    CHECK(inWindow >= 1); // ...attributed to the window
    CHECK(engine.drainGcPauses(nullptr, 0) == 0); // drained dry; a zero-cap drain is a no-op
}

void test_trigger_policy(cjs::JsEngine& engine)
{
    std::string err;
    cjs::GcWindowResult result;

    // An absurdly high growth trigger never fires (and force is off) — the window is a pump-only
    // service point.
    cjs::GcWindowOptions quiet;
    quiet.budget_ms = 2.0;
    quiet.trigger_bytes = std::uint64_t{1} << 40;
    CHECK(engine.gcWindow(quiet, result, err));
    CHECK(!result.collected);

    // Churn well past a small trigger, and the next window collects.
    churn(engine, 50000);
    cjs::GcWindowOptions eager;
    eager.budget_ms = 8.0;
    eager.trigger_bytes = 1;
    CHECK(engine.gcWindow(eager, result, err));
    CHECK(result.collected);

    // Immediately after a collection reset the baseline, a large trigger holds again.
    CHECK(engine.gcWindow(quiet, result, err));
    CHECK(!result.collected);
}

void test_heap_stats(cjs::JsEngine& engine)
{
    cjs::GcHeapStats stats;
    std::string err;
    CHECK(engine.gcHeapStats(stats, err));
    CHECK(stats.used_bytes > 0);
    CHECK(stats.total_bytes >= stats.used_bytes);
}
} // namespace

int main()
{
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (engine == nullptr)
    {
        std::fprintf(stderr, "createV8Engine failed: %s\n", err.c_str());
        return 1;
    }

    test_invalid_budget(*engine);
    test_forced_window_collects_and_attributes(*engine);
    test_trigger_policy(*engine);
    test_heap_stats(*engine);
    CHECK(engine->gcPausesDropped() == 0); // the ring never overflowed under normal drainage

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("gc_discipline (V8): all checks passed\n");
    return 0;
}

#else // !CONTEXT_JS_HAS_V8 — the local stub toolchain (profile setup.md/test.md carve-out)

int main()
{
    std::string err;
    CHECK(!cjs::v8BackendAvailable());
    CHECK(cjs::createV8Engine(err) == nullptr);
    CHECK(!err.empty());

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("gc_discipline (stub): V8 battery runs on the CI legs\n");
    return 0;
}

#endif
