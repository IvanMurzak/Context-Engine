// M6 exit criterion 2 — `m6-exit-2-gc-budget` (design §M6-EXIT, issue #197; R-SIM-008 vs
// R-LANG-012, L-47): over a SUSTAINED run of the 2D game's authored TypeScript gameplay
// (samples/platformer-2d/scripts/movement.ts — transpiled + bundled by the REAL esbuild toolchain
// and executed in the REAL V8 host), the X1 GC-pause profiler channel shows EVERY JS-tier GC pause
// inside the R-SIM-008 inter-tick budget.
//
// RIDES the landed X1 machinery (PR #189): the JsEngine GC pull seam (gcWindow / drainGcPauses),
// the GcPauseChannel aggregation + fail-closed within_budget verdict, and the pooled/no-allocation
// hot API (`globalThis.ctx`). The driver mirrors the shipped shape: run the TS control system once
// per fixed tick (the JS lane), then service the scheduled inter-tick GC window (the R-HEAD-002
// tick-boundary service point) and drain the pauses into the channel attributed to that tick. The
// budget is the R-SIM-008 definition the `context profile gc` command ships: a quarter of the
// fixed 60 Hz timestep.
//
// CI-only for its V8 dependency path (the rusty_v8 prebuilt links only on the 3-OS CI build legs;
// the local Strawberry-GCC dev gate builds the js STUB) — mirrors test_ts_in_v8.cpp's split, but
// branched at RUNTIME via v8BackendAvailable(): on the stub toolchain the toolchain half still
// proves the authored movement.ts BUNDLES, and the V8 battery is asserted unavailable, so the
// local `ctest --preset dev` stays green while the CI legs run the real sustained battery.

#include "context/runtime/js/js_host.h"
#include "context/runtime/profile/gc_channel.h"
#include "context/runtime/ts/hot_api.h"
#include "context/runtime/ts/ts_toolchain.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#ifndef CONTEXT_ESBUILD_PATH
#error "CONTEXT_ESBUILD_PATH must be defined by CMake (the staged esbuild binary path)"
#endif
#ifndef CONTEXT_SAMPLES_DIR
#error "CONTEXT_SAMPLES_DIR (path to the samples/ corpus root) must be defined by the build."
#endif

namespace cjs = context::runtime::js;
namespace cts = context::runtime::ts;
namespace profile = context::runtime::profile;
namespace fs = std::filesystem;

