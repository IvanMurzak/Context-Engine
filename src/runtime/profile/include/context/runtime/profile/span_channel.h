// The L-47 per-system span channel (a15, R-OBS-002 / R-OBS-004). The SECOND profiler channel in
// this module (the first is the GC-pause channel, gc_channel.h): where that channel makes JS-tier
// GC pauses observable, this one makes the SCHEDULER's per-system work observable — one attributed
// span per system per tick, tagged with the authoring LANE (native C++ / TS script / WASM) so the
// polyglot cost is legible (R-OBS-004: "the scheduler emits per-system trace spans regardless of
// authoring language"). It is the substrate `context profile session`, the Tracy trace export, and
// the HUD all read. Deliberately STL-only + allocation-bounded (no JS seam) — a native system's
// span carries no JS type, so the render-side HUD can consume the derived snapshot without linking
// the JS backend.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace context::runtime::profile
{

// The authoring lane a system runs on (L-38: systems are scheduled per lane; TS occupies the single
// JS lane). The profiler attributes every span to one lane so C++/TS/WASM cost is separable
// (R-OBS-004). Kept a small fixed enum — a new tier extends it here and every consumer (snapshot,
// trace export, HUD) picks it up through lane_name().
enum class Lane : std::uint8_t
{
    Native = 0, // C++ / engine-native system
    Script = 1, // TypeScript on the embedded JS lane
    Wasm = 2,   // a WASM package system
};

// The stable lowercase wire name of a lane (used verbatim in the JSON envelope + the Tracy trace
// category). Total over the enum — an unknown value maps to "unknown" rather than reading past the
// table (fail-soft, never UB).
[[nodiscard]] const char* lane_name(Lane lane) noexcept;

// A registered system the channel attributes spans to: its display name + the lane it runs on.
struct SystemDescriptor
{
    std::string name;
    Lane lane = Lane::Native;
};

// One timed span: the system that ran (an index into the channel's descriptor table), the fixed
// tick it ran on, and how long it took. Spans reference the descriptor by index (not by string) so
// recording is allocation-free once the descriptor table is registered — the retained sample vector
// is reserved once and never grows.
struct SystemSpan
{
    std::uint64_t tick = 0;
    std::uint32_t system = 0; // index into systems()
    double duration_ms = 0.0;
};

// Running per-system aggregates over EVERY recorded span (aggregates never drop — only the retained
// sample list is capacity-bounded, mirroring GcPauseChannel). Index-aligned with systems().
struct SystemSpanAggregate
{
    std::uint64_t call_count = 0;
    double total_ms = 0.0;
    double max_ms = 0.0;
};

// The channel: a fixed descriptor table + a capacity-bounded sample list + never-dropping
// per-system aggregates. Steady-state record() is O(1) with zero allocation after the table is
// built and the sample vector reserved (when full, new samples update aggregates only and
// overflowed() reports the truncation — the aggregates stay exact).
class SpanChannel
{
public:
    explicit SpanChannel(std::size_t sample_capacity = 8192);

    // Register a system and return its stable descriptor index (call once per system at setup, in
    // scheduler order). Re-registering is not deduplicated — the caller owns uniqueness.
    std::uint32_t register_system(std::string name, Lane lane);

    // Record one span for an already-registered system index. Aggregates always update; the sample
    // list retains the span only while under capacity. Returns true when the span was retained.
    // An out-of-range system index is ignored (returns false) — fail-soft, never UB.
    bool record(std::uint64_t tick, std::uint32_t system, double duration_ms);

    [[nodiscard]] const std::vector<SystemDescriptor>& systems() const noexcept { return systems_; }
    [[nodiscard]] const std::vector<SystemSpan>& samples() const noexcept { return samples_; }
    [[nodiscard]] const std::vector<SystemSpanAggregate>& aggregates() const noexcept
    {
        return aggregates_;
    }
    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
    [[nodiscard]] std::size_t sample_capacity() const noexcept { return capacity_; }

    // Total recorded span count (== samples across all systems, including any truncated from the
    // retained list) and total wall time over every recorded span.
    [[nodiscard]] std::uint64_t span_count() const noexcept { return span_count_; }
    [[nodiscard]] double total_ms() const noexcept { return total_ms_; }

    void clear();

private:
    std::size_t capacity_;
    std::vector<SystemDescriptor> systems_;
    std::vector<SystemSpanAggregate> aggregates_;
    std::vector<SystemSpan> samples_;
    std::uint64_t span_count_ = 0;
    double total_ms_ = 0.0;
    bool overflowed_ = false;
};

} // namespace context::runtime::profile
