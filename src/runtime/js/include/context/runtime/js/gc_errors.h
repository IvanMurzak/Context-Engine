// The sim.gc.* error codes (M6 X1 — JS-tier GC discipline, R-SIM-008 / L-47). The same
// promote-a-local-string pattern as the sim packages' errors.h files: the OWNING module defines
// the code strings (consumers — the CLI profile command, tests — reference these constants, never
// re-spell the literals) and src/editor/contract/src/error_catalog.cpp REGISTERS them in the
// F0a-reserved sim.gc.* block (this module never links the contract layer). Appended within that
// block only — additive-only, protocolMajor stays 0.

#pragma once

namespace context::runtime::js
{

// A GC-discipline / GC-profiler operation needs the in-process JS VM, but this binary carries the
// stub backend (the rusty_v8 prebuilt cannot link on this toolchain). Internal-class fail-closed:
// nothing ran; the sim is unaffected (the JS tier is simply absent from this build).
inline constexpr const char* kGcUnavailableCode = "sim.gc.unavailable";

// A scheduled inter-tick GC window was refused: the requested pause budget was not a finite
// positive duration (validation-class; nothing was collected).
inline constexpr const char* kGcInvalidBudgetCode = "sim.gc.invalid_budget";

// The VM reported a failure while running a scheduled inter-tick GC window or a GC-profiler query
// (internal-class; the sim state is unaffected — GC touches the JS heap only).
inline constexpr const char* kGcWindowFailedCode = "sim.gc.window_failed";

} // namespace context::runtime::js
