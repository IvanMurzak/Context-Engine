// §2d spike benchmark harness. THROWAWAY spike code.
//
// One engine config per process (V8 flags are process-wide). Emits a single JSON object on
// stdout; run_bench.py drives fresh processes and aggregates.
//
//   context-spike-jsengine --engine=quickjs|v8 [--jitless] --bench=startup|memory|calls|zerocopy|compute|gc|all

#include "ctx/js_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

namespace ctx::spike {

#ifndef CTX_SPIKE_HAS_QUICKJS
std::unique_ptr<JsEngine> createQuickJsEngine(const EngineConfig&, std::string& err) {
    err = "QuickJS backend not built (CONTEXT_SPIKE_JSENGINE_QUICKJS=OFF)";
    return nullptr;
}
#endif
#ifndef CTX_SPIKE_HAS_V8
std::unique_ptr<JsEngine> createV8Engine(const EngineConfig&, std::string& err) {
    err = "V8 backend not built (CONTEXT_SPIKE_JSENGINE_V8=OFF)";
    return nullptr;
}
#endif

namespace {

using Clock = std::chrono::steady_clock;

double nowMs() {
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

double elapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Stat {
    double median = 0, min = 0, max = 0;
};

Stat stat(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    Stat s;
    s.min = v.front();
    s.max = v.back();
    s.median = v[v.size() / 2];
    if (v.size() % 2 == 0) s.median = (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2.0;
    return s;
}

// Grows n until one run of `run(n)` takes >= minMs, then returns 5 timed runs (ms each).
std::vector<double> calibrateAndRep(const std::function<void(long long)>& run, double minMs,
                                    long long startN, long long* outN) {
    long long n = startN;
    for (;;) {
        const auto t0 = Clock::now();
        run(n);
        const double ms = elapsedMs(t0);
        if (ms >= minMs || n > (1LL << 40)) break;
        n *= 2;
    }
    *outN = n;
    std::vector<double> reps;
    for (int i = 0; i < 5; i++) {
        const auto t0 = Clock::now();
        run(n);
        reps.push_back(elapsedMs(t0));
    }
    return reps;
}

std::size_t memPrivateBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return pmc.PrivateUsage;
    }
#endif
    return 0;
}

std::size_t memWorkingSetBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return pmc.WorkingSetSize;
    }
#endif
    return 0;
}

[[noreturn]] void die(const std::string& msg) {
    std::fprintf(stderr, "FATAL: %s\n", msg.c_str());
    std::exit(1);
}

void check(bool ok, const std::string& msg) {
    if (!ok) die(msg);
}

// ---- workloads --------------------------------------------------------------------------------

constexpr std::string_view kCallTargets = R"js(
function empty() {}
function add3(a, b, c) { return a + b + c; }
function loopNop(n) { for (let i = 0; i < n; i++); return n; }
function loopHostEmpty(n) { for (let i = 0; i < n; i++) hostEmpty(); return n; }
function loopHostAdd3(n) { let s = 0; for (let i = 0; i < n; i++) s = hostAdd3(s, 1.5, 2.5); return s; }
)js";

// Compute-heavy kernel: numeric core (mandelbrot) + object/property traffic (particle update).
// Deterministic — the driver cross-checks the checksum across engines.
constexpr std::string_view kComputeKernel = R"js(
function kernel() {
  let acc = 0;
  for (let py = 0; py < 96; py++) {
    const y0 = -1.3 + 2.6 * py / 96;
    for (let px = 0; px < 96; px++) {
      const x0 = -2.05 + 2.6 * px / 96;
      let x = 0, y = 0, it = 0;
      while (x * x + y * y <= 4 && it < 64) {
        const xt = x * x - y * y + x0;
        y = 2 * x * y + y0;
        x = xt;
        it++;
      }
      acc += it;
    }
  }
  const parts = [];
  for (let i = 0; i < 1000; i++) parts.push({ x: i * 0.5, y: i * 0.25, vx: 0.1, vy: -0.05 });
  for (let step = 0; step < 20; step++) {
    for (let i = 0; i < parts.length; i++) {
      const p = parts[i];
      p.x += p.vx;
      p.y += p.vy;
      if (p.x > 100) p.x -= 200;
      if (p.y < -100) p.y += 200;
    }
  }
  for (let i = 0; i < parts.length; i += 97) acc += parts[i].x;
  return acc;
}
function kernelReps(n) { let r = 0; for (let i = 0; i < n; i++) r = kernel(); return r; }
)js";

