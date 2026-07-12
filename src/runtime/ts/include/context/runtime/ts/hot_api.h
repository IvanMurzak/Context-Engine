// The engine-provided pooled / no-allocation math + query API for hot TS systems (M6 X1 — the
// query half of R-SIM-008, on the F0b simmath math core + the R-LANG-009 zero-copy view protocol).
//
// R-SIM-008's first lever is that steady-state gameplay should allocate LITTLE OR NOTHING per
// frame, so allocation-triggered mid-tick GC pauses become rare and collection happens in the
// scheduled inter-tick window instead. This module ships one engine-provided JS source
// (`hot_api_js()`, installed once as `globalThis.ctx`) whose every API is allocation-free at
// steady state:
//
//   ctx.pool   — preallocated Float64Array vector registers (v2/v3/v4), handed out by rewinding a
//                cursor (`reset()` per tick/system); the SAME objects are reused forever. On
//                exhaustion it falls back to allocating (correctness first) and counts the miss
//                in `ctx.pool.misses` so a test/profiler can assert steady-state zero.
//   ctx.math   — in-place vector math over caller-provided Float64Arrays (v2/v3 add/sub/scale/
//                dot/cross/len2/...): every function writes `out` and returns it — no temporaries.
//                Presentation-tier math (floats are OFF the deterministic sim path).
//   ctx.fixed  — the Q16.16 fixed-point scalar mirror of packages/simmath fixed.h for SIM-path
//                math in TS: raw int values in Numbers, |raw| <= 2^31 (the simmath domain), with
//                mul (floor, exact via hi/lo split) and div/fromRatio (C++ truncate-toward-zero,
//                exact via remainder correction) matching the C++ core BIT-FOR-BIT inside the
//                domain — proven against the real C++ simmath in test_hot_api.cpp.
//   ctx.query  — pooled strided row-cursors over the positional zero-copy views a system executor
//                receives from runSystemView (R-LANG-009): `ctx.query.reset()` at executor entry,
//                `ctx.query.open(view, stride)` per view — cursor objects are reused across
//                invocations (`ctx.query.misses` counts exhaustion fallbacks). Detach-safe: a
//                cursor never outlives its invocation's views by contract (reset() drops the
//                references; the R-LANG-009 hard gate has already neutered them).
//
// Install is idempotent (`ctx.__hotApi` versioned marker). This header is deliberately interface
// -only over the STL-only JsEngine seam — including it introduces no V8 (or context_js archive)
// link dependency; only callers that actually hold a JsEngine link the backend.

#pragma once

#include "context/runtime/js/js_engine.h"

#include <string_view>

namespace context::runtime::ts
{

// The engine-provided hot-API JS module source (plain ES5-compatible JS — evaluable directly by
// the V8 host with no toolchain pass, and a valid TS subset for authored-bundle inclusion).
[[nodiscard]] std::string_view hot_api_js();

// Install the hot API into `engine`'s global scope as `globalThis.ctx`. Idempotent — a second
// install on the same engine is a no-op. Returns false + fills `err` on an eval failure.
inline bool install_hot_api(js::JsEngine& engine, std::string& err)
{
    return engine.eval(hot_api_js(), nullptr, err);
}

} // namespace context::runtime::ts
