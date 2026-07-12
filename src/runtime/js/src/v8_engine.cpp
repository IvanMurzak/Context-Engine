// Hybrid in-process V8 JS host (issue #76 / L-61 / R-LANG-002/008). The STL-free majority of
// the work uses the real v8:: C++ API (Isolate / HandleScope / Context / Script / String /
// FunctionTemplate — host<->JS both ways); the two STL-crossing ops (platform boot, shape-B
// VM-allocated backing store) go through rusty_v8's extern-"C" v8__* shims (v8_shims.h) — the
// from-source migration seam. Compiled ONLY on toolchains that can link the rusty_v8 prebuilt
// (CONTEXT_JS_HAS_V8); the stub in js_host_stub.cpp covers the rest.

#include "context/runtime/js/js_host.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <libplatform/libplatform.h>
#include <v8-array-buffer.h>
#include <v8-callbacks.h>
#include <v8-context.h>
#include <v8-external.h>
#include <v8-function.h>
#include <v8-function-callback.h>
#include <v8-initialization.h>
#include <v8-isolate.h>
#include <v8-local-handle.h>
#include <v8-persistent-handle.h>
#include <v8-primitive.h>
#include <v8-script.h>
#include <v8-statistics.h>
#include <v8-template.h>
#include <v8-typed-array.h>

#include "inspector_session.h"
#include "v8_shims.h"

