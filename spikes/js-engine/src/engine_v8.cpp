// V8 backend for the §2d embedding seam. THROWAWAY spike code.
//
// V8's platform/flag initialization is process-wide, so exactly ONE V8 engine (one config —
// JIT or --jitless) may exist per process; the bench driver runs each config in a fresh process.

#include "ctx/js_engine.hpp"

#include <libplatform/libplatform.h>
#include <v8-array-buffer.h>
#include <v8-context.h>
#include <v8-external.h>
#include <v8-function.h>
#include <v8-initialization.h>
#include <v8-isolate.h>
#include <v8-local-handle.h>
#include <v8-persistent-handle.h>
#include <v8-primitive.h>
#include <v8-script.h>
#include <v8-template.h>

#include <vector>

namespace ctx::spike {
namespace {

struct HostEntry {
    HostFunction fn = nullptr;
    void* user = nullptr;
};

std::unique_ptr<v8::Platform> g_platform;  // process-lifetime
bool g_initialized = false;

class V8Engine final : public JsEngine {
public:
    bool init(const EngineConfig& cfg, std::string& err) {
        if (g_initialized) {
            err = "spike restriction: one V8 engine per process (flags are process-wide)";
            return false;
        }
        g_initialized = true;
        if (cfg.jitless) v8::V8::SetFlagsFromString("--jitless");
        if (!cfg.exePath.empty()) v8::V8::InitializeICUDefaultLocation(cfg.exePath.c_str());
        g_platform = v8::platform::NewDefaultPlatform();
        v8::V8::InitializePlatform(g_platform.get());
        v8::V8::Initialize();

        createParams_.array_buffer_allocator =
            v8::ArrayBuffer::Allocator::NewDefaultAllocator();
        isolate_ = v8::Isolate::New(createParams_);
        if (!isolate_) {
            err = "v8::Isolate::New failed";
            return false;
        }
        isolate_->Enter();
        v8::HandleScope scope(isolate_);
        v8::Local<v8::Context> context = v8::Context::New(isolate_);
        context_.Reset(isolate_, context);
        context->Enter();
        return true;
    }

    ~V8Engine() override {
        if (isolate_) {
            {
                v8::HandleScope scope(isolate_);
                context_.Get(isolate_)->Exit();
            }
            functions_.clear();
            buffers_.clear();
            context_.Reset();
            isolate_->Exit();
            isolate_->Dispose();
        }
        delete createParams_.array_buffer_allocator;
        // Platform stays alive until process exit (spike: one engine per process).
    }

    std::string_view name() const override { return "v8"; }

    std::string version() const override { return v8::V8::GetVersion(); }

    bool eval(std::string_view code, double* numResult, std::string& err) override {
        v8::HandleScope scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::TryCatch tryCatch(isolate_);
        v8::Local<v8::String> src;
        if (!v8::String::NewFromUtf8(isolate_, code.data(), v8::NewStringType::kNormal,
                                     static_cast<int>(code.size()))
                 .ToLocal(&src)) {
            err = "source string allocation failed";
            return false;
        }
        v8::Local<v8::Script> script;
        if (!v8::Script::Compile(context, src).ToLocal(&script)) {
            err = describe(tryCatch);
            return false;
        }
        v8::Local<v8::Value> result;
        if (!script->Run(context).ToLocal(&result)) {
            err = describe(tryCatch);
            return false;
        }
        if (numResult && result->IsNumber()) {
            *numResult = result.As<v8::Number>()->Value();
        }
        return true;
    }

    FunctionHandle getFunction(std::string_view globalName) override {
        v8::HandleScope scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Local<v8::Value> v;
        if (!context->Global()
                 ->Get(context, makeString(globalName))
                 .ToLocal(&v) ||
            !v->IsFunction()) {
            return kInvalidFunction;
        }
        functions_.emplace_back(isolate_, v.As<v8::Function>());
        return static_cast<FunctionHandle>(functions_.size());
    }