// GC churn: steady small-object allocation; samples hostNowMs() every 64 iterations and records
// the worst / p99 gap — a PROXY for GC pause (includes any engine hiccup, not only GC).
constexpr std::string_view kGcChurn = R"js(
function gcChurn(iters) {
  const keep = new Array(1024);
  const gaps = [];
  let prev = hostNowMs();
  let idx = 0;
  for (let i = 0; i < iters; i++) {
    keep[idx] = { a: i, b: 'k' + (i & 255), c: [i, i + 1] };
    idx = (idx + 1) & 1023;
    if ((i & 63) === 0) {
      const now = hostNowMs();
      gaps.push(now - prev);
      prev = now;
    }
  }
  gaps.sort((x, y) => x - y);
  globalThis.gcMaxMs = gaps[gaps.length - 1];
  globalThis.gcP99Ms = gaps[Math.floor((gaps.length - 1) * 0.99)];
  return gaps.length;
}
)js";

// ---- host functions ---------------------------------------------------------------------------

double hostEmpty(void*, const double*, std::size_t) { return 0; }
double hostAdd3(void*, const double* a, std::size_t n) { return n == 3 ? a[0] + a[1] + a[2] : -1; }
double hostNowMs(void*, const double*, std::size_t) { return nowMs(); }

// ---- JSON emission ----------------------------------------------------------------------------

struct Json {
    std::string out = "{";
    bool first = true;

    void key(const std::string& k) {
        if (!first) out += ",";
        first = false;
        out += "\"" + k + "\":";
    }
    void num(const std::string& k, double v) {
        key(k);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6g", v);
        out += buf;
    }
    void str(const std::string& k, const std::string& v) {
        key(k);
        out += "\"" + v + "\"";
    }
    void boolean(const std::string& k, bool v) {
        key(k);
        out += v ? "true" : "false";
    }
    void statObj(const std::string& k, const Stat& s, double scale = 1.0) {
        key(k);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "{\"median\":%.6g,\"min\":%.6g,\"max\":%.6g}",
                      s.median * scale, s.min * scale, s.max * scale);
        out += buf;
    }
    void raw(const std::string& k, const std::string& v) {
        key(k);
        out += v;
    }
    std::string finish() { return out + "}"; }
};

// ---- benches ----------------------------------------------------------------------------------

void benchCalls(JsEngine& e, Json& j) {
    std::string err;
    check(e.eval(kCallTargets, nullptr, err), "calls setup: " + err);
    check(e.bindHostFunction("hostEmpty", hostEmpty, nullptr), "bind hostEmpty");
    check(e.bindHostFunction("hostAdd3", hostAdd3, nullptr), "bind hostAdd3");
    // Re-eval loop bodies AFTER binding so host functions resolve.
    check(e.eval(kCallTargets, nullptr, err), "calls re-setup: " + err);

    const FunctionHandle fnEmpty = e.getFunction("empty");
    const FunctionHandle fnAdd3 = e.getFunction("add3");
    const FunctionHandle fnLoopNop = e.getFunction("loopNop");
    const FunctionHandle fnLoopHostEmpty = e.getFunction("loopHostEmpty");
    const FunctionHandle fnLoopHostAdd3 = e.getFunction("loopHostAdd3");
    check(fnEmpty && fnAdd3 && fnLoopNop && fnLoopHostEmpty && fnLoopHostAdd3,
          "function resolution failed");

    struct Case {
        const char* nameCallsPerSec;
        std::function<void(long long)> run;
    };
    std::string cerr2;
    const double a3[3] = {1.5, 2.5, 3.0};
    const Case cases[] = {
        {"host_to_js_empty",
         [&](long long n) {
             for (long long i = 0; i < n; i++) {
                 if (!e.callFunction(fnEmpty, nullptr, 0, nullptr, cerr2)) die(cerr2);
             }
         }},
        {"host_to_js_add3",
         [&](long long n) {
             double r = 0;
             for (long long i = 0; i < n; i++) {
                 if (!e.callFunction(fnAdd3, a3, 3, &r, cerr2)) die(cerr2);
             }
         }},
        {"js_loop_nop_baseline",
         [&](long long n) {
             const double arg = static_cast<double>(n);
             if (!e.callFunction(fnLoopNop, &arg, 1, nullptr, cerr2)) die(cerr2);
         }},
        {"js_to_host_empty",
         [&](long long n) {
             const double arg = static_cast<double>(n);
             if (!e.callFunction(fnLoopHostEmpty, &arg, 1, nullptr, cerr2)) die(cerr2);
         }},
        {"js_to_host_add3",
         [&](long long n) {
             const double arg = static_cast<double>(n);
             if (!e.callFunction(fnLoopHostAdd3, &arg, 1, nullptr, cerr2)) die(cerr2);
         }},
    };

    for (const Case& c : cases) {
        long long n = 0;
        std::vector<double> reps = calibrateAndRep(c.run, 100.0, 1 << 12, &n);
        // per-op nanoseconds and ops/sec from per-rep totals
        std::vector<double> nsPerOp, opsPerSec;
        for (double ms : reps) {
            nsPerOp.push_back(ms * 1e6 / static_cast<double>(n));
            opsPerSec.push_back(static_cast<double>(n) / (ms / 1000.0));
        }
        Json sub;
        sub.num("n_per_rep", static_cast<double>(n));
        sub.statObj("ns_per_op", stat(nsPerOp));
        sub.statObj("ops_per_sec", stat(opsPerSec));
        j.raw(c.nameCallsPerSec, sub.finish());
    }
}

