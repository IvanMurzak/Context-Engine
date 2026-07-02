// spikes/wasm — WASM native-tier embedding seam (L-8, R-LANG-003/005/008; R-KERNEL-001,
// R-SEC-001/002).
//
// ONE minimal seam, multiple runtimes. This is the shape RuntimeKernel's per-platform WASM
// execution backend (R-LANG-003) would take; the spike implements it twice — wasmtime
// (Cranelift JIT + Pulley interpreter) and WAMR (fast-interpreter + AOT, the iOS-class no-JIT
// answer) — and benchmarks the seam, not the runtimes' native APIs. THROWAWAY spike code.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ctx::spike {

// Execution mode requested from a backend. Which modes a backend supports is part of the
// spike's findings (R-LANG-005 per-platform execution strategy):
//   wasmtime: Jit (Cranelift), BaselineJit (Winch), Interp (Pulley bytecode)
//   wamr:     Interp (fast-interpreter), Aot (load a wamrc-precompiled .aot image)
enum class ExecMode { Jit, BaselineJit, Interp, Aot };

struct EngineConfig {
    ExecMode mode = ExecMode::Jit;
};

// WASM core value — exactly what crosses the call boundary (no marshalling layer; the spike
// measures the boundary itself).
struct Val {
    enum class Kind { I32, I64, F32, F64 } kind = Kind::I32;
    union {
        std::int32_t i32;
        std::int64_t i64;
        float f32;
        double f64;
    };
    static Val makeI32(std::int32_t v) { Val x; x.kind = Kind::I32; x.i32 = v; return x; }
    static Val makeI64(std::int64_t v) { Val x; x.kind = Kind::I64; x.i64 = v; return x; }
    static Val makeF32(float v)        { Val x; x.kind = Kind::F32; x.f32 = v; return x; }
    static Val makeF64(double v)       { Val x; x.kind = Kind::F64; x.f64 = v; return x; }
};

// Opaque handle to a resolved exported function. 0 is invalid.
using FunctionHandle = std::uint64_t;
inline constexpr FunctionHandle kInvalidFunction = 0;

class WasmEngine {
public:
    virtual ~WasmEngine() = default;

    virtual std::string_view name() const = 0;   // "wasmtime" | "wamr"
    virtual std::string version() const = 0;
    virtual std::string_view mode() const = 0;   // "cranelift-jit" | "winch" | "pulley-interp"
                                                 // | "fast-interp" | "aot"

    // --- module lifecycle (measured: compile / instantiate, R-LANG-005 load story) ----------
    // Compile (or, for AOT images, load) the module bytes. Returns false + err on failure.
    virtual bool compile(const std::uint8_t* bytes, std::size_t len, std::string& err) = 0;
    // Instantiate the compiled module. The spike's guest imports NOTHING (not even WASI) —
    // instantiation provides zero imports, which IS the R-SEC-002 import-gating statement.
    virtual bool instantiate(std::string& err) = 0;

    // --- calls (measured: the host->module boundary, R-LANG-008 coarse-grained law) ----------
    virtual FunctionHandle getFunction(std::string_view exportName) = 0;
    // On a guest trap, returns false and fills `err` with a message containing "trap".
    // The process MUST survive (R-SEC-001 containment; the `trap` bench verifies).
    virtual bool call(FunctionHandle fn, const Val* args, std::size_t nargs, Val* results,
                      std::size_t nresults, std::string& err) = 0;

    // --- linear memory (measured: the zero-copy pattern, R-LANG-008/009) ---------------------
    // Host-side view of the module's exported linear memory. POINTER-STABILITY RULE (finding,
    // not assumption): this pointer is only guaranteed until the next memory.grow — re-fetch
    // at every system entry, the WASM analog of the R-LANG-009 view-lifetime protocol.
    virtual std::uint8_t* memoryData(std::string& err) = 0;
    virtual std::size_t memoryBytes() = 0;
};

// Factories return nullptr + err when the backend/mode is unavailable (that unavailability is
// itself a finding — e.g. a wasmtime prebuilt without the requested compilation strategy).
std::unique_ptr<WasmEngine> createWasmtimeEngine(const EngineConfig& cfg, std::string& err);
std::unique_ptr<WasmEngine> createWamrEngine(const EngineConfig& cfg, std::string& err);

}  // namespace ctx::spike