    bool callFunction(FunctionHandle fn, const double* args, std::size_t nargs, double* result,
                      std::string& err) override {
        if (fn == kInvalidFunction || fn > functions_.size()) {
            err = "bad function handle";
            return false;
        }
        if (nargs > 8) {
            err = "spike seam supports <= 8 args";
            return false;
        }
        v8::HandleScope scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::TryCatch tryCatch(isolate_);
        v8::Local<v8::Value> argv[8];
        for (std::size_t i = 0; i < nargs; i++) {
            argv[i] = v8::Number::New(isolate_, args[i]);
        }
        v8::Local<v8::Value> r;
        if (!functions_[fn - 1]
                 .Get(isolate_)
                 ->Call(context, v8::Undefined(isolate_), static_cast<int>(nargs), argv)
                 .ToLocal(&r)) {
            err = describe(tryCatch);
            return false;
        }
        if (result && r->IsNumber()) *result = r.As<v8::Number>()->Value();
        return true;
    }

    bool bindHostFunction(std::string_view globalName, HostFunction fn, void* user) override {
        hostEntries_.push_back(std::make_unique<HostEntry>(HostEntry{fn, user}));
        v8::HandleScope scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Local<v8::External> data =
            v8::External::New(isolate_, hostEntries_.back().get());
        v8::Local<v8::Function> jsfn;
        if (!v8::Function::New(context, &V8Engine::trampoline, data).ToLocal(&jsfn)) {
            return false;
        }
        return context->Global()
            ->Set(context, makeString(globalName), jsfn)
            .FromMaybe(false);
    }

    BufferHandle attachHostBuffer(std::string_view globalName, void* data, std::size_t bytes,
                                  std::string& err) override {
#ifdef V8_ENABLE_SANDBOX
        // MEASURED FATALITY, not a guess: on this sandbox-enabled build (the default for
        // Chrome-configuration/prebuilt V8), ArrayBuffer::NewBackingStore over external host
        // memory aborts the process: "When the V8 Sandbox is enabled, ArrayBuffer backing
        // stores must be allocated inside the sandbox address space." Zero-copy over
        // engine-owned host memory is impossible; use the VM-allocated shared shape instead.
        (void)globalName;
        (void)data;
        (void)bytes;
        err = "V8 sandbox forbids external backing stores (fatal abort observed); "
              "wrap-host-memory zero-copy is impossible on sandbox-enabled V8 builds";
        return kInvalidBuffer;
#else
        v8::HandleScope scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        std::unique_ptr<v8::BackingStore> store = v8::ArrayBuffer::NewBackingStore(
            data, bytes, v8::BackingStore::EmptyDeleter, nullptr);
        v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(isolate_, std::move(store));
        if (!ab->IsDetachable()) {
            err = "externally-backed ArrayBuffer reports IsDetachable() == false";
            return kInvalidBuffer;
        }
        if (!context->Global()->Set(context, makeString(globalName), ab).FromMaybe(false)) {
            err = "global Set failed";
            return kInvalidBuffer;
        }
        return storeBuffer(ab);
#endif
    }

    AllocHandle allocSharedBuffer(std::size_t bytes, void** outData, std::string& err) override {
        // Sandbox-interior stable allocation: V8 owns the pages (inside the sandbox address
        // space); the host reads/writes through Data(). The shared_ptr keeps the memory alive
        // across attach/detach cycles — detach kills JS views, never the allocation.
        std::shared_ptr<v8::BackingStore> store =
            v8::ArrayBuffer::NewBackingStore(isolate_, bytes);
        if (!store || !store->Data()) {
            err = "NewBackingStore(isolate, bytes) failed";
            return kInvalidAlloc;
        }
        if (outData) *outData = store->Data();
        allocs_.push_back(std::move(store));
        return static_cast<AllocHandle>(allocs_.size());
    }

    BufferHandle attachSharedBuffer(std::string_view globalName, AllocHandle alloc,
                                    std::string& err) override {
        if (alloc == kInvalidAlloc || alloc > allocs_.size() || !allocs_[alloc - 1]) {
            err = "bad alloc handle";
            return kInvalidBuffer;
        }
        v8::HandleScope scope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(isolate_, allocs_[alloc - 1]);
        if (!ab->IsDetachable()) {
            err = "shared-backing ArrayBuffer reports IsDetachable() == false";
            return kInvalidBuffer;
        }
        if (!context->Global()->Set(context, makeString(globalName), ab).FromMaybe(false)) {
            err = "global Set failed";
            return kInvalidBuffer;
        }
        return storeBuffer(ab);
    }

