// The L-47 Tracy/Perfetto trace export (a15, R-OBS-002) — "deep capture via export to world-class
// tools, don't rebuild them." Writes the per-system spans + GC pauses as the Chrome Trace Event
// Format ({"traceEvents":[...]}), the lingua-franca a Tracy capture imports via `tracy-import-chrome
// profile.json profile.tracy`, and which Perfetto and chrome://tracing open directly. So the engine
// exports to Tracy without linking it (no heavyweight profiler dependency; deny-by-default license
// gate untouched). Timestamps are a synthesized per-tick timeline: the span DURATIONS are the real
// measured wall times, and each tick occupies one fixed-timestep window (tick_hz), inside which a
// lane's spans are laid end-to-end — the honest reconstruction for a deterministic fixed-tick loop,
// which measures per-tick cost, not absolute wall-clock offsets.

#pragma once

#include "context/runtime/profile/gc_channel.h"
#include "context/runtime/profile/span_channel.h"

#include <cstdint>
#include <ostream>

namespace context::runtime::profile
{

// Write the Chrome Trace Event Format document for `spans` (+ the GC channel when non-null) to
// `out`. `tick_hz` sets the per-tick window width (defaults to 60 Hz when 0 is passed). Returns the
// number of trace events written (span events + gc events; excludes the thread-name metadata).
std::uint64_t write_chrome_trace(const SpanChannel& spans, const GcPauseChannel* gc,
                                 std::uint64_t tick_hz, std::ostream& out);

} // namespace context::runtime::profile
