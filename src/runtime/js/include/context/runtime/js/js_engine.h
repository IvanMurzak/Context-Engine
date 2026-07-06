// Backend-agnostic in-process JS host interface — the M3 TypeScript scripting tier's
// foundation (issue #76 / L-61 / R-LANG-002/008). This header is deliberately STL-only (no
// V8 types leak through it): it is the from-source-migration SEAM (L-61). Today the only
// backend is the hybrid rusty_v8 prebuilt host (src/v8_engine.cpp); an owned-from-source V8
// (use_custom_libcxx=false) is a clean backend swap behind this same interface — a separate
// pre-1.0 hardening task (see README.md § From-source migration seam).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace context::runtime::js
{

// A host function exposed to JS: receives `nargs` doubles, returns a double. Doubles only —
// task 2a proves the boundary crosses both ways; a full value-marshalling layer is task 2b.
using HostFunction = double (*)(void* user, const double* args, std::size_t nargs);

// Opaque handle to a resolved global JS function. 0 is invalid.
using FunctionHandle = std::uint64_t;
inline constexpr FunctionHandle kInvalidFunction = 0;

// Opaque handle to a VM-allocated (sandbox-interior) backing store — shape B. 0 is invalid.
using VmBufferHandle = std::uint64_t;
inline constexpr VmBufferHandle kInvalidVmBuffer = 0;

// A VM-allocated (sandbox-interior) backing store — the R-LANG-008 (amended 2026-07-05)
// shape-B storage. `data` is host-readable/writable for the buffer's lifetime; the engine
// owns the allocation and keeps `data` stable across attach/detach cycles. Stock V8
// (V8_ENABLE_SANDBOX, on in every prebuilt) fatally rejects wrapping host-owned memory, so
// "engine-owned" means lifetime/authority — NOT allocator identity. Task 3 builds the
// zero-copy view protocol (per-system ArrayBuffers over this store + the R-LANG-009 detach)
// ON this seam; task 2a only exposes it and proves the allocation is host read/write visible.
struct VmBuffer
{
    VmBufferHandle handle = kInvalidVmBuffer;
    void* data = nullptr;
    std::size_t size = 0;
};

class JsEngine
{
public:
    virtual ~JsEngine() = default;

    virtual std::string_view name() const = 0;  // "v8"
    virtual std::string version() const = 0;     // the embedded V8 version string

    // --- evaluation ------------------------------------------------------------------------
    // Evaluate `code` at global scope. A numeric completion value lands in *numResult when
    // non-null. Returns false and fills `err` on a compile/runtime exception.
    virtual bool eval(std::string_view code, double* numResult, std::string& err) = 0;

    // --- host -> JS ------------------------------------------------------------------------
    // Resolve a global function once; call repeatedly through the cached handle.
    virtual FunctionHandle getFunction(std::string_view globalName) = 0;
    virtual bool callFunction(FunctionHandle fn, const double* args, std::size_t nargs,
                              double* result, std::string& err) = 0;

    // --- JS -> host ------------------------------------------------------------------------
    // Bind a C++ function as globalThis[globalName]; JS calls into it and the return flows back.
    virtual bool bindHostFunction(std::string_view globalName, HostFunction fn, void* user,
                                  std::string& err) = 0;

    // --- shape-B VM-allocated shared memory (R-LANG-008/009 seam) --------------------------
    // Allocate a VM-interior backing store the host can read/write through VmBuffer::data.
    // Freed by freeVmBuffer or automatically at engine teardown.
    virtual bool allocVmBuffer(std::size_t bytes, VmBuffer& out, std::string& err) = 0;
    virtual bool freeVmBuffer(VmBufferHandle h, std::string& err) = 0;

    // --- R-OBS-005 CDP inspector seam ------------------------------------------------------
    // true when v8-inspector.h is wired into this backend (a real, instantiable
    // V8InspectorClient stub exists as the future TS source-mapped debugger's home). The
    // debugger itself is NOT built in task 2a.
    virtual bool inspectorSeamPresent() const = 0;
};

}  // namespace context::runtime::js
