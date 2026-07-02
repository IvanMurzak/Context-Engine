// spikes/js-engine — §2d embedding seam (DESIGN-DECISIONS.md §2d, R-LANG-008/009/011, R-OBS-005).
//
// ONE minimal seam, multiple backends. This is the shape the EditorKernel/RuntimeKernel TS tier
// would embed a JS VM behind; the spike implements it twice (QuickJS interpreter-class, V8
// JIT-class) and benchmarks the seam, not the engines' native APIs. THROWAWAY spike code.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ctx::spike {

// Host function exposed to JS: receives `nargs` doubles, returns a double.
// Doubles only — the spike measures the boundary, not a full marshalling layer.
using HostFunction = double (*)(void* user, const double* args, std::size_t nargs);

struct EngineConfig {
    // Request interpreter-only execution (V8: --jitless; QuickJS: always an interpreter).
    bool jitless = false;
    // argv[0]; engines that locate data files next to the executable need it (V8: icudtl.dat).
    std::string exePath;
};

// Opaque handle to an ArrayBuffer the engine exposes over host memory. 0 is invalid.
using BufferHandle = std::uint64_t;
inline constexpr BufferHandle kInvalidBuffer = 0;

// Opaque handle to a resolved JS function. 0 is invalid.
using FunctionHandle = std::uint64_t;
inline constexpr FunctionHandle kInvalidFunction = 0;

// Opaque handle to a VM-side stable shared allocation. 0 is invalid.
using AllocHandle = std::uint64_t;
inline constexpr AllocHandle kInvalidAlloc = 0;

class JsEngine {
public:
    virtual ~JsEngine() = default;

    virtual std::string_view name() const = 0;  // "quickjs" | "v8"
    virtual std::string version() const = 0;

    // --- evaluation ------------------------------------------------------------------------
    // Evaluates `code` at global scope. A numeric completion value lands in *numResult when
    // non-null. Returns false and fills `err` on exception.
    virtual bool eval(std::string_view code, double* numResult, std::string& err) = 0;

    // --- host -> JS calls ------------------------------------------------------------------
    // Resolve a global function once; call repeatedly through the cached handle.
    virtual FunctionHandle getFunction(std::string_view globalName) = 0;
    virtual bool callFunction(FunctionHandle fn, const double* args, std::size_t nargs,
                              double* result, std::string& err) = 0;

    // --- JS -> host binding ----------------------------------------------------------------
    virtual bool bindHostFunction(std::string_view globalName, HostFunction fn, void* user) = 0;

    // --- zero-copy ArrayBuffer sharing (R-LANG-008) + detach protocol (R-LANG-009) ----------
    //
    // TWO sharing shapes, because engines disagree on who may own the memory:
    //
    // (A) wrap HOST memory: expose `bytes` at `data` as globalThis[globalName] without copying.
    //     QuickJS supports this; sandbox-enabled V8 builds FATALLY REJECT it (backing stores
    //     must live inside the sandbox address space) — those return kInvalidBuffer + err.
    virtual BufferHandle attachHostBuffer(std::string_view globalName, void* data,
                                          std::size_t bytes, std::string& err) = 0;
    //
    // (B) VM-allocated shared memory (inverted ownership): the VM allocates a STABLE buffer the
    //     host reads/writes through *outData; per-system-invocation ArrayBuffers are attached
    //     over it and detached at system exit while the allocation persists. This is the shape
    //     sandbox-enabled V8 supports; QuickJS implements it trivially (malloc + wrap).
    virtual AllocHandle allocSharedBuffer(std::size_t bytes, void** outData,
                                          std::string& err) = 0;
    virtual BufferHandle attachSharedBuffer(std::string_view globalName, AllocHandle alloc,
                                            std::string& err) = 0;
    virtual bool freeSharedBuffer(AllocHandle alloc, std::string& err) = 0;
    //
    // Detach/neuter an ArrayBuffer from either shape — the R-LANG-009 end-of-system step. The
    // underlying memory is never freed or touched; outstanding JS views must go dead
    // (byteLength 0 / throw on access).
    virtual bool detachHostBuffer(BufferHandle h, std::string& err) = 0;

    // --- housekeeping ----------------------------------------------------------------------
    virtual void collectGarbage() = 0;  // best-effort full GC between benchmark phases
};

// Factories return nullptr + err when the backend is unavailable.
std::unique_ptr<JsEngine> createQuickJsEngine(const EngineConfig& cfg, std::string& err);
std::unique_ptr<JsEngine> createV8Engine(const EngineConfig& cfg, std::string& err);

}  // namespace ctx::spike
