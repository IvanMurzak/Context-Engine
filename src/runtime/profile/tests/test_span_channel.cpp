// R-QA-013 unit test for the L-47 per-system span channel (span_channel.h) + the snapshot builder
// (snapshot.h): registration + recording + per-system aggregates + per-lane rollup (happy),
// capacity overflow + out-of-range record + clear (edge), and lane-name totality (failure/edge).
// Engine-free — spans are recorded directly, so this is a LOCAL gate on every toolchain.

#include "context/runtime/profile/snapshot.h"
#include "context/runtime/profile/span_channel.h"

#include <cstdint>
#include <cstdio>
#include <string>

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

namespace profile = context::runtime::profile;
using profile::Lane;

namespace
{
bool approx_eq(double a, double b) { return (a - b < 1e-9) && (b - a < 1e-9); }
} // namespace

int main()
{
    // --- lane_name is total (every enumerator + an out-of-range value) --------------------------
    {
        CHECK(std::string(profile::lane_name(Lane::Native)) == "native");
        CHECK(std::string(profile::lane_name(Lane::Script)) == "script");
        CHECK(std::string(profile::lane_name(Lane::Wasm)) == "wasm");
        CHECK(std::string(profile::lane_name(static_cast<Lane>(99))) == "unknown");
    }

    // --- registration + recording + aggregates (happy path) -------------------------------------
    {
        profile::SpanChannel ch;
        const std::uint32_t motion = ch.register_system("motion", Lane::Native);
        const std::uint32_t control = ch.register_system("control", Lane::Native);
        const std::uint32_t js = ch.register_system("js.gameplay", Lane::Script);
        CHECK(motion == 0);
        CHECK(control == 1);
        CHECK(js == 2);
        CHECK(ch.systems().size() == 3);
        CHECK(ch.systems()[js].lane == Lane::Script);

        // tick 0
        CHECK(ch.record(0, motion, 1.0));
        CHECK(ch.record(0, control, 2.0));
        CHECK(ch.record(0, js, 4.0));
        // tick 1
        CHECK(ch.record(1, motion, 3.0));
        CHECK(ch.record(1, js, 2.0));

        CHECK(ch.span_count() == 5);
        CHECK(approx_eq(ch.total_ms(), 12.0));
        CHECK(ch.samples().size() == 5);
        CHECK(!ch.overflowed());

        const auto& agg = ch.aggregates();
        CHECK(agg[motion].call_count == 2);
        CHECK(approx_eq(agg[motion].total_ms, 4.0));
        CHECK(approx_eq(agg[motion].max_ms, 3.0));
        CHECK(agg[js].call_count == 2);
        CHECK(approx_eq(agg[js].total_ms, 6.0));

        // --- snapshot: per-system + per-lane rollup ---------------------------------------------
        profile::ProfileSnapshot snap = profile::build_snapshot(ch, /*tick_count=*/2, /*tick_hz=*/60);
        CHECK(snap.tick_count == 2);
        CHECK(snap.tick_hz == 60);
        CHECK(snap.system_count == 3);
        CHECK(approx_eq(snap.total_cpu_ms, 12.0));
        CHECK(snap.systems.size() == 3);
        CHECK(snap.systems[0].name == "motion");
        CHECK(snap.systems[2].lane == Lane::Script);
        // Two lanes ran (native + script); wasm omitted (no spans).
        CHECK(snap.lanes.size() == 2);
        CHECK(snap.lanes[0].lane == Lane::Native);
        CHECK(snap.lanes[0].span_count == 3); // motion x2 + control x1
        CHECK(approx_eq(snap.lanes[0].total_ms, 6.0));
        CHECK(snap.lanes[1].lane == Lane::Script);
        CHECK(snap.lanes[1].span_count == 2);
        CHECK(approx_eq(snap.lanes[1].total_ms, 6.0));
    }

    // --- out-of-range system index is ignored (fail-soft, never UB) -----------------------------
    {
        profile::SpanChannel ch;
        ch.register_system("a", Lane::Native);
        CHECK(!ch.record(0, 5, 1.0)); // index 5 unregistered
        CHECK(ch.span_count() == 0);
    }

    // --- capacity overflow: aggregates stay exact, retained samples truncate --------------------
    {
        profile::SpanChannel ch(/*sample_capacity=*/2);
        const std::uint32_t s = ch.register_system("s", Lane::Native);
        CHECK(ch.record(0, s, 1.0));
        CHECK(ch.record(1, s, 1.0));
        CHECK(!ch.record(2, s, 1.0)); // over capacity -> not retained, but aggregated
        CHECK(ch.overflowed());
        CHECK(ch.samples().size() == 2);
        CHECK(ch.span_count() == 3);              // every span counted
        CHECK(ch.aggregates()[s].call_count == 3);
        CHECK(approx_eq(ch.total_ms(), 3.0));
    }

    // --- clear keeps the descriptor table but resets recorded data ------------------------------
    {
        profile::SpanChannel ch;
        const std::uint32_t s = ch.register_system("s", Lane::Native);
        ch.record(0, s, 5.0);
        ch.clear();
        CHECK(ch.systems().size() == 1); // descriptors retained
        CHECK(ch.span_count() == 0);
        CHECK(ch.samples().empty());
        CHECK(ch.aggregates()[s].call_count == 0);
        CHECK(!ch.overflowed());
    }

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d CHECK(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