namespace context::runtime::js
{
namespace
{

// V8 platform + engine bootstrap is process-wide and once-only. The platform is a deliberate
// reachable process-global (never torn down before process exit — the V8 embedding contract),
// so it is LSan-clean: reachable via a static, not leaked. Multiple V8Engine instances share
// this ONE platform, each owning its own Isolate.
std::once_flag g_v8_once;
v8::Platform* g_platform = nullptr;

void bootstrapV8Once()
{
    std::call_once(g_v8_once, [] {
        // Platform boot is STL-crossing in the real API (std::unique_ptr<Platform> +
        // std::unique_ptr<TracingController>), so it goes through the extern-"C" shim.
        // Boot the MULTI-THREADED default platform (thread_pool_size 0 = hardware concurrency,
        // idle_task_support false). This is the platform V8 expects for a normal Isolate
        // lifecycle: it constructs the DefaultWorkerThreadsTaskRunner pool, and — critically —
        // Isolate teardown (isolate_->Dispose()) posts a delayed heap-release task to that pool
        // via MemoryPool::PostDelayedReleaseTask. The single-threaded default platform has NO
        // background task runner, so that teardown post derefs a null runner and SEGVs on every
        // V8-dependent CI leg — do NOT switch back to it. The benign TSan data race inside this
        // pool (rusty_v8's prebuilt is NOT TSan-instrumented) is handled ORTHOGONALLY by a narrow
        // TSan suppressions file wired into the js_engine ctest for the tsan preset only
        // (src/runtime/js/tsan-suppressions.txt + CMakeLists.txt) — NOT by changing this platform.
        g_platform = v8__Platform__NewDefaultPlatform(/*thread_pool_size=*/0,
                                                      /*idle_task_support=*/false);
        v8::V8::InitializePlatform(g_platform);
        v8::V8::Initialize();
        // rusty_v8 builds V8 with v8_use_external_startup_data=false, so the snapshot + ICU
        // data are embedded in the archive — no external icudtl.dat / snapshot_blob.bin
        // staging is required next to the executable.
    });
}

struct HostEntry
{
    HostFunction fn = nullptr;
    void* user = nullptr;
};

// V8 149's External API requires an ExternalPointerTypeTag on both New() and Value(): objects
// of different C++ types should use different tag values, and the New/Value tags MUST match.
// This host wraps exactly ONE C++ type (HostEntry*), so the documented default tag suffices —
// naming it keeps the two call sites in lockstep.
constexpr v8::ExternalPointerTypeTag kHostEntryTag = v8::kExternalPointerTypeTagDefault;

// Reconstitute a v8::Local from the raw handle-slot pointer a rusty_v8 shim returns (its
// `local_to_ptr` transmutes a Local — a single pointer to the slot — to a raw pointer; this copies
// the bits back). Valid only inside the live HandleScope the slot belongs to. Mirrors rusty_v8's
// own `ptr_to_local`. A bit-copy (not a reinterpret-deref) so it is strict-aliasing-clean and
// tolerates whatever pointer type v8::Local holds internally.
template <class T>
v8::Local<T> ptr_to_local(const T* p)
{
    static_assert(sizeof(v8::Local<T>) == sizeof(const T*),
                  "v8::Local<T> must be a single pointer for the handle-slot bit-copy");
    v8::Local<T> local;
    std::memcpy(&local, &p, sizeof(local));
    return local;
}

// Create the JS typed array of `element` over [byteOffset, byteOffset + count*width) of `ab`. Each
// v8::XxxArray::New signature is STL-free, so it links directly (no shim). Returns an empty Local on
// an unhandled element (never, in practice — the switch is total).
v8::Local<v8::Value> makeTypedArray(v8::Local<v8::ArrayBuffer> ab, ViewElement element,
                                    std::size_t byteOffset, std::size_t count)
{
    switch (element)
    {
    case ViewElement::u8:
        return v8::Uint8Array::New(ab, byteOffset, count);
    case ViewElement::i8:
        return v8::Int8Array::New(ab, byteOffset, count);
    case ViewElement::u16:
        return v8::Uint16Array::New(ab, byteOffset, count);
    case ViewElement::i16:
        return v8::Int16Array::New(ab, byteOffset, count);
    case ViewElement::u32:
        return v8::Uint32Array::New(ab, byteOffset, count);
    case ViewElement::i32:
        return v8::Int32Array::New(ab, byteOffset, count);
    case ViewElement::f32:
        return v8::Float32Array::New(ab, byteOffset, count);
    case ViewElement::f64:
        return v8::Float64Array::New(ab, byteOffset, count);
    case ViewElement::i64:
        return v8::BigInt64Array::New(ab, byteOffset, count);
    case ViewElement::u64:
        return v8::BigUint64Array::New(ab, byteOffset, count);
    }
    return v8::Local<v8::Value>();
}

class V8Engine final : public JsEngine
{
public:
    bool init(std::string& err)
    {
        bootstrapV8Once();
        createParams_.array_buffer_allocator =
            v8::ArrayBuffer::Allocator::NewDefaultAllocator();
        isolate_ = v8::Isolate::New(createParams_);
        if (isolate_ == nullptr)
        {
            err = "v8::Isolate::New failed";
            return false;
        }
        v8::Isolate::Scope isolateScope(isolate_);
        // R-SIM-008 GC-pause observation: bracket every GC event with the embedder prologue/
        // epilogue pair (STL-free, links directly). The callbacks only stamp a steady_clock time
        // and write a fixed ring slot — a GC callback must never allocate or run arbitrary code.
        isolate_->AddGCPrologueCallback(&V8Engine::gcPrologue, this);
        isolate_->AddGCEpilogueCallback(&V8Engine::gcEpilogue, this);
        v8::HandleScope handleScope(isolate_);
        v8::Local<v8::Context> context = v8::Context::New(isolate_);
        context_.Reset(isolate_, context);
        return true;
    }

    ~V8Engine() override
    {
        if (isolate_ != nullptr)
        {
            {
                v8::Isolate::Scope isolateScope(isolate_);
                isolate_->RemoveGCPrologueCallback(&V8Engine::gcPrologue, this);
                isolate_->RemoveGCEpilogueCallback(&V8Engine::gcEpilogue, this);
                v8::HandleScope handleScope(isolate_);
                for (v8::BackingStore* bs : vmBuffers_)
                {
                    if (bs != nullptr)
                    {
                        v8__BackingStore__DELETE(bs);
                    }
                }
                vmBuffers_.clear();
                functions_.clear();
                context_.Reset();
            }
            isolate_->Dispose();
        }
        delete createParams_.array_buffer_allocator;
        // g_platform intentionally outlives every engine (process-global; LSan-reachable).
    }

    std::string_view name() const override { return "v8"; }

    std::string version() const override { return v8::V8::GetVersion(); }

    bool eval(std::string_view code, double* numResult, std::string& err) override
    {
        // Shares the compile+run skeleton with the inspector session's run(); no ScriptOrigin (empty
        // resourceName) and a numeric result is extracted into numResult when present.
        return detail::compileAndRun(isolate_, context_, code, {}, numResult, err);
    }

