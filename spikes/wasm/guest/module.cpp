// spikes/wasm — guest module (L-8 / R-LANG-003). THROWAWAY spike code.
//
// A small C++ "native-tier logic module" compiled to wasm32 and executed in-process by the
// host benchmark (spikes/wasm/src/bench_main.cpp) on two runtimes (wasmtime, WAMR).
//
// Deliberately FREESTANDING (wasm32-unknown-unknown, -nostdlib, ZERO imports — not even WASI):
// the module's entire capability surface is its exports + its own linear memory. That is the
// R-SEC-001/R-SEC-002 sandbox story measured by the `trap`/`sandbox` benches: what the module
// can touch by default is NOTHING the host did not hand it.
//
// Build (see build_module.py — pins the toolchain + flags and records the artifact hash):
//   clang++ --target=wasm32-unknown-unknown -O2 -nostdlib -fno-builtin \
//       -Wl,--no-entry -Wl,--export-dynamic -Wl,-z,stack-size=65536 module.cpp -o module.wasm
//
// The compute kernel is a LINE-FOR-LINE port of the js-engine spike's JS kernel
// (spikes/js-engine/src/bench_main.cpp kComputeKernel) so the TS-tier vs WASM-tier throughput
// ratio is measured on the SAME workload (same doubles, same checksum: 175162.0).

#include <stdint.h>

#define WASM_EXPORT(name) extern "C" __attribute__((export_name(#name)))

// ---- zero-copy shared buffer (R-LANG-008) ------------------------------------------------------
// Static (BSS) buffer inside linear memory. The host discovers it via buf_ptr()/buf_size(),
// maps the runtime's linear-memory base pointer, and reads/writes it IN PLACE — no copies.

static float g_buf[1u << 20];  // 4 MiB, zero-initialized BSS (adds no bytes to module.wasm)

WASM_EXPORT(buf_ptr) uint32_t buf_ptr() { return reinterpret_cast<uint32_t>(&g_buf[0]); }
WASM_EXPORT(buf_size) uint32_t buf_size() { return sizeof(g_buf); }

// ---- call-boundary targets ----------------------------------------------------------------------

WASM_EXPORT(nop) void nop() {}

WASM_EXPORT(add) int32_t add(int32_t a, int32_t b) { return a + b; }

WASM_EXPORT(sum3) double sum3(double a, double b, double c) { return a + b + c; }

// ---- linear-memory mutation kernels (R-LANG-008 zero-copy pattern) ------------------------------
// `ptr` is a guest pointer (offset into linear memory) the HOST computed from buf_ptr();
// mutation happens in place, visible to the host without any copy-back.

WASM_EXPORT(scale_f32) void scale_f32(uint32_t ptr, uint32_t count, float factor) {
    float* p = reinterpret_cast<float*>(ptr);
    for (uint32_t i = 0; i < count; i++) p[i] *= factor;
}

WASM_EXPORT(sum_f32) double sum_f32(uint32_t ptr, uint32_t count) {
    const float* p = reinterpret_cast<const float*>(ptr);
    double s = 0;
    for (uint32_t i = 0; i < count; i++) s += p[i];
    return s;
}

// ---- compute kernel (ported 1:1 from spikes/js-engine kComputeKernel) ----------------------------

namespace {

struct Particle {
    double x, y, vx, vy;
};
Particle g_parts[1000];

double kernelOnce() {
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
        g_parts[i].x = i * 0.5;
        g_parts[i].y = i * 0.25;
        g_parts[i].vx = 0.1;
        g_parts[i].vy = -0.05;
    }
    for (int step = 0; step < 20; step++) {
        for (int i = 0; i < 1000; i++) {
            Particle& p = g_parts[i];
            p.x += p.vx;
            p.y += p.vy;
            if (p.x > 100) p.x -= 200;
            if (p.y < -100) p.y += 200;
        }
    }
    for (int i = 0; i < 1000; i += 97) acc += g_parts[i].x;
    return acc;
}

}  // namespace

WASM_EXPORT(kernel) double kernel() { return kernelOnce(); }

WASM_EXPORT(kernel_reps) double kernel_reps(int32_t n) {
    double r = 0;
    for (int32_t i = 0; i < n; i++) r = kernelOnce();
    return r;
}

// ---- pointer-stability probe (memory.grow) -------------------------------------------------------

WASM_EXPORT(mem_size_pages) int32_t mem_size_pages() {
    return static_cast<int32_t>(__builtin_wasm_memory_size(0));
}

WASM_EXPORT(mem_grow) int32_t mem_grow(int32_t pages) {
    return static_cast<int32_t>(__builtin_wasm_memory_grow(0, static_cast<uint32_t>(pages)));
}

// ---- sandbox probes (deliberate out-of-bounds access; MUST trap, not corrupt) --------------------

WASM_EXPORT(load_u8) int32_t load_u8(uint32_t addr) {
    return *reinterpret_cast<volatile uint8_t*>(addr);
}

WASM_EXPORT(store_u8) void store_u8(uint32_t addr, int32_t v) {
    *reinterpret_cast<volatile uint8_t*>(addr) = static_cast<uint8_t>(v);
}
