// The L-47 profiling snapshot (a15, R-OBS-002) — a flattened, presentation-ready value the CLI
// envelope serializes and a HUD renders. Pure values (no channel/JS types) so a consumer can read a
// profiling result without linking the JS backend or the live channels: `context profile session`
// builds one from the live SpanChannel + GC channel; a HUD model is derived from one. The GC block
// is a value MIRROR of the GcPauseChannel aggregates (with an availability flag), so the snapshot
// stays JS-seam-free while still carrying GC-pause attribution when the VM was present.

#pragma once

#include "context/runtime/profile/span_channel.h"

#include <cstdint>
#include <string>
#include <vector>

namespace context::runtime::profile
{

// Per-lane rollup: how much wall time C++ / TS / WASM systems consumed over the run.
struct LaneCounters
{
    Lane lane = Lane::Native;
    std::uint64_t span_count = 0;
    double total_ms = 0.0;
};

// One system's aggregate cost, name + lane resolved (a flattened SystemSpanAggregate).
struct SystemStat
{
    std::string name;
    Lane lane = Lane::Native;
    std::uint64_t call_count = 0;
    double total_ms = 0.0;
    double max_ms = 0.0;
};

// The GC-pause channel as a snapshot value (R-SIM-008 attribution folded into the SAME surface).
// `available == false` means this build carried the stub JS backend (no VM), so the run reports
// spans/counters only — every numeric field stays zero and `reason` names the absence.
struct GcSummary
{
    bool available = false;
    std::string reason;
    std::uint64_t pause_count = 0;
    std::uint64_t in_window_count = 0;
    double total_pause_ms = 0.0;
    double max_pause_ms = 0.0;
    double max_mid_tick_pause_ms = 0.0; // worst pause OUTSIDE a window — what R-SIM-008 polices
    std::uint64_t dropped = 0;
    double budget_ms = 0.0;
    bool within_budget = true;
};

// The whole profiling result: counters + per-system + per-lane spans + the GC channel.
struct ProfileSnapshot
{
    std::uint64_t tick_count = 0;
    std::uint64_t tick_hz = 0;
    std::uint64_t system_count = 0;
    double total_cpu_ms = 0.0; // sum over every recorded span (all lanes)
    std::vector<SystemStat> systems;
    std::vector<LaneCounters> lanes; // only lanes that actually ran, in Native/Script/Wasm order
    GcSummary gc;
};

// Build the counters + per-system + per-lane portions of a snapshot from a live SpanChannel. The
// caller fills `tick_count` / `tick_hz` (from the session) and `gc` (from the GC channel, or the
// stub-absence marker) — those are not derivable from spans alone.
[[nodiscard]] ProfileSnapshot build_snapshot(const SpanChannel& spans, std::uint64_t tick_count,
                                             std::uint64_t tick_hz);

} // namespace context::runtime::profile