    FunctionHandle getFunction(std::string_view globalName) override
    {
        v8::Isolate::Scope isolateScope(isolate_);
        v8::HandleScope handleScope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope contextScope(context);

        v8::Local<v8::Value> v;
        if (!context->Global()->Get(context, makeString(globalName)).ToLocal(&v) ||
            !v->IsFunction())
        {
            return kInvalidFunction;
        }
        functions_.emplace_back(isolate_, v.As<v8::Function>());
        return static_cast<FunctionHandle>(functions_.size());
    }

    bool callFunction(FunctionHandle fn, const double* args, std::size_t nargs, double* result,
                      std::string& err) override
    {
        if (fn == kInvalidFunction || fn > functions_.size())
        {
            err = "bad function handle";
            return false;
        }
        if (nargs > kMaxArgs)
        {
            err = "host<->JS seam supports <= 8 args (task 2a)";
            return false;
        }
        v8::Isolate::Scope isolateScope(isolate_);
        v8::HandleScope handleScope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope contextScope(context);
        v8::TryCatch tryCatch(isolate_);

        v8::Local<v8::Value> argv[kMaxArgs];
        for (std::size_t i = 0; i < nargs; ++i)
        {
            argv[i] = v8::Number::New(isolate_, args[i]);
        }
        v8::Local<v8::Value> r;
        if (!functions_[fn - 1]
                 .Get(isolate_)
                 ->Call(context, v8::Undefined(isolate_), static_cast<int>(nargs), argv)
                 .ToLocal(&r))
        {
            err = describe(context, tryCatch);
            return false;
        }
        if (result != nullptr && r->IsNumber())
        {
            *result = r.As<v8::Number>()->Value();
        }
        return true;
    }

    bool bindHostFunction(std::string_view globalName, HostFunction fn, void* user,
                          std::string& err) override
    {
        v8::Isolate::Scope isolateScope(isolate_);
        v8::HandleScope handleScope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope contextScope(context);

        hostEntries_.push_back(std::make_unique<HostEntry>(HostEntry{fn, user}));
        v8::Local<v8::External> data =
            v8::External::New(isolate_, hostEntries_.back().get(), kHostEntryTag);
        v8::Local<v8::Function> jsfn;
        if (!v8::Function::New(context, &V8Engine::trampoline, data).ToLocal(&jsfn))
        {
            err = "v8::Function::New failed";
            return false;
        }
        if (!context->Global()->Set(context, makeString(globalName), jsfn).FromMaybe(false))
        {
            err = "global Set failed";
            return false;
        }
        return true;
    }

    bool allocVmBuffer(std::size_t bytes, VmBuffer& out, std::string& err) override
    {
        v8::Isolate::Scope isolateScope(isolate_);
        // Sandbox-interior stable allocation via the extern-"C" shim: V8 owns the pages inside
        // the sandbox address space; the host reads/writes through Data(). The raw pointer
        // stays valid for the allocation's life (until freeVmBuffer / teardown), so per-system
        // ArrayBuffers (task 3) can attach/detach over it without disturbing the store.
        v8::BackingStore* bs =
            v8__ArrayBuffer__NewBackingStore__with_byte_length(isolate_, bytes);
        if (bs == nullptr)
        {
            err = "v8__ArrayBuffer__NewBackingStore__with_byte_length returned null";
            return false;
        }
        void* data = v8__BackingStore__Data(*bs);
        if (bytes > 0 && data == nullptr)
        {
            v8__BackingStore__DELETE(bs);
            err = "VM backing store reported null data for a non-zero allocation";
            return false;
        }
        vmBuffers_.push_back(bs);
        out.handle = static_cast<VmBufferHandle>(vmBuffers_.size());
        out.data = data;
        out.size = v8__BackingStore__ByteLength(*bs);
        return true;
    }

    bool freeVmBuffer(VmBufferHandle h, std::string& err) override
    {
        if (h == kInvalidVmBuffer || h > vmBuffers_.size() || vmBuffers_[h - 1] == nullptr)
        {
            err = "bad VM buffer handle";
            return false;
        }
        v8__BackingStore__DELETE(vmBuffers_[h - 1]);
        vmBuffers_[h - 1] = nullptr;
        return true;
    }

