// The L-47 GC-pause profiler channel implementation (see gc_channel.h).

#include "context/runtime/profile/gc_channel.h"

namespace context::runtime::profile
{

namespace
{
// v8::GCType's incremental-marking bit (v8-callbacks.h: kGCTypeIncrementalMarking = 1 << 3). The
// channel treats the engine's `kind` as opaque bits except for this one exclusion; mirroring the
// constant here keeps the profile module free of any V8 header.
constexpr std::uint32_t kIncrementalMarkingBit = 1u << 3;
} // namespace

bool gc_kind_is_pause(std::uint32_t kind) noexcept
{
    // A bracket whose ONLY bit is incremental marking is a cycle span, not a stop-the-world
    // pause. kind == 0 (unknown backend) is conservatively counted as a pause — fail-closed for
    // the budget verdict.
    return kind != kIncrementalMarkingBit;
}

GcPauseChannel::GcPauseChannel(std::size_t sample_capacity)
    : capacity_(sample_capacity == 0 ? 1 : sample_capacity)
{
    samples_.reserve(capacity_);
}

bool GcPauseChannel::record(const GcPauseSample& sample)
{
    if (gc_kind_is_pause(sample.kind))
    {
        ++aggregates_.pause_count;
        aggregates_.total_pause_ms += sample.duration_ms;
        if (sample.duration_ms > aggregates_.max_pause_ms)
            aggregates_.max_pause_ms = sample.duration_ms;
        if (sample.in_window)
            ++aggregates_.in_window_count;
        else if (sample.duration_ms > aggregates_.max_mid_tick_pause_ms)
            aggregates_.max_mid_tick_pause_ms = sample.duration_ms;
    }
    if (samples_.size() >= capacity_)
    {
        overflowed_ = true;
        return false;
    }
    samples_.push_back(sample);
    return true;
}

std::size_t GcPauseChannel::drain(js::JsEngine& engine, std::uint64_t tick)
{
    // Fold the engine-side overflow delta first (fail-closed: a lost record fails within_budget).
    const std::uint64_t dropped_total = engine.gcPausesDropped();
    if (dropped_total > engine_dropped_seen_)
    {
        record_dropped(dropped_total - engine_dropped_seen_);
        engine_dropped_seen_ = dropped_total;
    }

    js::GcPause buffer[64];
    std::size_t drained = 0;
    for (;;)
    {
        const std::size_t n = engine.drainGcPauses(buffer, 64);
        if (n == 0)
            break;
        for (std::size_t i = 0; i < n; ++i)
        {
            GcPauseSample sample;
            sample.tick = tick;
            sample.duration_ms = buffer[i].duration_ms;
            sample.kind = buffer[i].kind;
            sample.in_window = buffer[i].in_window;
            record(sample);
        }
        drained += n;
    }
    return drained;
}

bool GcPauseChannel::within_budget(double budget_ms) const noexcept
{
    return aggregates_.dropped == 0 && aggregates_.max_pause_ms <= budget_ms;
}

void GcPauseChannel::clear()
{
    samples_.clear();
    aggregates_ = GcPauseAggregates{};
    overflowed_ = false;
}

} // namespace context::runtime::profile