// Runs the correctness + cost battery for one sharing shape. `data` points at the shared
// memory (host-owned for wrap mode, VM-owned for shared mode); `attach` produces a fresh
// ArrayBuffer "HB"/"HBX" over it.
void zeroCopyBattery(JsEngine& e, Json& j, double* data, std::size_t nDoubles,
                     const std::function<BufferHandle(std::string_view, std::string&)>& attach) {
    std::string err;
    for (std::size_t i = 0; i < nDoubles; i++) data[i] = static_cast<double>(i) * 0.5;

    // -- correctness: JS reads shared memory, JS writes visible to host, host writes visible to JS
    BufferHandle hb = attach("HB", err);
    check(hb != kInvalidBuffer, "attach: " + err);
    double sum = 0;
    check(e.eval("(function(){ const v = new Float64Array(HB); let s = 0;"
                 " for (let i = 0; i < v.length; i++) s += v[i]; v[0] = 1234.5; return s; })()",
                 &sum, err),
          "zerocopy read: " + err);
    double expected = 0;
    for (std::size_t i = 0; i < nDoubles; i++) expected += static_cast<double>(i) * 0.5;
    j.boolean("js_reads_shared_memory", sum == expected);
    j.boolean("js_write_visible_to_host", data[0] == 1234.5);
    data[1] = 777.25;
    double v1 = 0;
    check(e.eval("new Float64Array(HB)[1]", &v1, err), "zerocopy host-write: " + err);
    j.boolean("host_write_visible_to_js", v1 == 777.25);

    // -- retained-view death (R-LANG-009): a view taken before detach must die at detach
    double viewLen = 0;
    check(e.eval("globalThis.gView = new Float64Array(HB); gView.length", &viewLen, err),
          "retained view: " + err);
    check(viewLen == static_cast<double>(nDoubles), "retained view length wrong");
    check(e.detachHostBuffer(hb, err), "detach: " + err);
    double afterLen = -1, afterByteLen = -1, readBehavior = -1;
    check(e.eval("gView.length", &afterLen, err), "post-detach view length: " + err);
    check(e.eval("HB.byteLength", &afterByteLen, err), "post-detach byteLength: " + err);
    // Per spec a detached typed-array read yields undefined; a new view over a detached buffer throws.
    check(e.eval("(function(){ try { return gView[0] === undefined ? 2 : 3; }"
                 " catch (e) { return 1; } })()",
                 &readBehavior, err),
          "post-detach read: " + err);
    double newViewThrows = -1;
    check(e.eval("(function(){ try { new Float64Array(HB); return 0; }"
                 " catch (e) { return 1; } })()",
                 &newViewThrows, err),
          "post-detach new view: " + err);
    j.boolean("detach_kills_retained_view", afterLen == 0 && afterByteLen == 0);
    j.str("post_detach_read",
          readBehavior == 2 ? "undefined" : (readBehavior == 1 ? "throws" : "STALE-VALUE"));
    j.boolean("post_detach_new_view_throws", newViewThrows == 1);
    j.boolean("memory_untouched_after_detach", data[1] == 777.25 && data[2] == 1.0);

    // -- attach / detach cost, measured as separate phases (detach = the R-LANG-009 number)
    constexpr int kBatch = 8192;
    std::vector<BufferHandle> handles(kBatch);
    std::vector<double> attachNs, detachNs, cycleNs;
    for (int rep = 0; rep < 5; rep++) {
        auto t0 = Clock::now();
        for (int i = 0; i < kBatch; i++) {
            handles[static_cast<std::size_t>(i)] = attach("HBX", err);
            if (handles[static_cast<std::size_t>(i)] == kInvalidBuffer)
                die("attach batch: " + err);
        }
        attachNs.push_back(elapsedMs(t0) * 1e6 / kBatch);
        t0 = Clock::now();
        for (int i = 0; i < kBatch; i++) {
            if (!e.detachHostBuffer(handles[static_cast<std::size_t>(i)], err))
                die("detach batch: " + err);
        }
        detachNs.push_back(elapsedMs(t0) * 1e6 / kBatch);
        // attach+touch+detach cycle: the per-system-invocation view lifecycle (R-LANG-009/012)
        t0 = Clock::now();
        for (int i = 0; i < kBatch / 8; i++) {
            BufferHandle h = attach("HBX", err);
            if (h == kInvalidBuffer) die("cycle attach: " + err);
            double x = 0;
            if (!e.eval("new Float64Array(HBX)[3]", &x, err)) die("cycle eval: " + err);
            if (!e.detachHostBuffer(h, err)) die("cycle detach: " + err);
        }
        cycleNs.push_back(elapsedMs(t0) * 1e6 / (kBatch / 8));
        e.collectGarbage();
    }
    j.statObj("attach_ns", stat(attachNs));
    j.statObj("detach_ns", stat(detachNs));
    j.statObj("attach_touch_detach_cycle_ns", stat(cycleNs));
}