    bool runSystemView(FunctionHandle fn, const ViewBinding* bindings, std::size_t nbindings,
                       std::string& err) override
    {
        if (fn == kInvalidFunction || fn > functions_.size())
        {
            err = "bad function handle";
            return false;
        }
        if (nbindings > kMaxSystemViews)
        {
            err = "too many system views (> kMaxSystemViews)";
            return false;
        }

        v8::Isolate::Scope isolateScope(isolate_);
        v8::HandleScope handleScope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope contextScope(context);

        // Attach: wrap each VmBuffer's STABLE backing store in a fresh per-system ArrayBuffer +
        // typed view (no copy — the view aliases the exact bytes the host reads/writes). Collect the
        // ArrayBuffers so they can be detached at exit regardless of how the run ends.
        v8::Local<v8::ArrayBuffer> attached[kMaxSystemViews];
        v8::Local<v8::Value> argv[kMaxSystemViews];
        std::size_t created = 0;
        bool ok = true;
        for (std::size_t i = 0; i < nbindings && ok; ++i)
        {
            const ViewBinding& b = bindings[i];
            if (b.buffer == kInvalidVmBuffer || b.buffer > vmBuffers_.size() ||
                vmBuffers_[b.buffer - 1] == nullptr)
            {
                err = "bad VM buffer handle in view binding";
                ok = false;
                break;
            }
            v8::BackingStore* bs = vmBuffers_[b.buffer - 1];
            const std::size_t width = view_element_width(b.element);
            const std::size_t storeBytes = v8__BackingStore__ByteLength(*bs);
            if (b.byteOffset % width != 0)
            {
                err = "view byteOffset is not aligned to the element width";
                ok = false;
                break;
            }
            if (b.byteOffset > storeBytes || b.count * width > storeBytes - b.byteOffset)
            {
                err = "view slice out of range for its VM buffer";
                ok = false;
                break;
            }
            // Hand the raw store to V8 as a NON-OWNING shared_ptr (null control block): V8 co-owns
            // the ArrayBuffer's store but never frees it — the host stays sole owner (see v8_shims.h
            // BackingStoreSharedPtrImage). Detach at exit thus kills the JS view, never the store.
            BackingStoreSharedPtrImage image;
            image.ptr = bs;
            image.cntrl = nullptr;
            const v8::ArrayBuffer* rawAb =
                v8__ArrayBuffer__New__with_backing_store(isolate_, &image);
            if (rawAb == nullptr)
            {
                err = "v8__ArrayBuffer__New__with_backing_store returned null";
                ok = false;
                break;
            }
            v8::Local<v8::ArrayBuffer> ab = ptr_to_local(rawAb);
            if (!ab->IsDetachable())
            {
                err = "per-system ArrayBuffer reports IsDetachable() == false";
                ok = false;
                break;
            }
            v8::Local<v8::Value> ta = makeTypedArray(ab, b.element, b.byteOffset, b.count);
            if (ta.IsEmpty())
            {
                err = "typed-array view creation failed";
                ok = false;
                break;
            }
            attached[created] = ab;
            argv[created] = ta;
            ++created;
        }

        // Run the executor exactly once — only if every binding attached cleanly.
        if (ok)
        {
            v8::TryCatch tryCatch(isolate_);
            v8::Local<v8::Value> r;
            if (!functions_[fn - 1]
                     .Get(isolate_)
                     ->Call(context, v8::Undefined(isolate_), static_cast<int>(created), argv)
                     .ToLocal(&r))
            {
                err = describe(context, tryCatch);
                ok = false;
            }
        }

        // R-LANG-009 end-of-system: DETACH/neuter every view we created — even on an exec throw or a
        // mid-attach failure, a retained view must never outlive its invocation. Best-effort: a
        // detach failure is recorded but does not stop neutering the rest.
        for (std::size_t i = 0; i < created; ++i)
        {
            if (!attached[i]->Detach(v8::Local<v8::Value>()).FromMaybe(false) && ok)
            {
                err = "ArrayBuffer::Detach returned Nothing/false at system exit";
                ok = false;
            }
        }
        return ok;
    }

