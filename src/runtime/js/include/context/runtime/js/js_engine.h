// Backend-agnostic in-process JS host interface — the M3 TypeScript scripting tier's
// foundation (issue #76 / L-61 / R-LANG-002/008). This header is deliberately STL-only (no
// V8 types leak through it): it is the from-source-migration SEAM (L-61). Today the only
// backend is the hybrid rusty_v8 prebuilt host (src/v8_engine.cpp); an owned-from-source V8
// (use_custom_libcxx=false) is a clean backend swap behind this same interface — a separate
// pre-1.0 hardening task (see README.md § From-source migration seam).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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

// --- zero-copy view protocol (R-LANG-009) ------------------------------------------------------
//
// The element type of a per-system typed-array view over a VmBuffer slice. Mirrors the JS
// TypedArray families 1:1; the width drives byte-offset/length validation. This enum is
// deliberately storage-only (no dependency on the declarative component vocabulary) so the JS
// host stays layering-clean — the (query, executor) tier maps a component field's x-ctx-storage
// scalar onto one of these when it binds a view (a follow-up task).
enum class ViewElement
{
    u8,   // Uint8Array
    i8,   // Int8Array
    u16,  // Uint16Array
    i16,  // Int16Array
    u32,  // Uint32Array
    i32,  // Int32Array
    f32,  // Float32Array
    f64,  // Float64Array
    i64,  // BigInt64Array
    u64,  // BigUint64Array
};

// Byte width of one ViewElement (1/2/4/8). Free function so both the host and its callers agree
// on the offset/length arithmetic without reaching into the backend.
[[nodiscard]] std::size_t view_element_width(ViewElement e) noexcept;

// One zero-copy view request handed to a system: expose the slice
// [byteOffset, byteOffset + count * width(element)) of VmBuffer `buffer` to JS as a typed array of
// `element`. The view ALIASES the VmBuffer's stable backing store (no copy) and is valid ONLY for
// the single runSystemView invocation it is passed to. `byteOffset` must be a multiple of the
// element width (V8 rejects a misaligned typed-array offset).
struct ViewBinding
{
    VmBufferHandle buffer = kInvalidVmBuffer;
    ViewElement element = ViewElement::u8;
    std::size_t byteOffset = 0;  // start offset into the VmBuffer, aligned to the element width
    std::size_t count = 0;       // number of ELEMENTS the view spans (not bytes)
};

// The most view bindings one system invocation may take (the R-LANG-012 per-archetype view-set is
// small — a handful of declared component columns). A generous cap that bounds the per-system
// ArrayBuffer create/detach churn; exceeding it is a caller error, not a silent clamp.
inline constexpr std::size_t kMaxSystemViews = 16;

// --- R-OBS-005 interactive CDP debug session (L-61: "config, not building a debugger") ----------
//
// A minimal, SYNCHRONOUS Chrome DevTools Protocol (CDP) session over V8's in-box inspector
// (v8-inspector.h — the same protocol Chrome DevTools / VS Code js-debug speak natively). The
// embedder drives it with plain CDP JSON: dispatch() feeds one command IN, and every response +
// notification the inspector produces is delivered to the sink installed via onMessage(). When a
// breakpoint (or a `debugger;` statement) pauses execution, V8 re-enters the embedder through a
// NESTED pause loop: the pump installed via onPause() is called repeatedly and dispatches the CDP
// commands (Debugger.resume / Debugger.stepOver / ...) that eventually resume. The whole loop is
// single-threaded and deterministic — no socket, no background thread — so a test (or an in-process
// client) can drive source-mapped breakpoints + stepping over the exact protocol a real
// DevTools/VS-Code client speaks. Kept STL-only (no V8 type leaks) so this header stays the
// from-source-migration seam; the implementation is the V8 backend (CI-only for its link).
//
// A loopback/websocket transport + a `/json` discovery target (for an OUT-OF-PROCESS DevTools) and a
// DAP bridge are a documented follow-up — this seam is the in-process CORE that transport wraps.
class InspectorSession
{
public:
    virtual ~InspectorSession() = default;