void benchZeroCopy(JsEngine& e, Json& j) {
    std::string err;
    constexpr std::size_t kDoubles = 8192;  // 64 KiB

    // -- shape A: ArrayBuffer wrapping engine-owned HOST memory (the R-LANG-008 ideal)
    {
        std::vector<double> host(kDoubles);
        std::string aerr;
        // Probe with a throwaway attach: engines that cannot wrap host memory refuse here
        // (sandbox-enabled V8 refuses at the seam — the raw API call would abort the process).
        BufferHandle probe =
            e.attachHostBuffer("HBPROBE", host.data(), kDoubles * sizeof(double), aerr);
        Json sub;
        sub.boolean("supported", probe != kInvalidBuffer);
        if (probe == kInvalidBuffer) {
            sub.str("error", aerr);
        } else {
            check(e.detachHostBuffer(probe, aerr), "probe detach: " + aerr);
            zeroCopyBattery(e, sub, host.data(), kDoubles,
                            [&](std::string_view name, std::string& err2) {
                                return e.attachHostBuffer(name, host.data(),
                                                          kDoubles * sizeof(double), err2);
                            });
        }
        j.raw("wrap_host_memory", sub.finish());
    }

    // -- shape B: VM-allocated stable shared buffer (inverted ownership; the sandbox-safe shape)
    {
        void* data = nullptr;
        AllocHandle alloc = e.allocSharedBuffer(kDoubles * sizeof(double), &data, err);
        Json sub;
        sub.boolean("supported", alloc != kInvalidAlloc && data != nullptr);
        if (alloc == kInvalidAlloc) {
            sub.str("error", err);
        } else {
            zeroCopyBattery(e, sub, static_cast<double*>(data), kDoubles,
                            [&](std::string_view name, std::string& err2) {
                                return e.attachSharedBuffer(name, alloc, err2);
                            });
            check(e.freeSharedBuffer(alloc, err), "freeShared: " + err);
        }
        j.raw("vm_allocated_shared", sub.finish());
    }
}

void benchCompute(JsEngine& e, Json& j) {
    std::string err;
    check(e.eval(kComputeKernel, nullptr, err), "compute setup: " + err);
    const FunctionHandle fnReps = e.getFunction("kernelReps");
    check(fnReps != kInvalidFunction, "kernelReps missing");

    double checksum = 0;
    std::string cerr2;
    long long n = 0;
    std::vector<double> reps = calibrateAndRep(
        [&](long long k) {
            const double arg = static_cast<double>(k);
            if (!e.callFunction(fnReps, &arg, 1, &checksum, cerr2)) die(cerr2);
        },
        200.0, 1, &n);
    std::vector<double> runsPerSec;
    for (double ms : reps) runsPerSec.push_back(static_cast<double>(n) / (ms / 1000.0));
    j.num("kernel_checksum", checksum);
    j.num("kernel_reps_per_measurement", static_cast<double>(n));
    j.statObj("kernel_runs_per_sec", stat(runsPerSec));
}