    // --- R-SIM-008 JS-tier GC discipline (M6 X1) --------------------------------------------
    //
    // The scheduled inter-tick GC window. The collection primitive is LowMemoryNotification()
    // — the embedder-sanctioned "collect all available garbage NOW" (STL-free, links directly;
    // V8 14.x removed the idle-task GC machinery, so idle notifications are not an option). The
    // pump phase drains the platform's queued foreground tasks via the REAL
    // v8::platform::PumpMessageLoop (STL-free signature; the platform g_platform is a genuine
    // v8::platform::DefaultPlatform from rusty_v8's v8__Platform__NewDefaultPlatform, and the
    // symbol is linked into the archive — binding.cc's v8__Platform__PumpMessageLoop wraps it),
    // giving V8's finalization/sweeping tasks their only service point in this embedder.
    bool gcWindow(const GcWindowOptions& options, GcWindowResult& result,
                  std::string& err) override
    {
        if (!std::isfinite(options.budget_ms) || options.budget_ms <= 0.0)
        {
            err = "gc window budget_ms must be finite and > 0";
            return false;
        }
        v8::Isolate::Scope isolateScope(isolate_);
        const auto t0 = std::chrono::steady_clock::now();
        auto elapsedMs = [&t0]() {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                             t0)
                .count();
        };

        v8::HeapStatistics hs;
        isolate_->GetHeapStatistics(&hs);
        result = GcWindowResult{};
        result.heap_used_before = static_cast<std::uint64_t>(hs.used_heap_size());

        // The growth baseline is set lazily at the FIRST window, so trigger_bytes measures churn
        // accumulated since inter-tick windows began — not the isolate's static boot-heap floor.
        if (!gcBaselineSet_)
        {
            gcBaseline_ = result.heap_used_before;
            gcBaselineSet_ = true;
        }

        gcInWindow_ = true;
        // Growth is compared by subtraction (never `gcBaseline_ + trigger_bytes`): the sum wraps
        // for a near-UINT64_MAX trigger, which would flip "effectively never collect" into
        // "collect every window".
        const bool grewPastTrigger = options.trigger_bytes > 0 &&
                                     result.heap_used_before >= gcBaseline_ &&
                                     result.heap_used_before - gcBaseline_ >= options.trigger_bytes;
        if (options.force_collect || grewPastTrigger)
        {
            // Synchronous full collection, deliberately NOT preempted by the budget: the pause is
            // MEASURED (it lands in the pause ring, attributed in_window) and the R-LANG-012
            // budget verdict is made from the channel — L-47's "measurable to be budgeted".
            isolate_->LowMemoryNotification();
            result.collected = true;
        }
        // Pump phase: drain queued foreground tasks within the remaining budget. Hard iteration
        // cap so a pathological task flood (or a stalled clock) cannot wedge the tick loop.
        while (elapsedMs() < options.budget_ms && result.tasks_pumped < kMaxWindowTasks)
        {
            if (!v8::platform::PumpMessageLoop(g_platform, isolate_,
                                               v8::platform::MessageLoopBehavior::kDoNotWait))
            {
                break;
            }
            ++result.tasks_pumped;
        }
        gcInWindow_ = false;

        if (result.collected || result.tasks_pumped > 0)
        {
            // Only a collection or a pumped task can have moved the heap; skip the second
            // heap-stats walk on the (per-tick) no-op path.
            isolate_->GetHeapStatistics(&hs);
            result.heap_used_after = static_cast<std::uint64_t>(hs.used_heap_size());
        }
        else
        {
            result.heap_used_after = result.heap_used_before;
        }
        if (result.collected)
        {
            gcBaseline_ = result.heap_used_after;
        }
        result.window_ms = elapsedMs();
        return true;
    }

    std::size_t drainGcPauses(GcPause* out, std::size_t cap) override
    {
        std::size_t n = 0;
        while (n < cap && gcRingCount_ > 0)
        {
            out[n++] = gcRing_[gcRingHead_];
            gcRingHead_ = (gcRingHead_ + 1) % kGcRingCap;
            --gcRingCount_;
        }
        return n;
    }

    std::uint64_t gcPausesDropped() const override { return gcDropped_; }

