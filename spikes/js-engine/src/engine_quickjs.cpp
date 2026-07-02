// QuickJS (quickjs-ng) backend for the §2d embedding seam. THROWAWAY spike code.

#include "ctx/js_engine.hpp"

#include <quickjs.h>

#include <cstdlib>
#include <vector>

namespace ctx::spike {
namespace {

struct HostEntry {
    HostFunction fn = nullptr;
    void* user = nullptr;
};

class QuickJsEngine final : public JsEngine {
public:
    QuickJsEngine() = default;

    bool init(std::string& err) {
        rt_ = JS_NewRuntime();
        if (!rt_) {
            err = "JS_NewRuntime failed";
            return false;
        }
        ctx_ = JS_NewContext(rt_);
        if (!ctx_) {
            err = "JS_NewContext failed";
            JS_FreeRuntime(rt_);
            rt_ = nullptr;
            return false;
        }
        JS_SetContextOpaque(ctx_, this);
        return true;
    }

    ~QuickJsEngine() override {
        if (ctx_) {
            for (auto& v : functions_) JS_FreeValue(ctx_, v);
            for (auto& b : buffers_) {
                if (b.live) JS_FreeValue(ctx_, b.value);
            }
            JS_FreeContext(ctx_);
        }
        if (rt_) JS_FreeRuntime(rt_);
    }

    std::string_view name() const override { return "quickjs"; }

    std::string version() const override { return JS_GetVersion(); }

    bool eval(std::string_view code, double* numResult, std::string& err) override {
        JSValue v = JS_Eval(ctx_, code.data(), code.size(), "<spike>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) {
            err = takeException();
            return false;
        }
        if (numResult) {
            double d = 0;
            if (JS_ToFloat64(ctx_, &d, v) == 0) *numResult = d;
        }
        JS_FreeValue(ctx_, v);
        return true;
    }

    FunctionHandle getFunction(std::string_view globalName) override {
        JSValue global = JS_GetGlobalObject(ctx_);
        JSValue fn = JS_GetPropertyStr(ctx_, global, std::string(globalName).c_str());
        JS_FreeValue(ctx_, global);
        if (JS_IsException(fn) || !JS_IsFunction(ctx_, fn)) {
            JS_FreeValue(ctx_, fn);
            return kInvalidFunction;
        }
        functions_.push_back(fn);
        return static_cast<FunctionHandle>(functions_.size());
    }

    bool callFunction(FunctionHandle fn, const double* args, std::size_t nargs, double* result,
                      std::string& err) override {
        if (fn == kInvalidFunction || fn > functions_.size()) {
            err = "bad function handle";
            return false;
        }
        JSValue argv[8];
        if (nargs > 8) {
            err = "spike seam supports <= 8 args";
            return false;
        }
        for (std::size_t i = 0; i < nargs; i++) argv[i] = JS_NewFloat64(ctx_, args[i]);
        JSValue r = JS_Call(ctx_, functions_[fn - 1], JS_UNDEFINED, static_cast<int>(nargs), argv);
        // Number JSValues carry no heap allocation, but free for symmetry/safety.
        for (std::size_t i = 0; i < nargs; i++) JS_FreeValue(ctx_, argv[i]);
        if (JS_IsException(r)) {
            err = takeException();
            return false;
        }
        if (result) {
            double d = 0;
            if (JS_ToFloat64(ctx_, &d, r) == 0) *result = d;
        }
        JS_FreeValue(ctx_, r);
        return true;
    }

    bool bindHostFunction(std::string_view globalName, HostFunction fn, void* user) override {
        hostEntries_.push_back(HostEntry{fn, user});
        const int magic = static_cast<int>(hostEntries_.size() - 1);
        JSValue jsfn = JS_NewCFunctionMagic(ctx_, &QuickJsEngine::trampoline,
                                            std::string(globalName).c_str(), 0,
                                            JS_CFUNC_generic_magic, magic);
        JSValue global = JS_GetGlobalObject(ctx_);
        const int ok =
            JS_SetPropertyStr(ctx_, global, std::string(globalName).c_str(), jsfn);  // consumes jsfn
        JS_FreeValue(ctx_, global);
        return ok >= 0;
    }

