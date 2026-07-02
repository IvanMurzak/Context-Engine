// spikes/wasm — L-8/R-LANG-003 benchmark harness. THROWAWAY spike code.
//
// One runtime config per process (clean footprint/memory numbers); emits a single JSON object
// on stdout; run_bench.py drives fresh processes and aggregates.
//
//   context-spike-wasm --runtime=wasmtime|wasmtime-winch|wasmtime-pulley|wamr|wamr-aot
//                      --bench=smoke|load|calls|zerocopy|compute|trap|grow|memory|all
//                      [--module=<path>]   (default: module.wasm / module.aot next to the exe)

#include "ctx/wasm_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

namespace ctx::spike {

#ifndef CTX_SPIKE_HAS_WASMTIME
std::unique_ptr<WasmEngine> createWasmtimeEngine(const EngineConfig&, std::string& err) {
    err = "wasmtime backend not built (CONTEXT_SPIKE_WASM_WASMTIME=OFF)";
    return nullptr;
}
#endif
#ifndef CTX_SPIKE_HAS_WAMR
std::unique_ptr<WasmEngine> createWamrEngine(const EngineConfig&, std::string& err) {
    err = "WAMR backend not built (CONTEXT_SPIKE_WASM_WAMR=OFF)";
    return nullptr;
}
#endif

namespace {

using Clock = std::chrono::steady_clock;

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

// ---- native reference kernel --------------------------------------------------------------
// The SAME kernel as guest/module.cpp (itself a 1:1 port of the js-engine spike's JS kernel),
// compiled natively by the host toolchain — the native=1.0x reference for relative throughput
// and the checksum cross-check.

struct Particle {
    double x, y, vx, vy;
};
Particle g_nativeParts[1000];

double nativeKernelOnce() {
    double acc = 0;
    for (int py = 0; py < 96; py++) {
        const double y0 = -1.3 + 2.6 * py / 96;
        for (int px = 0; px < 96; px++) {
            const double x0 = -2.05 + 2.6 * px / 96;
            double x = 0, y = 0;
            int it = 0;
            while (x * x + y * y <= 4 && it < 64) {
                const double xt = x * x - y * y + x0;
                y = 2 * x * y + y0;
                x = xt;
                it++;
            }
            acc += it;
        }
    }
    for (int i = 0; i < 1000; i++) {
        g_nativeParts[i].x = i * 0.5;
        g_nativeParts[i].y = i * 0.25;
        g_nativeParts[i].vx = 0.1;
        g_nativeParts[i].vy = -0.05;
    }
    for (int step = 0; step < 20; step++) {
        for (int i = 0; i < 1000; i++) {
            Particle& p = g_nativeParts[i];
            p.x += p.vx;
            p.y += p.vy;
            if (p.x > 100) p.x -= 200;
            if (p.y < -100) p.y += 200;
        }
    }
    for (int i = 0; i < 1000; i += 97) acc += g_nativeParts[i].x;
    return acc;
}

double nativeKernelReps(long long n) {
    double r = 0;
    for (long long i = 0; i < n; i++) r = nativeKernelOnce();
    return r;
}

// ---- JSON emission --------------------------------------------------------------------------

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
        out += "\"";
        for (char c : v) {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else if (static_cast<unsigned char>(c) >= 0x20) out += c;
        }
        out += "\"";
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

// ---- shared setup ----------------------------------------------------------------------------

struct RuntimeSpec {
    std::string flag;  // --runtime= value
    bool isWamr = false;
    ExecMode mode = ExecMode::Jit;
};

const RuntimeSpec kRuntimes[] = {
    {"wasmtime", false, ExecMode::Jit},
    {"wasmtime-winch", false, ExecMode::BaselineJit},
    {"wasmtime-pulley", false, ExecMode::Interp},
    {"wamr", true, ExecMode::Interp},
    {"wamr-aot", true, ExecMode::Aot},
};

std::unique_ptr<WasmEngine> createEngine(const RuntimeSpec& spec, std::string& err) {
    EngineConfig cfg;
    cfg.mode = spec.mode;
    return spec.isWamr ? createWamrEngine(cfg, err) : createWasmtimeEngine(cfg, err);
}

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    check(in.good(), "cannot open module file: " + path);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

struct Exports {
    FunctionHandle nop, add, sum3, bufPtr, bufSize, scaleF32, sumF32, kernel, kernelReps,
        memSizePages, memGrow, loadU8, storeU8;
};

Exports resolveExports(WasmEngine& e) {
    Exports x{};
    x.nop = e.getFunction("nop");
    x.add = e.getFunction("add");
    x.sum3 = e.getFunction("sum3");
    x.bufPtr = e.getFunction("buf_ptr");
    x.bufSize = e.getFunction("buf_size");
    x.scaleF32 = e.getFunction("scale_f32");
    x.sumF32 = e.getFunction("sum_f32");
    x.kernel = e.getFunction("kernel");
    x.kernelReps = e.getFunction("kernel_reps");
    x.memSizePages = e.getFunction("mem_size_pages");
    x.memGrow = e.getFunction("mem_grow");
    x.loadU8 = e.getFunction("load_u8");
    x.storeU8 = e.getFunction("store_u8");
    check(x.nop && x.add && x.sum3 && x.bufPtr && x.bufSize && x.scaleF32 && x.sumF32 &&
              x.kernel && x.kernelReps && x.memSizePages && x.memGrow && x.loadU8 && x.storeU8,
          "export resolution failed");
    return x;
}

std::uint32_t callU32(WasmEngine& e, FunctionHandle fn) {
    Val r{};
    std::string err;
    check(e.call(fn, nullptr, 0, &r, 1, err), "callU32: " + err);
    return static_cast<std::uint32_t>(r.i32);
}

// ---- benches -----------------------------------------------------------------------------------

void benchLoad(const RuntimeSpec& spec, const std::vector<std::uint8_t>& bytes, Json& j) {
    std::vector<double> compileMs, instantiateMs;
    for (int i = 0; i < 5; i++) {
        std::string err;
        auto e = createEngine(spec, err);
        check(e != nullptr, "createEngine: " + err);
        auto t0 = Clock::now();
        check(e->compile(bytes.data(), bytes.size(), err), "compile: " + err);
        compileMs.push_back(elapsedMs(t0));
        t0 = Clock::now();
        check(e->instantiate(err), "instantiate: " + err);
        instantiateMs.push_back(elapsedMs(t0));
    }
    j.num("module_bytes", static_cast<double>(bytes.size()));
    j.num("compile_ms_first", compileMs.front());
    j.statObj("compile_ms", stat(compileMs));
    j.statObj("instantiate_ms", stat(instantiateMs));
}

void benchCalls(WasmEngine& e, const Exports& x, Json& j) {
    std::string err;
    // one-shot correctness
    Val r{};
    const Val addArgs[2] = {Val::makeI32(7), Val::makeI32(35)};
    check(e.call(x.add, addArgs, 2, &r, 1, err) && r.i32 == 42, "add(7,35) != 42: " + err);
    const Val sumArgs[3] = {Val::makeF64(1.5), Val::makeF64(2.5), Val::makeF64(3.0)};
    check(e.call(x.sum3, sumArgs, 3, &r, 1, err) && r.f64 == 7.0, "sum3 != 7.0: " + err);

    struct Case {
        const char* name;
        std::function<void(long long)> run;
    };
    const Case cases[] = {
        {"host_to_wasm_empty",
         [&](long long n) {
             std::string cerr2;
             for (long long i = 0; i < n; i++) {
                 if (!e.call(x.nop, nullptr, 0, nullptr, 0, cerr2)) die(cerr2);
             }
         }},
        {"host_to_wasm_add_i32",
         [&](long long n) {
             std::string cerr2;
             Val out{};
             for (long long i = 0; i < n; i++) {
                 if (!e.call(x.add, addArgs, 2, &out, 1, cerr2)) die(cerr2);
             }
         }},
        {"host_to_wasm_sum3_f64",
         [&](long long n) {
             std::string cerr2;
             Val out{};
             for (long long i = 0; i < n; i++) {
                 if (!e.call(x.sum3, sumArgs, 3, &out, 1, cerr2)) die(cerr2);
             }
         }},
    };

    for (const Case& c : cases) {
        long long n = 0;
        std::vector<double> reps = calibrateAndRep(c.run, 100.0, 1 << 12, &n);
        std::vector<double> nsPerOp, opsPerSec;
        for (double ms : reps) {
            nsPerOp.push_back(ms * 1e6 / static_cast<double>(n));
            opsPerSec.push_back(static_cast<double>(n) / (ms / 1000.0));
        }
        Json sub;
        sub.num("n_per_rep", static_cast<double>(n));
        sub.statObj("ns_per_op", stat(nsPerOp));
        sub.statObj("ops_per_sec", stat(opsPerSec));
        j.raw(c.name, sub.finish());
    }
}

void benchZerocopy(WasmEngine& e, const Exports& x, Json& j) {
    std::string err;
    const std::uint32_t bufPtr = callU32(e, x.bufPtr);
    const std::uint32_t bufSize = callU32(e, x.bufSize);
    const std::uint32_t count = bufSize / 4;
    std::uint8_t* base = e.memoryData(err);
    check(base != nullptr, "memoryData: " + err);
    check(bufPtr + bufSize <= e.memoryBytes(), "guest buffer outside linear memory?");
    float* buf = reinterpret_cast<float*>(base + bufPtr);

    // -- correctness: host writes IN PLACE, guest reads them; guest mutates, host reads back.
    for (std::uint32_t i = 0; i < count; i++) buf[i] = static_cast<float>(i % 1000) * 0.5f;
    double expected = 0;
    for (std::uint32_t i = 0; i < count; i++) expected += static_cast<double>(buf[i]);
    Val r{};
    const Val sumArgs[2] = {Val::makeI32(static_cast<std::int32_t>(bufPtr)),
                            Val::makeI32(static_cast<std::int32_t>(count))};
    check(e.call(x.sumF32, sumArgs, 2, &r, 1, err), "sum_f32: " + err);
    j.boolean("host_write_visible_to_guest", r.f64 == expected);

    const Val scaleArgs[3] = {Val::makeI32(static_cast<std::int32_t>(bufPtr)),
                              Val::makeI32(static_cast<std::int32_t>(count)),
                              Val::makeF32(2.0f)};
    check(e.call(x.scaleF32, scaleArgs, 3, nullptr, 0, err), "scale_f32: " + err);
    bool scaled = true;
    for (std::uint32_t i = 0; i < count && scaled; i += 997) {
        scaled = buf[i] == static_cast<float>(i % 1000) * 0.5f * 2.0f;
    }
    j.boolean("guest_write_visible_to_host", scaled);

    // -- pointer identity: the mapped base must be stable across calls (absent memory.grow).
    std::uint8_t* base2 = e.memoryData(err);
    j.boolean("memory_base_stable_across_calls", base2 == base);

    // -- timed: full-buffer mutation through the boundary (1M floats, in place)
    {
        long long n = 0;
        std::vector<double> reps = calibrateAndRep(
            [&](long long reps2) {
                std::string cerr2;
                for (long long i = 0; i < reps2; i++) {
                    const Val a[3] = {Val::makeI32(static_cast<std::int32_t>(bufPtr)),
                                      Val::makeI32(static_cast<std::int32_t>(count)),
                                      Val::makeF32(1.0f)};
                    if (!e.call(x.scaleF32, a, 3, nullptr, 0, cerr2)) die(cerr2);
                }
            },
            100.0, 4, &n);
        std::vector<double> nsPerElem;
        for (double ms : reps) nsPerElem.push_back(ms * 1e6 / (static_cast<double>(n) * count));
        Json sub;
        sub.num("elems", count);
        sub.num("n_per_rep", static_cast<double>(n));
        sub.statObj("ns_per_elem", stat(nsPerElem));
        // 8 bytes touched per element (4 read + 4 written), in place.
        sub.num("gb_per_sec_median", 8.0 / stat(nsPerElem).median);
        j.raw("scale_1m_f32", sub.finish());
    }

    // -- native reference: the same mutation done directly by the host on the SAME memory.
    // No volatile on the data (that would forbid vectorization and handicap native); the
    // factor is loaded through a volatile so the multiply cannot be folded away. The host
    // compiler MAY auto-vectorize — that IS the honest native ceiling (the guest was compiled
    // WITHOUT -msimd128; WASM SIMD is a recorded future lever, see FINDINGS).
    {
        long long n = 0;
        volatile float factorSrc = 1.0f;
        std::vector<double> reps = calibrateAndRep(
            [&](long long reps2) {
                for (long long i = 0; i < reps2; i++) {
                    const float factor = factorSrc;
                    for (std::uint32_t k = 0; k < count; k++) buf[k] *= factor;
                }
            },
            100.0, 4, &n);
        std::vector<double> nsPerElem;
        for (double ms : reps) nsPerElem.push_back(ms * 1e6 / (static_cast<double>(n) * count));
        Json sub;
        sub.statObj("ns_per_elem", stat(nsPerElem));
        j.raw("scale_1m_f32_native_host", sub.finish());
    }

    // -- timed: small-batch mutation (64 floats) — the boundary+tiny-work per-system shape.
    {
        long long n = 0;
        std::vector<double> reps = calibrateAndRep(
            [&](long long reps2) {
                std::string cerr2;
                for (long long i = 0; i < reps2; i++) {
                    const Val a[3] = {Val::makeI32(static_cast<std::int32_t>(bufPtr)),
                                      Val::makeI32(64), Val::makeF32(1.0f)};
                    if (!e.call(x.scaleF32, a, 3, nullptr, 0, cerr2)) die(cerr2);
                }
            },
            100.0, 1 << 12, &n);
        std::vector<double> nsPerCall;
        for (double ms : reps) nsPerCall.push_back(ms * 1e6 / static_cast<double>(n));
        Json sub;
        sub.num("n_per_rep", static_cast<double>(n));
        sub.statObj("ns_per_call", stat(nsPerCall));
        j.raw("scale_64_f32", sub.finish());
    }
}

void benchCompute(WasmEngine& e, const Exports& x, Json& j) {
    std::string err;
    const double nativeChecksum = nativeKernelOnce();
    Val r{};
    check(e.call(x.kernel, nullptr, 0, &r, 1, err), "kernel: " + err);
    j.num("checksum", r.f64);
    j.num("native_checksum", nativeChecksum);
    j.boolean("checksum_matches_native", r.f64 == nativeChecksum);

    long long n = 0;
    std::vector<double> reps = calibrateAndRep(
        [&](long long reps2) {
            std::string cerr2;
            check(reps2 <= 0x7fffffff, "kernel rep count exceeds i32");
            const Val a = Val::makeI32(static_cast<std::int32_t>(reps2));
            Val out{};
            if (!e.call(x.kernelReps, &a, 1, &out, 1, cerr2)) die(cerr2);
        },
        300.0, 4, &n);
    std::vector<double> runsPerSec;
    for (double ms : reps) runsPerSec.push_back(static_cast<double>(n) / (ms / 1000.0));
    j.num("n_per_rep", static_cast<double>(n));
    j.statObj("kernel_runs_per_sec", stat(runsPerSec));

    long long nn = 0;
    std::vector<double> nreps = calibrateAndRep(
        [&](long long reps2) { (void)nativeKernelReps(reps2); }, 300.0, 4, &nn);
    std::vector<double> nativeRunsPerSec;
    for (double ms : nreps) nativeRunsPerSec.push_back(static_cast<double>(nn) / (ms / 1000.0));
    j.statObj("native_kernel_runs_per_sec", stat(nativeRunsPerSec));
}

// Trap messages can carry a multi-line wasm backtrace; keep the informative line (wasmtime
// puts the cause on a trailing "Caused by: wasm trap: ..." line, WAMR puts it first).
std::string firstLine(const std::string& s) {
    std::size_t start = 0;
    std::string first;
    while (start <= s.size()) {
        std::size_t nl = s.find('\n', start);
        if (nl == std::string::npos) nl = s.size();
        const std::string line = s.substr(start, nl - start);
        if (first.empty() && !line.empty()) first = line;
        if (line.find("wasm trap:") != std::string::npos ||
            line.find("out of bounds") != std::string::npos) {
            return line;
        }
        start = nl + 1;
    }
    return first;
}

void benchTrap(WasmEngine& e, const Exports& x, Json& j) {
    std::string err;
    const auto memBytes = static_cast<std::uint32_t>(
        std::min<std::size_t>(e.memoryBytes(), 0xffffffffu));

    // OOB read: first byte past the end, and far past the end (guard-page territory).
    Val r{};
    const Val edge = Val::makeI32(static_cast<std::int32_t>(memBytes));
    const bool edgeCallOk = e.call(x.loadU8, &edge, 1, &r, 1, err);
    j.boolean("oob_read_traps", !edgeCallOk);
    j.str("oob_read_message", edgeCallOk ? "" : firstLine(err));

    err.clear();
    const Val farAddr = Val::makeI32(static_cast<std::int32_t>(0x7fffffff));
    const bool farCallOk = e.call(x.loadU8, &farAddr, 1, &r, 1, err);
    j.boolean("oob_far_read_traps", !farCallOk);

    err.clear();
    const Val st[2] = {Val::makeI32(static_cast<std::int32_t>(memBytes)), Val::makeI32(0x5a)};
    const bool storeOk = e.call(x.storeU8, st, 2, nullptr, 0, err);
    j.boolean("oob_write_traps", !storeOk);
    j.str("oob_write_message", storeOk ? "" : firstLine(err));

    // The engine (and process) must remain usable after a trap — R-SEC-001 containment.
    err.clear();
    j.boolean("engine_usable_after_trap", e.call(x.nop, nullptr, 0, nullptr, 0, err));

    // Structural sandbox statement: the guest declares ZERO imports (verified at instantiate:
    // an empty import list satisfied it) — no WASI, no host functions, nothing ambient.
    j.boolean("instantiated_with_zero_imports", true);
}

void benchGrow(WasmEngine& e, const Exports& x, Json& j) {
    std::string err;
    const std::uint32_t bufPtr = callU32(e, x.bufPtr);
    std::uint8_t* baseBefore = e.memoryData(err);
    check(baseBefore != nullptr, "memoryData: " + err);
    const std::size_t bytesBefore = e.memoryBytes();
    float* buf = reinterpret_cast<float*>(baseBefore + bufPtr);
    buf[0] = 424242.0f;  // sentinel — must survive the grow

    Val r{};
    const Val pages = Val::makeI32(256);  // +16 MiB
    check(e.call(x.memGrow, &pages, 1, &r, 1, err), "mem_grow: " + err);
    check(r.i32 >= 0, "mem_grow failed (returned -1)");

    std::uint8_t* baseAfter = e.memoryData(err);
    const std::size_t bytesAfter = e.memoryBytes();
    j.num("bytes_before", static_cast<double>(bytesBefore));
    j.num("bytes_after", static_cast<double>(bytesAfter));
    j.boolean("grew", bytesAfter > bytesBefore);
    j.boolean("base_moved_after_grow", baseAfter != baseBefore);
    const float* bufAfter = reinterpret_cast<const float*>(baseAfter + bufPtr);
    j.boolean("contents_preserved_after_grow", bufAfter[0] == 424242.0f);
}

void benchMemory(WasmEngine& e, Json& j) {
    j.num("private_bytes_after_init", static_cast<double>(memPrivateBytes()));
    j.num("working_set_after_init", static_cast<double>(memWorkingSetBytes()));
    j.num("linear_memory_bytes", static_cast<double>(e.memoryBytes()));
}

// smoke = the full correctness battery, no timed benches; exit 1 on any failure (CI gate).
int benchSmoke(WasmEngine& e, const Exports& x, Json& j) {
    std::string err;
    bool pass = true;

    Val r{};
    const Val addArgs[2] = {Val::makeI32(7), Val::makeI32(35)};
    pass &= e.call(x.add, addArgs, 2, &r, 1, err) && r.i32 == 42;

    const std::uint32_t bufPtr = callU32(e, x.bufPtr);
    std::uint8_t* base = e.memoryData(err);
    pass &= base != nullptr;
    float* buf = reinterpret_cast<float*>(base + bufPtr);
    buf[0] = 21.0f;
    const Val scaleArgs[3] = {Val::makeI32(static_cast<std::int32_t>(bufPtr)), Val::makeI32(1),
                              Val::makeF32(2.0f)};
    pass &= e.call(x.scaleF32, scaleArgs, 3, nullptr, 0, err);
    pass &= buf[0] == 42.0f;

    const Val oob = Val::makeI32(static_cast<std::int32_t>(0x7fffffff));
    pass &= !e.call(x.loadU8, &oob, 1, &r, 1, err);          // must trap...
    pass &= e.call(x.nop, nullptr, 0, nullptr, 0, err);      // ...and stay usable

    pass &= e.call(x.kernel, nullptr, 0, &r, 1, err) && r.f64 == nativeKernelOnce();

    j.boolean("smoke_pass", pass);
    return pass ? 0 : 1;
}

std::string dirOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

}  // namespace

int benchMain(int argc, char** argv) {
    std::string runtimeFlag, benchFlag = "all", modulePath;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg.rfind("--runtime=", 0) == 0) runtimeFlag = arg.substr(10);
        else if (arg.rfind("--bench=", 0) == 0) benchFlag = arg.substr(8);
        else if (arg.rfind("--module=", 0) == 0) modulePath = arg.substr(9);
        else die("unknown arg: " + arg + " (usage: --runtime=... --bench=... [--module=...])");
    }

    const RuntimeSpec* spec = nullptr;
    for (const RuntimeSpec& s : kRuntimes) {
        if (s.flag == runtimeFlag) spec = &s;
    }
    if (spec == nullptr) {
        die("--runtime must be one of: wasmtime, wasmtime-winch, wasmtime-pulley, wamr, "
            "wamr-aot");
    }
    if (modulePath.empty()) {
        modulePath = dirOf(argv[0]) + (spec->mode == ExecMode::Aot ? "module.aot"
                                                                   : "module.wasm");
    }

    const std::vector<std::uint8_t> bytes = readFile(modulePath);

    std::string err;
    auto engine = createEngine(*spec, err);
    check(engine != nullptr, "createEngine(" + runtimeFlag + "): " + err);
    check(engine->compile(bytes.data(), bytes.size(), err), "compile: " + err);
    check(engine->instantiate(err), "instantiate: " + err);
    const Exports exports = resolveExports(*engine);

    Json j;
    j.str("runtime", std::string(engine->name()));
    j.str("mode", std::string(engine->mode()));
    j.str("version", engine->version());
    j.str("bench", benchFlag);
    j.num("module_bytes", static_cast<double>(bytes.size()));

    int rc = 0;
    const bool all = benchFlag == "all";
    if (benchFlag == "smoke") rc = benchSmoke(*engine, exports, j);
    if (all || benchFlag == "load") benchLoad(*spec, bytes, j);
    if (all || benchFlag == "calls") benchCalls(*engine, exports, j);
    if (all || benchFlag == "zerocopy") benchZerocopy(*engine, exports, j);
    if (all || benchFlag == "compute") benchCompute(*engine, exports, j);
    if (all || benchFlag == "trap") benchTrap(*engine, exports, j);
    if (all || benchFlag == "grow") benchGrow(*engine, exports, j);
    if (all || benchFlag == "memory") benchMemory(*engine, j);

    std::printf("%s\n", j.finish().c_str());
    return rc;
}

}  // namespace ctx::spike

int main(int argc, char** argv) { return ctx::spike::benchMain(argc, argv); }