    bool gcHeapStats(GcHeapStats& out, std::string& err) override
    {
        (void)err;
        v8::Isolate::Scope isolateScope(isolate_);
        v8::HeapStatistics hs;
        isolate_->GetHeapStatistics(&hs);
        out.used_bytes = static_cast<std::uint64_t>(hs.used_heap_size());
        out.total_bytes = static_cast<std::uint64_t>(hs.total_heap_size());
        out.external_bytes = static_cast<std::uint64_t>(hs.external_memory());
        return true;
    }

    // The V8 backend always carries the interactive CDP inspector seam (attachInspector below wires
    // a real V8Inspector session). This is a compile-time fact of THIS TU (it exists only under
    // CONTEXT_JS_HAS_V8), so it is a constant `true` rather than a runtime flag.
    //
    // Historically this returned `inspector_ != nullptr`, where `inspector_` was a
    // detail::V8InspectorSeam — a client that DIRECTLY subclassed v8_inspector::V8InspectorClient
    // from this system-STL TU. That subclass was the ONLY thing in our code that forced the compiler
    // to emit the v8_inspector::V8InspectorClient vtable + its INLINE virtual defaults (valueSubtype,
    // deepSerialize, … — each returning a std::unique_ptr<StringBuffer> BY VALUE) as weak symbols in
    // our object, compiled against the SYSTEM libc++. The rusty_v8 prebuilt is use_custom_libcxx=true
    // and defines the SAME weak vtable/inline-defaults against its Chromium libc++. When the hybrid
    // link deduplicated those weak symbols and kept OUR system-STL `valueSubtype`, an archive-side
    // caller (value-mirror.cc: `clientFor(context)->valueSubtype(object)`) received a system-libc++
    // std::unique_ptr<StringBuffer> whose bytes it reinterpreted as a Chromium-libc++ unique_ptr — a
    // spuriously non-null StringBuffer* whose `->string()` then dereferenced @0x10 during the
    // debugger-break call-frame walk (issue #104, the attach-time SEGV). The live inspector client is
    // the archive-vtable SeamClient in inspector.cpp; nothing needs a directly-subclassed client, so
    // V8InspectorSeam (and inspector_seam.h) are removed and this returns the compile-time constant.
    bool inspectorSeamPresent() const override { return true; }

    std::unique_ptr<InspectorSession> attachInspector(std::string& err) override
    {
        // The interactive CDP attach (issue #94), built out from the task-2a seam. Shares THIS
        // engine's Isolate + Context; the returned session must not outlive the engine.
        return detail::createInspectorSession(isolate_, context_, err);
    }

private:
    static constexpr std::size_t kMaxArgs = 8;
    // R-SIM-008 GC observation plumbing. Ring capacity bounds the undrained-pause backlog (the
    // profiler drains every tick, so 512 is generous headroom); kMaxWindowTasks bounds the window's
    // pump loop against a task flood. kGcTypeSlots covers V8's five GCType bits (v8-callbacks.h).
    static constexpr std::size_t kGcRingCap = 512;
    static constexpr std::uint64_t kMaxWindowTasks = 1024;
    static constexpr std::size_t kGcTypeSlots = 5;

    // Map a (single-bit) GCType to its start-time slot; kGcTypeSlots for an unexpected value.
    static std::size_t gcTypeSlot(v8::GCType type)
    {
        switch (type)
        {
        case v8::kGCTypeScavenge:
            return 0;
        case v8::kGCTypeMinorMarkSweep:
            return 1;
        case v8::kGCTypeMarkSweepCompact:
            return 2;
        case v8::kGCTypeIncrementalMarking:
            return 3;
        case v8::kGCTypeProcessWeakCallbacks:
            return 4;
        default:
            return kGcTypeSlots;
        }
    }

    // GC prologue/epilogue (fire on the isolate thread, bracketing each GC event). Allocation-free
    // by contract: stamp a steady_clock time in the per-type slot, and on the epilogue write one
    // fixed ring slot (drop-newest + count when the ring is full — the profiler reports the loss).
    static void gcPrologue(v8::Isolate* /*isolate*/, v8::GCType type, v8::GCCallbackFlags /*flags*/,
                           void* data)
    {
        auto* self = static_cast<V8Engine*>(data);
        const std::size_t slot = gcTypeSlot(type);
        if (slot < kGcTypeSlots)
        {
            self->gcStart_[slot] = std::chrono::steady_clock::now();
        }
    }

