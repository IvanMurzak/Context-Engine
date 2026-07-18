// R-QA-013 unit test for the L-47 Tracy/Perfetto trace export (trace_export.h): the Chrome Trace
// Event Format is well-formed, carries a complete ("ph":"X") event per span with the right lane
// category, folds in the GC channel when present, labels each lane track (thread_name metadata),
// and reports the exact event count (happy/edge). The spans-only (no GC) path is covered too — the
// stub-JS-backend case. Engine-free: the channels are driven with synthetic records, so this is a
// LOCAL gate on every toolchain.

#include "context/runtime/profile/gc_channel.h"
#include "context/runtime/profile/span_channel.h"
#include "context/runtime/profile/trace_export.h"

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

namespace
{
int g_failures = 0;

void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}
} // namespace

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            fail(__FILE__, __LINE__, #cond);                                                       \
    } while (false)

namespace profile = context::runtime::profile;
using profile::Lane;

int main()
{
    // Build a small multi-lane channel: two native systems + one script system across two ticks.
    profile::SpanChannel spans;
    const std::uint32_t motion = spans.register_system("motion", Lane::Native);
    const std::uint32_t js = spans.register_system("js.gameplay", Lane::Script);
    spans.record(0, motion, 1.0);
    spans.record(0, js, 4.0);
    spans.record(1, motion, 2.0);
    spans.record(1, js, 3.0);

    // A GC channel with two attributed pauses (one in-window, one mid-tick).
    profile::GcPauseChannel gc;
    {
        profile::GcPauseSample a;
        a.tick = 0;
        a.duration_ms = 0.5;
        a.in_window = true;
        profile::GcPauseSample b;
        b.tick = 1;
        b.duration_ms = 0.7;
        b.in_window = false;
        gc.record(a);
        gc.record(b);
    }

    // --- with GC: every span + every gc pause becomes a trace event -----------------------------
    {
        std::ostringstream out;
        const std::uint64_t events = profile::write_chrome_trace(spans, &gc, /*tick_hz=*/60, out);
        const std::string doc = out.str();
        CHECK(events == 6); // 4 spans + 2 gc pauses
        CHECK(contains(doc, "\"traceEvents\":["));
        CHECK(contains(doc, "\"displayTimeUnit\":\"ms\""));
        CHECK(contains(doc, "\"name\":\"motion\""));
        CHECK(contains(doc, "\"name\":\"js.gameplay\""));
        CHECK(contains(doc, "\"cat\":\"native\""));
        CHECK(contains(doc, "\"cat\":\"script\""));
        CHECK(contains(doc, "\"ph\":\"X\""));
        CHECK(contains(doc, "\"gc.window\""));   // in-window pause
        CHECK(contains(doc, "\"gc.mid-tick\"")); // mid-tick pause
        CHECK(contains(doc, "\"cat\":\"js-gc\""));
        // Lane + gc tracks are labelled so Tracy/Perfetto name the rows.
        CHECK(contains(doc, "\"name\":\"thread_name\""));
        CHECK(contains(doc, "\"name\":\"native\""));
        CHECK(contains(doc, "\"name\":\"script\""));
        CHECK(contains(doc, "\"name\":\"js-gc\""));
        // Well-formed object boundaries.
        CHECK(!doc.empty() && doc.front() == '{' && doc.back() == '}');
    }

    // --- without GC (stub-JS-backend path): spans only, no gc track -----------------------------
    {
        std::ostringstream out;
        const std::uint64_t events = profile::write_chrome_trace(spans, nullptr, /*tick_hz=*/60, out);
        const std::string doc = out.str();
        CHECK(events == 4); // spans only
        CHECK(contains(doc, "\"name\":\"motion\""));
        CHECK(!contains(doc, "js-gc")); // no GC track at all
    }

    // --- tick_hz == 0 falls back to 60 Hz without dividing by zero ------------------------------
    {
        std::ostringstream out;
        const std::uint64_t events = profile::write_chrome_trace(spans, nullptr, /*tick_hz=*/0, out);
        CHECK(events == 4);
        CHECK(!out.str().empty());
    }

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d CHECK(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