namespace
{
int g_failures = 0;
void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            fail(__FILE__, __LINE__, #cond);                                                       \
    } while (false)

// The authored subject: the platformer's REAL per-tick TS control system.
const std::string kMovementTs =
    std::string(CONTEXT_SAMPLES_DIR) + "/platformer-2d/scripts/movement.ts";

// movement.ts tuning mirrors (Q16.16 raw) — the correctness anchors that prove the TS genuinely ran.
constexpr double kMaxRunSpeedRaw = 4.0 * 65536.0; // MAX_RUN_SPEED
constexpr double kRunAccelRaw = 65536.0 / 8.0;    // RUN_ACCEL
constexpr double kJumpVelocityRaw = 6.0 * 65536.0; // JUMP_VELOCITY

// The R-SIM-008 inter-tick budget, exactly as `context profile gc` defines it: a quarter of the
// fixed timestep at the game's 60 Hz tick rate.
constexpr double kBudgetMs = 0.25 * (1000.0 / 60.0);

// The budget actually ENFORCED by this gate, and the sanitizer-overhead widen it applies.
//
// kBudgetMs is a REAL wall-clock ceiling, so it can only be measured honestly on an UNINSTRUMENTED
// build. BOTH CI sanitizer legs inflate the very quantity it bounds — measured on run 29987903124,
// one commit, bit-identical workload (ticks=600 pauses=10 inWindow=10 dropped=0):
//
//   build (ubuntu/macos/windows), no instrumentation .. enforces 4.167 ms, green
//   sanitize (TSan, ubuntu) .......................... maxPauseMs = 114.592 (enforced 416.667, ok)
//   sanitize (ASan+UBSan, ubuntu) ................... maxPauseMs =   4.473 (enforced 4.167, RED)
//
// Only TSan was widened when this gate landed, because the ASan+UBSan leg then measured
// maxPauseMs=0.991 and appeared to have 4x headroom. It does not: across the thirteen consecutive
// ASan+UBSan runs of that same workload spanning jobs 89084381975 … 89143913144, maxPauseMs ranged
// 1.056 … 4.473 ms — a 4.2x load-driven spread whose tail crosses the 4.167 ms ceiling, so the leg
// reds intermittently with no engine regression (issue #335). ASan+UBSan is therefore widened too.
// The blocking exit gate ("M6 exit gate" CI step, `ctest --preset dev`, on all three `build` legs)
// sets NEITHER define and always enforces the real, unwidened kBudgetMs — that is where R-SIM-008
// is actually verified; on an instrumented leg this gate proves the machinery still runs and drops
// nothing, not a real-time property the instrumentation makes unmeasurable.
#if defined(CONTEXT_TSAN_BUILD)
constexpr double kSanitizerBudgetScale = 100.0; // 3.6x margin over the observed 114.592 ms
#elif defined(CONTEXT_ASAN_BUILD)
constexpr double kSanitizerBudgetScale = 10.0; // 9.3x margin over the observed 4.473 ms
#else
constexpr double kSanitizerBudgetScale = 1.0; // uninstrumented: enforce the REAL budget
#endif
constexpr double kEnforcedBudgetMs = kBudgetMs * kSanitizerBudgetScale;
static_assert(kSanitizerBudgetScale >= 1.0, "the widen must never TIGHTEN the real budget");

// Regression guard for issue #335. The defect was a BUILD-WIRING gap, not a wrong constant: the
// CMake `if(CONTEXT_TSAN)` block plumbed CONTEXT_TSAN_BUILD while its sibling `if(CONTEXT_SANITIZE)`
// block plumbed no ASan counterpart, so the ASan+UBSan leg silently enforced a real-time budget
// against an instrumented measurement. Ask the COMPILER whether a sanitizer is active — independent
// of the CMake defines, so the two cannot drift — and fail the build when the wiring did not widen
// for it. A future preset that instruments this TU without plumbing its define therefore cannot
// reach CI as an intermittent red. (Same detection idiom as integration_test.h's
// kSanitizerTimeoutScale: GCC exposes __SANITIZE_*, Clang signals via __has_feature.)
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define CONTEXT_M6EXIT2_INSTRUMENTED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define CONTEXT_M6EXIT2_INSTRUMENTED 1
#endif
#endif
#if defined(CONTEXT_M6EXIT2_INSTRUMENTED)
static_assert(kSanitizerBudgetScale > 1.0,
              "this TU is sanitizer-instrumented but no wall-clock widen was plumbed for it: the "
              "enforced GC-pause ceiling would measure instrumentation overhead instead of "
              "R-SIM-008 (issue #335). Define CONTEXT_ASAN_BUILD / CONTEXT_TSAN_BUILD for the "
              "preset in src/tests/integration/CMakeLists.txt.");
#endif
// ...and the CONVERSE, because the guard above is worth exactly what its detection is worth. That
// detection is one-directional: if a compiler answered neither probe, CONTEXT_M6EXIT2_INSTRUMENTED
// would silently never be defined, the assert above would never be compiled at all, and the guard
// would sit INERT on precisely the legs it exists to protect — passing vacuously, with no signal.
// CONTEXT_SANITIZE / CONTEXT_TSAN add `-fsanitize=...` to every TU unconditionally
// (src/CMakeLists.txt), and they are the ONLY things that plumb the widen defines, so a plumbed
// define IMPLIES an instrumented TU. Assert that the compiler agrees: a detection that ever goes
// blind then reds the `sanitize` / `tsan` leg at BUILD time instead of quietly disarming the guard.
#if (defined(CONTEXT_ASAN_BUILD) || defined(CONTEXT_TSAN_BUILD))                                   \
    && !defined(CONTEXT_M6EXIT2_INSTRUMENTED)
#error "a sanitizer widen define is plumbed, but this compiler reports no active sanitizer: the "  \
       "issue-#335 wiring guard above cannot fire and is inert. Extend the detection (see "        \
       "integration_test.h's kSanitizerTimeoutScale) rather than removing this check."
#endif

constexpr int kWarmupTicks = 64;
constexpr int kSustainedTicks = 600;
constexpr int kForceCollectEvery = 120; // periodic scheduled full collections over the run

// The pooled, allocation-free-at-steady-state driver harness (the X1 discipline movement.ts is
// authored under): preallocated Int32Array velocity columns behind a PlayerView/InputStateView
// shaped adapter, and scalar-only per-tick entry points installed on globalThis. Written to a temp
// .ts entry that imports the REAL authored movement.ts, so esbuild bundles the genuine subject.
std::string harness_ts()
{
    std::string ts;
    ts += "import { playerControlSystem } from \"" + kMovementTs + "\";\n";
    ts += R"TS(
// Pooled driver state — allocated once at load, reused every tick (zero per-tick allocation).
const ROWS = 8;
const velX = new Int32Array(ROWS);
const velY = new Int32Array(ROWS);
let moveXHeld = 0;
let jumpHeld = 0;

const view = {
    count: ROWS,
    getVelX(row: number): number { return velX[row]; },
    setVelX(row: number, rawQ16: number): void { velX[row] = rawQ16; },
    getVelY(row: number): number { return velY[row]; },
    setVelY(row: number, rawQ16: number): void { velY[row] = rawQ16; },
};
const input = {
    moveX(): number { return moveXHeld; },
    jumpButton(): number { return jumpHeld; },
};

(globalThis as any).setInput = (mx: number, jump: number): number => {
    moveXHeld = mx;
    jumpHeld = jump;
    return 0;
};
(globalThis as any).tickOnce = (): number => {
    playerControlSystem(view, input);
    return 0;
};
(globalThis as any).readVelX = (): number => velX[0];
(globalThis as any).readVelY = (): number => velY[0];
)TS";
    return ts;
}

