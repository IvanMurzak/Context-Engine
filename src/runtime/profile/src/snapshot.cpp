// The L-47 profiling snapshot builder (see snapshot.h).

#include "context/runtime/profile/snapshot.h"

#include <array>

namespace context::runtime::profile
{

ProfileSnapshot build_snapshot(const SpanChannel& spans, std::uint64_t tick_count,
                               std::uint64_t tick_hz)
{
    ProfileSnapshot snap;
    snap.tick_count = tick_count;
    snap.tick_hz = tick_hz;
    snap.system_count = static_cast<std::uint64_t>(spans.systems().size());
    snap.total_cpu_ms = spans.total_ms();

    // Per-system stats, in registration (scheduler) order.
    const std::vector<SystemDescriptor>& descriptors = spans.systems();
    const std::vector<SystemSpanAggregate>& aggregates = spans.aggregates();
    snap.systems.reserve(descriptors.size());

    // Per-lane rollup accumulators, indexed by the Lane enum value.
    std::array<LaneCounters, 3> lane_acc{};
    lane_acc[0].lane = Lane::Native;
    lane_acc[1].lane = Lane::Script;
    lane_acc[2].lane = Lane::Wasm;

    for (std::size_t i = 0; i < descriptors.size(); ++i)
    {
        const SystemDescriptor& d = descriptors[i];
        const SystemSpanAggregate& a = aggregates[i];
        snap.systems.push_back(
            SystemStat{d.name, d.lane, a.call_count, a.total_ms, a.max_ms});

        const std::size_t li = static_cast<std::size_t>(d.lane);
        if (li < lane_acc.size())
        {
            lane_acc[li].span_count += a.call_count;
            lane_acc[li].total_ms += a.total_ms;
        }
    }

    // Emit only lanes that actually ran, in Native/Script/Wasm order (a lane with no spans is
    // omitted rather than reported as a zero row — the taxonomy supports all three, the run
    // exercised a subset).
    for (const LaneCounters& lc : lane_acc)
        if (lc.span_count > 0)
            snap.lanes.push_back(lc);

    return snap;
}

} // namespace context::runtime::profile