    BufferHandle attachHostBuffer(std::string_view globalName, void* data, std::size_t bytes,
                                  std::string& err) override {
        // Zero-copy: wraps host memory directly; free_func == NULL so neither GC nor detach
        // ever frees or touches the host allocation.
        JSValue ab = JS_NewArrayBuffer(ctx_, static_cast<uint8_t*>(data), bytes,
                                       /*free_func=*/nullptr, /*opaque=*/nullptr,
                                       /*is_shared=*/false);
        if (JS_IsException(ab)) {
            err = takeException();
            return kInvalidBuffer;
        }
        JSValue global = JS_GetGlobalObject(ctx_);
        // Keep our own reference for detach; the property set consumes one reference.
        JSValue dup = JS_DupValue(ctx_, ab);
        const int ok = JS_SetPropertyStr(ctx_, global, std::string(globalName).c_str(), ab);
        JS_FreeValue(ctx_, global);
        if (ok < 0) {
            JS_FreeValue(ctx_, dup);
            err = "JS_SetPropertyStr failed";
            return kInvalidBuffer;
        }
        if (freeSlot_ != 0) {
            const std::size_t slot = freeSlot_ - 1;
            freeSlot_ = buffers_[slot].nextFree;
            buffers_[slot] = Buffer{dup, true, 0};
            return static_cast<BufferHandle>(slot + 1);
        }
        buffers_.push_back(Buffer{dup, true, 0});
        return static_cast<BufferHandle>(buffers_.size());
    }

    AllocHandle allocSharedBuffer(std::size_t bytes, void** outData, std::string& err) override {
        // QuickJS happily wraps arbitrary host memory, so "VM-allocated shared" is just a
        // host-side allocation the engine owns (mirrors the V8 sandbox shape for comparison).
        void* data = std::calloc(1, bytes);
        if (!data) {
            err = "calloc failed";
            return kInvalidAlloc;
        }
        allocs_.push_back(Alloc{data, bytes, true});
        if (outData) *outData = data;
        return static_cast<AllocHandle>(allocs_.size());
    }

    BufferHandle attachSharedBuffer(std::string_view globalName, AllocHandle alloc,
                                    std::string& err) override {
        if (alloc == kInvalidAlloc || alloc > allocs_.size() || !allocs_[alloc - 1].live) {
            err = "bad alloc handle";
            return kInvalidBuffer;
        }
        return attachHostBuffer(globalName, allocs_[alloc - 1].data, allocs_[alloc - 1].bytes,
                                err);
    }

    bool freeSharedBuffer(AllocHandle alloc, std::string& err) override {
        if (alloc == kInvalidAlloc || alloc > allocs_.size() || !allocs_[alloc - 1].live) {
            err = "bad alloc handle";
            return false;
        }
        std::free(allocs_[alloc - 1].data);
        allocs_[alloc - 1] = Alloc{nullptr, 0, false};
        return true;
    }

    bool detachHostBuffer(BufferHandle h, std::string& err) override {
        if (h == kInvalidBuffer || h > buffers_.size() || !buffers_[h - 1].live) {
            err = "bad buffer handle";
            return false;
        }
        Buffer& b = buffers_[h - 1];
        JS_DetachArrayBuffer(ctx_, b.value);  // R-LANG-009: views die, host memory untouched
        JS_FreeValue(ctx_, b.value);
        b.live = false;
        b.nextFree = freeSlot_;
        freeSlot_ = h;
        return true;
    }

    void collectGarbage() override { JS_RunGC(rt_); }

private:
    struct Buffer {
        JSValue value{};
        bool live = false;
        BufferHandle nextFree = 0;
    };

    struct Alloc {
        void* data = nullptr;
        std::size_t bytes = 0;
        bool live = false;
    };

    static JSValue trampoline(JSContext* ctx, JSValueConst /*thisVal*/, int argc,
                              JSValueConst* argv, int magic) {
        auto* self = static_cast<QuickJsEngine*>(JS_GetContextOpaque(ctx));
        const HostEntry& entry = self->hostEntries_[static_cast<std::size_t>(magic)];
        double args[8];
        const int n = argc > 8 ? 8 : argc;
        for (int i = 0; i < n; i++) {
            if (JS_ToFloat64(ctx, &args[i], argv[i]) != 0) return JS_EXCEPTION;
        }
        const double r = entry.fn(entry.user, args, static_cast<std::size_t>(n));
        return JS_NewFloat64(ctx, r);
    }

    std::string takeException() {
        JSValue ex = JS_GetException(ctx_);
        const char* s = JS_ToCString(ctx_, ex);
        std::string msg = s ? s : "<unprintable exception>";
        if (s) JS_FreeCString(ctx_, s);
        JS_FreeValue(ctx_, ex);
        return msg;
    }

    JSRuntime* rt_ = nullptr;
    JSContext* ctx_ = nullptr;
    std::vector<JSValue> functions_;
    std::vector<HostEntry> hostEntries_;
    std::vector<Buffer> buffers_;
    std::vector<Alloc> allocs_;
    BufferHandle freeSlot_ = 0;
};

}  // namespace

std::unique_ptr<JsEngine> createQuickJsEngine(const EngineConfig& /*cfg*/, std::string& err) {
    auto engine = std::make_unique<QuickJsEngine>();
    if (!engine->init(err)) return nullptr;
    return engine;
}

}  // namespace ctx::spike