// Bundle the harness (and through it the real movement.ts) to a self-executing JS module.
std::string bundle_harness(std::string& err)
{
    const fs::path dir =
        fs::temp_directory_path() /
        ("context-m6exit2-" +
         std::to_string(static_cast<long long>(
             std::chrono::steady_clock::now().time_since_epoch().count())));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
    {
        err = "temp dir creation failed: " + ec.message();
        return {};
    }
    const fs::path entry = dir / "gc_budget_harness.ts";
    {
        std::ofstream out(entry, std::ios::binary);
        if (!out)
        {
            err = "cannot write " + entry.string();
            return {};
        }
        out << harness_ts();
    }

    std::unique_ptr<cts::TsToolchain> tc = cts::createEsbuildToolchain(CONTEXT_ESBUILD_PATH, err);
    std::string js;
    if (tc)
    {
        cts::TranspileOptions opts;
        opts.bundle = true;
        opts.format = cts::ModuleFormat::Iife;
        cts::TranspileResult r = tc->transpile(entry.string(), opts);
        if (r.ok)
            js = r.js;
        else
            err = r.diagnostics.empty() ? "bundle failed" : r.diagnostics.front().message;
    }
    fs::remove_all(dir, ec); // best-effort cleanup; never fails the gate
    return js;
}

// Drain any pending pause records (warm-up noise) so the measured run starts from a clean channel.
void drain_dry(cjs::JsEngine& engine)
{
    cjs::GcPause buffer[16];
    while (engine.drainGcPauses(buffer, 16) != 0)
    {
    }
}

// Call a zero/two-arg JS function and return its number result.
double call_js(cjs::JsEngine& engine, cjs::FunctionHandle fn, const double* args,
               std::size_t nargs)
{
    double out = 0.0;
    std::string err;
    const bool ok = engine.callFunction(fn, args, nargs, &out, err);
    CHECK(ok);
    if (!ok)
        std::fprintf(stderr, "callFunction failed: %s\n", err.c_str());
    return out;
}
} // namespace