    bool freeSharedBuffer(AllocHandle alloc, std::string& err) override {
        if (alloc == kInvalidAlloc || alloc > allocs_.size() || !allocs_[alloc - 1]) {
            err = "bad alloc handle";
            return false;
        }
        allocs_[alloc - 1].reset();
        return true;
    }

    bool detachHostBuffer(BufferHandle h, std::string& err) override {
        if (h == kInvalidBuffer || h > buffers_.size() || buffers_[h - 1].IsEmpty()) {
            err = "bad buffer handle";
            return false;
        }
        v8::HandleScope scope(isolate_);
        v8::Local<v8::ArrayBuffer> ab = buffers_[h - 1].Get(isolate_);
        // R-LANG-009: detach with no detach-key set — views die, host memory untouched.
        if (!ab->Detach(v8::Local<v8::Value>()).FromMaybe(false)) {
            err = "ArrayBuffer::Detach returned Nothing/false";
            return false;
        }
        buffers_[h - 1].Reset();
        bufferFreeList_[h - 1] = freeSlot_;
        freeSlot_ = h;
        return true;
    }

    void collectGarbage() override { isolate_->LowMemoryNotification(); }

private:
    static void trampoline(const v8::FunctionCallbackInfo<v8::Value>& info) {
        auto* entry = static_cast<HostEntry*>(info.Data().As<v8::External>()->Value());
        double args[8];
        const int n = info.Length() > 8 ? 8 : info.Length();
        for (int i = 0; i < n; i++) {
            v8::Local<v8::Value> v = info[i];
            args[i] = v->IsNumber()
                          ? v.As<v8::Number>()->Value()
                          : v->NumberValue(info.GetIsolate()->GetCurrentContext()).FromMaybe(0.0);
        }
        const double r = entry->fn(entry->user, args, static_cast<std::size_t>(n));
        info.GetReturnValue().Set(r);
    }

    BufferHandle storeBuffer(v8::Local<v8::ArrayBuffer> ab) {
        if (freeSlot_ != 0) {
            const std::size_t slot = freeSlot_ - 1;
            freeSlot_ = bufferFreeList_[slot];
            buffers_[slot].Reset(isolate_, ab);
            return static_cast<BufferHandle>(slot + 1);
        }
        buffers_.emplace_back(isolate_, ab);
        bufferFreeList_.push_back(0);
        return static_cast<BufferHandle>(buffers_.size());
    }

    v8::Local<v8::String> makeString(std::string_view s) {
        return v8::String::NewFromUtf8(isolate_, s.data(), v8::NewStringType::kInternalized,
                                       static_cast<int>(s.size()))
            .ToLocalChecked();
    }

    std::string describe(const v8::TryCatch& tryCatch) {
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Local<v8::Value> ex = tryCatch.Exception();
        if (ex.IsEmpty()) return "<no exception>";
        v8::Local<v8::String> str;
        if (!ex->ToString(context).ToLocal(&str)) return "<unprintable exception>";
        v8::String::Utf8Value utf8(isolate_, str);
        return *utf8 ? *utf8 : "<utf8 conversion failed>";
    }

    v8::Isolate::CreateParams createParams_;
    v8::Isolate* isolate_ = nullptr;
    v8::Global<v8::Context> context_;
    std::vector<v8::Global<v8::Function>> functions_;
    std::vector<std::unique_ptr<HostEntry>> hostEntries_;
    std::vector<v8::Global<v8::ArrayBuffer>> buffers_;
    std::vector<BufferHandle> bufferFreeList_;
    std::vector<std::shared_ptr<v8::BackingStore>> allocs_;
    BufferHandle freeSlot_ = 0;
};

}  // namespace

std::unique_ptr<JsEngine> createV8Engine(const EngineConfig& cfg, std::string& err) {
    auto engine = std::make_unique<V8Engine>();
    if (!engine->init(cfg, err)) return nullptr;
    return engine;
}

}  // namespace ctx::spike
