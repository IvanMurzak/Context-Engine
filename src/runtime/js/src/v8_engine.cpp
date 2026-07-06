// Hybrid in-process V8 JS host (issue #76 / L-61 / R-LANG-002/008). The STL-free majority of
// the work uses the real v8:: C++ API (Isolate / HandleScope / Context / Script / String /
// FunctionTemplate — host<->JS both ways); the two STL-crossing ops (platform boot, shape-B
// VM-allocated backing store) go through rusty_v8's extern-"C" v8__* shims (v8_shims.h) — the
// from-source migration seam. Compiled ONLY on toolchains that can link the rusty_v8 prebuilt
// (CONTEXT_JS_HAS_V8); the stub in js_host_stub.cpp covers the rest.

#include "context/runtime/js/js_host.h"

#include <memory>
#include <mutex>
#include <vector>

#include <v8-array-buffer.h>
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
#include <v8-template.h>

#include "inspector_seam.h"
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
        g_platform = v8__Platform__NewDefaultPlatform(0, /*idle_task_support=*/false);
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
        v8::HandleScope handleScope(isolate_);
        v8::Local<v8::Context> context = v8::Context::New(isolate_);
        context_.Reset(isolate_, context);
        inspector_ = std::make_unique<detail::V8InspectorSeam>();
        return true;
    }

    ~V8Engine() override
    {
        if (isolate_ != nullptr)
        {
            {
                v8::Isolate::Scope isolateScope(isolate_);
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
        v8::Isolate::Scope isolateScope(isolate_);
        v8::HandleScope handleScope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope contextScope(context);
        v8::TryCatch tryCatch(isolate_);

        v8::Local<v8::String> src;
        if (!v8::String::NewFromUtf8(isolate_, code.data(), v8::NewStringType::kNormal,
                                     static_cast<int>(code.size()))
                 .ToLocal(&src))
        {
            err = "source string allocation failed";
            return false;
        }
        v8::Local<v8::Script> script;
        if (!v8::Script::Compile(context, src).ToLocal(&script))
        {
            err = describe(context, tryCatch);
            return false;
        }
        v8::Local<v8::Value> result;
        if (!script->Run(context).ToLocal(&result))
        {
            err = describe(context, tryCatch);
            return false;
        }
        if (numResult != nullptr && result->IsNumber())
        {
            *numResult = result.As<v8::Number>()->Value();
        }
        return true;
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

    bool inspectorSeamPresent() const override { return inspector_ != nullptr; }

private:
    static constexpr std::size_t kMaxArgs = 8;

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
        v8::Local<v8::Value> ex = tryCatch.Exception();
        if (ex.IsEmpty())
        {
            return "<no exception>";
        }
        v8::Local<v8::String> str;
        if (!ex->ToString(context).ToLocal(&str))
        {
            return "<unprintable exception>";
        }
        v8::String::Utf8Value utf8(isolate_, str);
        return *utf8 != nullptr ? *utf8 : "<utf8 conversion failed>";
    }

    v8::Isolate::CreateParams createParams_;
    v8::Isolate* isolate_ = nullptr;
    v8::Global<v8::Context> context_;
    std::vector<v8::Global<v8::Function>> functions_;
    std::vector<std::unique_ptr<HostEntry>> hostEntries_;
    std::vector<v8::BackingStore*> vmBuffers_;  // owned; freed via v8__BackingStore__DELETE
    std::unique_ptr<detail::V8InspectorSeam> inspector_;
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
