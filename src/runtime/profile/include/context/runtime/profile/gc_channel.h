// The L-47 GC-pause profiler channel (M6 X1, R-SIM-008 / R-OBS-002) — the FIRST profiler channel
// in-tree. It aggregates the JS engine's GC-pause records (drained between ticks from the
// JsEngine pull seam) into per-tick, attributed samples plus running aggregates, so a per-frame
// GC pause is observable and attributable against the R-LANG-012 frame budget: every sample says
// WHEN it happened (tick), HOW LONG it stopped the JS lane (duration), and WHERE it was scheduled
// (inside the R-SIM-008 inter-tick window, or mid-tick). L-47's fuller shape (per-system spans
// across C++/TS/WASM, Tracy/RenderDoc export, the HUD) grows around this channel later; this file
// deliberately stays a small, allocation-bounded, STL-only substrate.

#pragma once

#include "context/runtime/js/js_engine.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace context::runtime::profile
{

// One attributed GC-pause sample: the engine's GcPause plus the sim tick it was drained at.
struct GcPauseSample
{
    std::uint64_t tick = 0;     // the fixed tick whose boundary drained this record
    double duration_ms = 0.0;   // wall time of the GC bracket
    std::uint32_t kind = 0;     // VM GC-type bits (V8 GCType), verbatim from the engine
    bool in_window = false;     // true = scheduled inter-tick window; false = mid-tick
};

// Running aggregates over every RECORDED sample (aggregates never drop — only the retained
// sample list is capacity-bounded). Incremental-marking brackets (kind bit 8) span a whole
// incremental CYCLE, not a stop-the-world pause, so they are recorded as samples (kind-tagged)
// but EXCLUDED from the pause aggregates — including one would misreport a multi-tick cycle as
// a single giant pause and poison the budget verdict.
struct GcPauseAggregates
{
    std::uint64_t pause_count = 0;      // pause-kind samples observed
    std::uint64_t in_window_count = 0;  // ... of which were inside a scheduled window
    double total_pause_ms = 0.0;
    double max_pause_ms = 0.0;          // worst pause anywhere
    double max_mid_tick_pause_ms = 0.0; // worst pause OUTSIDE any window (what R-SIM-008 polices)
    std::uint64_t dropped = 0;          // engine-side ring drops folded in via record_dropped()
};

// The channel: a capacity-bounded sample list + never-dropping aggregates. Steady-state cost is
// O(1) per record with zero allocation after construction (the sample vector is reserved once;
// when full, new samples update aggregates only and `overflowed()` reports the truncation).
class GcPauseChannel
{
public:
    explicit GcPauseChannel(std::size_t sample_capacity = 4096);

    // Record one sample (aggregates always update; the sample list retains it only while under
    // capacity). Returns true when the sample was retained.
    bool record(const GcPauseSample& sample);

    // Fold engine-side ring drops (JsEngine::gcPausesDropped deltas) into the aggregates.
    void record_dropped(std::uint64_t dropped) { aggregates_.dropped += dropped; }

    // Drain every pending pause record from `engine` into this channel, attributed to `tick`.
    // Returns how many records were drained. The intended call site is the session's inter-tick
    // hook, right after JsEngine::gcWindow — so window pauses and the tick's mid-tick pauses all
    // land attributed to the tick that just completed.
    std::size_t drain(js::JsEngine& engine, std::uint64_t tick);

    [[nodiscard]] const std::vector<GcPauseSample>& samples() const noexcept { return samples_; }
    [[nodiscard]] const GcPauseAggregates& aggregates() const noexcept { return aggregates_; }
    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
    [[nodiscard]] std::size_t sample_capacity() const noexcept { return capacity_; }

    // The R-LANG-012 budget verdict: true when every observed pause (window or mid-tick) fits
    // inside `budget_ms` AND no engine-side record was lost (a dropped record could hide a
    // breach, so loss fails the verdict — fail-closed). This is the assertion the blocking
    // m6-exit-2-gc-budget gate makes over a sustained run.
    [[nodiscard]] bool within_budget(double budget_ms) const noexcept;

    void clear();

private:
    std::size_t capacity_;
    std::vector<GcPauseSample> samples_;
    GcPauseAggregates aggregates_;
    bool overflowed_ = false;
    // High-water mark of JsEngine::gcPausesDropped() already folded into aggregates_.dropped —
    // drain() records only the delta, so repeated drains never double-count engine-side drops.
    std::uint64_t engine_dropped_seen_ = 0;
};

// Whether a GC-type bit set counts toward PAUSE aggregates (everything but the incremental-marking
// cycle bracket; see GcPauseAggregates). Exposed for tests and future channel consumers.
[[nodiscard]] bool gc_kind_is_pause(std::uint32_t kind) noexcept;

} // namespace context::runtime::profile