    static void gcEpilogue(v8::Isolate* /*isolate*/, v8::GCType type, v8::GCCallbackFlags /*flags*/,
                           void* data)
    {
        auto* self = static_cast<V8Engine*>(data);
        const std::size_t slot = gcTypeSlot(type);
        if (slot >= kGcTypeSlots || self->gcStart_[slot] == std::chrono::steady_clock::time_point{})
        {
            return; // unmatched epilogue (no recorded prologue) — nothing trustworthy to record
        }
        GcPause pause;
        pause.duration_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - self->gcStart_[slot])
                                .count();
        pause.kind = static_cast<std::uint32_t>(type);
        pause.in_window = self->gcInWindow_;
        self->gcStart_[slot] = std::chrono::steady_clock::time_point{};
        if (self->gcRingCount_ == kGcRingCap)
        {
            ++self->gcDropped_;
            return;
        }
        self->gcRing_[(self->gcRingHead_ + self->gcRingCount_) % kGcRingCap] = pause;
        ++self->gcRingCount_;
    }

    static void trampoline(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        auto* entry =
            static_cast<HostEntry*>(info.Data().As<v8::External>()->Value(kHostEntryTag));
        double args[kMaxArgs];
        // task-2a doubles-only boundary: JS->host silently clamps to the first kMaxArgs
        // (callFunction on the host->JS side rejects instead). Extra args are intentionally
        // dropped here; a richer arity/type contract is deferred to the task-3 view protocol.
        const int n =
            info.Length() > static_cast<int>(kMaxArgs) ? static_cast<int>(kMaxArgs) : info.Length();
        v8::Local<v8::Context> ctx = info.GetIsolate()->GetCurrentContext();
        for (int i = 0; i < n; ++i)
        {
            v8::Local<v8::Value> v = info[i];
            args[i] =
                v->IsNumber() ? v.As<v8::Number>()->Value() : v->NumberValue(ctx).FromMaybe(0.0);
        }
        const double r = entry->fn(entry->user, args, static_cast<std::size_t>(n));
        info.GetReturnValue().Set(r);
    }

    v8::Local<v8::String> makeString(std::string_view s)
    {
        return v8::String::NewFromUtf8(isolate_, s.data(), v8::NewStringType::kInternalized,
                                       static_cast<int>(s.size()))
            .ToLocalChecked();
    }

    std::string describe(v8::Local<v8::Context> context, const v8::TryCatch& tryCatch)
    {
        // Prefer the exception's FULL stack trace (so the R-OBS-005 TS-source-map remapper sees the
        // raw JS frames and can resolve them to authored .ts positions), falling back to the bare
        // message / stringified non-Error primitive. Shared with the inspector session's run() path.
        return detail::describeException(isolate_, context, tryCatch);
    }

    v8::Isolate::CreateParams createParams_;
    v8::Isolate* isolate_ = nullptr;
    v8::Global<v8::Context> context_;
    std::vector<v8::Global<v8::Function>> functions_;
    std::vector<std::unique_ptr<HostEntry>> hostEntries_;
    std::vector<v8::BackingStore*> vmBuffers_;  // owned; freed via v8__BackingStore__DELETE

    // R-SIM-008 GC discipline state (single-threaded like the rest of the engine: the GC
    // callbacks fire on the isolate thread, never concurrently with the host's calls).
    std::array<GcPause, kGcRingCap> gcRing_{};
    std::size_t gcRingHead_ = 0;   // oldest undrained record
    std::size_t gcRingCount_ = 0;  // undrained records in the ring
    std::uint64_t gcDropped_ = 0;  // records lost to ring overflow (monotonic)
    std::array<std::chrono::steady_clock::time_point, kGcTypeSlots> gcStart_{};
    bool gcInWindow_ = false;          // true while gcWindow() is on the stack (attribution)
    std::uint64_t gcBaseline_ = 0;     // heap-used baseline for the trigger_bytes growth policy
    bool gcBaselineSet_ = false;       // baseline initialized lazily at the first window
};

}  // namespace

bool v8BackendAvailable() { return true; }

std::unique_ptr<JsEngine> createV8Engine(std::string& err)
{
    auto engine = std::make_unique<V8Engine>();
    if (!engine->init(err))
    {
        return nullptr;
    }
    return engine;
}

}  // namespace context::runtime::js