int main()
{
    // --- the toolchain half (runs on EVERY toolchain): the authored TS bundles ------------------
    std::string err;
    const std::string js = bundle_harness(err);
    CHECK(!js.empty());
    if (js.empty())
    {
        std::fprintf(stderr, "bundle error: %s\n", err.c_str());
        return 1;
    }
    CHECK(js.find("playerControlSystem") != std::string::npos);

    if (!cjs::v8BackendAvailable())
    {
        // The local stub toolchain (profile setup.md/test.md carve-out): the V8 battery is a
        // CI-legs concern; assert the backend honestly reports unavailable and stay green.
        CHECK(cjs::createV8Engine(err) == nullptr);
        CHECK(!err.empty());
        if (g_failures != 0)
        {
            std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
            return 1;
        }
        std::printf("m6-exit-2-gc-budget (stub): movement.ts bundles; the sustained V8 GC-budget "
                    "battery runs on the CI legs\n");
        return 0;
    }

    // --- the V8 battery (the 3-OS CI build legs) -------------------------------------------------
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (engine == nullptr)
    {
        std::fprintf(stderr, "createV8Engine failed: %s\n", err.c_str());
        return 1;
    }

    // The engine-provided pooled hot API (movement.ts calls ctx.fixed.mul on the sim path).
    CHECK(cts::install_hot_api(*engine, err));
    CHECK(engine->eval(js, nullptr, err));
    if (g_failures != 0)
    {
        std::fprintf(stderr, "setup failed: %s\n", err.c_str());
        return 1;
    }

    const cjs::FunctionHandle set_input = engine->getFunction("setInput");
    const cjs::FunctionHandle tick_once = engine->getFunction("tickOnce");
    const cjs::FunctionHandle read_vel_x = engine->getFunction("readVelX");
    const cjs::FunctionHandle read_vel_y = engine->getFunction("readVelY");
    CHECK(set_input != cjs::kInvalidFunction);
    CHECK(tick_once != cjs::kInvalidFunction);
    CHECK(read_vel_x != cjs::kInvalidFunction);
    CHECK(read_vel_y != cjs::kInvalidFunction);

    // --- correctness anchor: the REAL authored control system computes its Q16-exact response ----
    const double run_right[2] = {1.0, 0.0};
    call_js(*engine, set_input, run_right, 2);
    for (int t = 0; t < kWarmupTicks; ++t)
        call_js(*engine, tick_once, nullptr, 0);
    CHECK(call_js(*engine, read_vel_x, nullptr, 0) == kMaxRunSpeedRaw); // saturated run speed

    const double run_and_jump[2] = {1.0, 1.0};
    call_js(*engine, set_input, run_and_jump, 2);
    call_js(*engine, tick_once, nullptr, 0); // grounded (vy == 0) -> instantaneous take-off
    CHECK(call_js(*engine, read_vel_y, nullptr, 0) == kJumpVelocityRaw);
    call_js(*engine, set_input, run_right, 2); // release jump for the sustained run

    // --- the sustained run: fixed ticks, scheduled inter-tick GC windows, attributed drains ------
    drain_dry(*engine); // discard load/warm-up brackets: the measured run starts clean

    profile::GcPauseChannel channel;
    for (int t = 0; t < kSustainedTicks; ++t)
    {
        call_js(*engine, tick_once, nullptr, 0); // the JS lane: the game's TS control system

        // The R-SIM-008 inter-tick service point: pump + (periodically) collect in the scheduled
        // window, then drain every pause record attributed to the tick that just completed.
        cjs::GcWindowOptions window;
        window.budget_ms = kBudgetMs;
        window.force_collect = (t % kForceCollectEvery) == (kForceCollectEvery - 1);
        cjs::GcWindowResult window_result;
        CHECK(engine->gcWindow(window, window_result, err));
        channel.drain(*engine, static_cast<std::uint64_t>(t)); // folds engine-side drops too
    }

    // The lane is still live at the end (the sustained run genuinely executed the TS every tick):
    // one decelerating tick moves velX by exactly RUN_ACCEL toward the reversed target.
    const double run_left[2] = {-1.0, 0.0};
    call_js(*engine, set_input, run_left, 2);
    call_js(*engine, tick_once, nullptr, 0);
    CHECK(call_js(*engine, read_vel_x, nullptr, 0) == kMaxRunSpeedRaw - kRunAccelRaw);

    // --- the R-LANG-012 verdict over the whole run ------------------------------------------------
    const profile::GcPauseAggregates& agg = channel.aggregates();
    std::printf("[m6-exit-2] ticks=%d pauses=%llu inWindow=%llu maxPauseMs=%.3f "
                "maxMidTickMs=%.3f dropped=%llu budgetMs=%.3f enforcedBudgetMs=%.3f\n",
                kSustainedTicks, static_cast<unsigned long long>(agg.pause_count),
                static_cast<unsigned long long>(agg.in_window_count), agg.max_pause_ms,
                agg.max_mid_tick_pause_ms, static_cast<unsigned long long>(agg.dropped),
                kBudgetMs, kEnforcedBudgetMs);

    CHECK(agg.pause_count >= 1);    // non-vacuous: the scheduled windows genuinely collected
    CHECK(agg.in_window_count >= 1); // ...attributed to the inter-tick window
    CHECK(engine->gcPausesDropped() == 0); // nothing was lost engine-side

    // THE exit assertion: every observed JS-tier GC pause fits the inter-tick budget, and no
    // record loss can be hiding a breach (within_budget is fail-closed on drops). Enforces
    // kEnforcedBudgetMs (== kBudgetMs on every uninstrumented leg — see its definition).
    CHECK(channel.within_budget(kEnforcedBudgetMs));

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("m6-exit-2-gc-budget (V8): every GC pause over the sustained movement.ts run fit "
                "the inter-tick budget\n");
    return 0;
}