    // Install the outbound CDP sink (each message a UTF-8 JSON string: a response or a
    // notification). Set it BEFORE dispatch()/run() so the scriptParsed + paused notifications are
    // observed. A later call replaces the sink; a null sink drops messages.
    virtual void onMessage(std::function<void(std::string_view)> sink) = 0;

    // Install the pause pump: called re-entrantly on each iteration of the nested pause loop V8 runs
    // while stopped at a breakpoint. It must dispatch() the next CDP command(s); returning true (or
    // dispatching a resume/step command, which V8 signals internally) exits the loop and resumes
    // execution. The loop is bounded, so a pump that never resumes cannot hang the process.
    virtual void onPause(std::function<bool()> pump) = 0;

    // Feed ONE CDP command JSON (e.g. {"id":1,"method":"Debugger.enable"}). Returns false + fills
    // `err` only when the session rejects the message before handing it to V8 (empty message);
    // V8 reports a protocol-level error as a normal CDP error RESPONSE through the onMessage sink,
    // not through this return value. Responses/notifications arrive via the sink.
    virtual bool dispatch(std::string_view cdpMessage, std::string& err) = 0;

    // Compile + run `code` under the resource name `resourceName` (its CDP script URL, so
    // Debugger.setBreakpointByUrl can target it and Debugger.scriptParsed reports it). Runs to
    // completion, driving the onPause pump at any breakpoint hit. Returns false + fills `err` (the
    // V8 error.stack) on an uncaught throw.
    virtual bool run(std::string_view code, std::string_view resourceName, std::string& err) = 0;
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

    // --- R-LANG-009 zero-copy view lifetime & invalidation protocol ------------------------
    //
    // Run `exec` as ONE system invocation over zero-copy views. For each binding a typed array is
    // created OVER the VmBuffer's stable backing store (NO copy — the JS view aliases the exact
    // bytes the host reads/writes through VmBuffer::data) and passed to `exec` as a positional
    // argument, in binding order. `exec` runs exactly once; then EVERY view's ArrayBuffer is
    // detached/neutered before this returns — the R-LANG-009 hard-gate. A reference to a view that
    // JS retained past the call (e.g. stashed on globalThis) is therefore dead afterwards: its
    // byteLength is 0, element reads yield undefined, and constructing a new view over its
    // ArrayBuffer throws. The VmBuffer's bytes are UNTOUCHED by detach (the host keeps ownership of
    // the backing store), so the host reads JS's writes back through VmBuffer::data after the call.
    //
    // Detach runs even when `exec` throws (a system that faults still may not leak a live view);
    // the thrown message is reported in `err` and the call returns false. Returns false + fills
    // `err` on a bad handle, a misaligned/out-of-range binding, > kMaxSystemViews bindings, or a
    // wrap/detach failure. `nbindings == 0` is allowed (runs `exec` with no view args).
    //
    // Structural changes (add/remove component, entity create/destroy) MUST NOT be applied while a
    // view is live — memory may move under the alias. Deferring them to an end-of-system command
    // buffer is the (query, executor) tier's responsibility (a follow-up task), not this seam's.
    virtual bool runSystemView(FunctionHandle exec, const ViewBinding* bindings,
                               std::size_t nbindings, std::string& err) = 0;

    // --- R-OBS-005 CDP inspector seam ------------------------------------------------------
    // true when v8-inspector.h is wired into this backend (a real, instantiable
    // V8InspectorClient stub exists as the future TS source-mapped debugger's home). The
    // debugger itself is NOT built in task 2a.
    virtual bool inspectorSeamPresent() const = 0;

    // --- R-OBS-005 interactive CDP inspector attach ----------------------------------------
    // Attach a CDP debug session over V8's in-box inspector (see InspectorSession above): the
    // interactive breakpoint/stepping half of R-OBS-005, built out from the task-2a seam. The
    // returned session shares this engine's Isolate + Context and MUST NOT outlive the engine.
    // Returns nullptr + fills `err` only when the backend has no inspector.
    [[nodiscard]] virtual std::unique_ptr<InspectorSession> attachInspector(std::string& err) = 0;
};

}  // namespace context::runtime::js
