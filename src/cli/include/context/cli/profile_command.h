// `context profile <verb>` — the L-47 profiler query CLI backend (M6 X1, R-SIM-008 / R-OBS-002).
//
// v1 carries ONE verb: `profile gc` — measure the JS-tier GC-pause profiler channel. It runs a
// self-contained synthetic workload in-process: a deterministic headless session (the demo
// scenario) stepped N fixed ticks, with per-system JS allocation churn injected through the
// read-only system observer (mid-tick, where gameplay JS would allocate) and the R-SIM-008
// scheduled inter-tick GC window run at every tick boundary. The envelope reports the GC-pause
// channel: per-tick attributed samples (in-window vs mid-tick), aggregates, the JS-heap gauge,
// and the R-LANG-012 budget verdict — L-47's "all profiling data CLI-queryable as JSON", v1.
//
// Needs the in-process JS VM: a stub-backend build (the local GCC gate) refuses fail-closed with
// sim.gc.unavailable (runtime/js gc_errors.h; registered in the catalog's sim.gc.* block).

#pragma once

#include "context/editor/contract/envelope.h"

#include <map>
#include <string>

namespace context::cli
{

// Dispatch `context profile <verb> [--flags]`. `verb` is `gc`; `flags` the parsed flags
// (--ticks / --budget-ms / --trigger-bytes / --churn).
[[nodiscard]] editor::contract::Envelope run_profile(const std::string& verb,
                                                     const std::map<std::string, std::string>& bound,
                                                     const std::map<std::string, std::string>& flags);

} // namespace context::cli
