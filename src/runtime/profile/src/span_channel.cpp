// The L-47 per-system span channel implementation (see span_channel.h).

#include "context/runtime/profile/span_channel.h"

#include <utility>

namespace context::runtime::profile
{

const char* lane_name(Lane lane) noexcept
{
    switch (lane)
    {
    case Lane::Native:
        return "native";
    case Lane::Script:
        return "script";
    case Lane::Wasm:
        return "wasm";
    }
    return "unknown";
}

SpanChannel::SpanChannel(std::size_t sample_capacity) : capacity_(sample_capacity)
{
    samples_.reserve(capacity_);
}

std::uint32_t SpanChannel::register_system(std::string name, Lane lane)
{
    const std::uint32_t index = static_cast<std::uint32_t>(systems_.size());
    systems_.push_back(SystemDescriptor{std::move(name), lane});
    aggregates_.push_back(SystemSpanAggregate{});
    return index;
}

bool SpanChannel::record(std::uint64_t tick, std::uint32_t system, double duration_ms)
{
    if (system >= systems_.size())
        return false;

    // Aggregates are exact over EVERY span, even those the retained list truncates.
    SystemSpanAggregate& agg = aggregates_[system];
    ++agg.call_count;
    agg.total_ms += duration_ms;
    if (duration_ms > agg.max_ms)
        agg.max_ms = duration_ms;

    ++span_count_;
    total_ms_ += duration_ms;

    if (samples_.size() >= capacity_)
    {
        overflowed_ = true;
        return false;
    }
    samples_.push_back(SystemSpan{tick, system, duration_ms});
    return true;
}

void SpanChannel::clear()
{
    // Keep the descriptor table + reserved capacity; reset only the recorded data. A profiling run
    // registers its systems once, then clear()s any warm-up spans before the measured window.
    for (SystemSpanAggregate& agg : aggregates_)
        agg = SystemSpanAggregate{};
    samples_.clear();
    span_count_ = 0;
    total_ms_ = 0.0;
    overflowed_ = false;
}

} // namespace context::runtime::profile