void benchGc(JsEngine& e, Json& j) {
    std::string err;
    check(e.bindHostFunction("hostNowMs", hostNowMs, nullptr), "bind hostNowMs");
    check(e.eval(kGcChurn, nullptr, err), "gc setup: " + err);
    const FunctionHandle fn = e.getFunction("gcChurn");
    check(fn != kInvalidFunction, "gcChurn missing");
    std::vector<double> maxMs, p99Ms;
    std::string cerr2;
    for (int rep = 0; rep < 5; rep++) {
        const double iters = 1000000;
        double samples = 0;
        if (!e.callFunction(fn, &iters, 1, &samples, cerr2)) die(cerr2);
        double mx = 0, p99 = 0;
        check(e.eval("gcMaxMs", &mx, err), "gcMaxMs: " + err);
        check(e.eval("gcP99Ms", &p99, err), "gcP99Ms: " + err);
        maxMs.push_back(mx);
        p99Ms.push_back(p99);
    }
    j.statObj("alloc_loop_max_gap_ms", stat(maxMs));
    j.statObj("alloc_loop_p99_gap_ms", stat(p99Ms));
    j.str("note", "proxy metric: worst sampled gap in a 1M-iteration small-object allocation loop"
                  " (64-iteration sampling stride); includes all engine hiccups, not GC alone");
}

}  // namespace
}  // namespace ctx::spike

int main(int argc, char** argv) {
    using namespace ctx::spike;

    std::string engineName, benchName = "all";
    bool jitless = false;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg.rfind("--engine=", 0) == 0) engineName = arg.substr(9);
        else if (arg == "--jitless") jitless = true;
        else if (arg.rfind("--bench=", 0) == 0) benchName = arg.substr(8);
        else die("unknown arg: " + arg);
    }
    if (engineName != "quickjs" && engineName != "v8") {
        die("usage: context-spike-jsengine --engine=quickjs|v8 [--jitless] --bench=<name>|all");
    }

    EngineConfig cfg;
    cfg.jitless = jitless;
    cfg.exePath = argv[0];

    Json j;
    j.str("engine", engineName);
    j.boolean("jitless", jitless);
    j.str("bench", benchName);

    const std::size_t privBefore = memPrivateBytes();
    const std::size_t wsBefore = memWorkingSetBytes();

    const auto t0 = Clock::now();
    std::string err;
    std::unique_ptr<JsEngine> engine = engineName == "quickjs"
                                           ? createQuickJsEngine(cfg, err)
                                           : createV8Engine(cfg, err);
    if (!engine) die("engine creation failed: " + err);
    double one = 0;
    check(engine->eval("1 + 1", &one, err) && one == 2.0, "first eval failed: " + err);
    const double startupMs = elapsedMs(t0);

    j.str("version", engine->version());
    j.num("startup_to_first_eval_ms", startupMs);

    if (benchName == "memory" || benchName == "all") {
        engine->collectGarbage();
        Json sub;
        sub.num("private_bytes_before_init", static_cast<double>(privBefore));
        sub.num("private_bytes_after_init", static_cast<double>(memPrivateBytes()));
        sub.num("private_delta_mb",
                (static_cast<double>(memPrivateBytes()) - static_cast<double>(privBefore)) /
                    (1024.0 * 1024.0));
        sub.num("working_set_delta_mb",
                (static_cast<double>(memWorkingSetBytes()) - static_cast<double>(wsBefore)) /
                    (1024.0 * 1024.0));
        j.raw("memory", sub.finish());
    }
    if (benchName == "calls" || benchName == "all") {
        Json sub;
        benchCalls(*engine, sub);
        j.raw("calls", sub.finish());
    }
    if (benchName == "zerocopy" || benchName == "all") {
        Json sub;
        benchZeroCopy(*engine, sub);
        j.raw("zerocopy", sub.finish());
    }
    if (benchName == "compute" || benchName == "all") {
        Json sub;
        benchCompute(*engine, sub);
        j.raw("compute", sub.finish());
    }
    if (benchName == "gc" || benchName == "all") {
        Json sub;
        benchGc(*engine, sub);
        j.raw("gc", sub.finish());
    }

    std::printf("%s\n", j.finish().c_str());
    return 0;
}
