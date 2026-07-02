// spikes/wasm — WAMR backend (built from source, runtime_lib.cmake recipe). THROWAWAY spike
// code.
//
// Modes: Interp = the fast-interpreter (WAMR's production no-JIT configuration — the
// iOS-class answer for R-LANG-005); Aot = load a wamrc-precompiled AOT image through the SAME
// wasm_runtime_load door (the runtime auto-detects the format; WAMR_BUILD_AOT adds only the
// loader, no compiler). LLVM JIT / Fast-JIT are out of spike scope (both need extra toolchain
// weight; recorded in FINDINGS).

#include "ctx/wasm_engine.hpp"

#include <wasm_export.h>

#include <cstring>
#include <vector>

namespace ctx::spike {

namespace {

wasm_val_t toWamr(const Val& v) {
    wasm_val_t w{};
    switch (v.kind) {
        case Val::Kind::I32: w.kind = WASM_I32; w.of.i32 = v.i32; break;
        case Val::Kind::I64: w.kind = WASM_I64; w.of.i64 = v.i64; break;
        case Val::Kind::F32: w.kind = WASM_F32; w.of.f32 = v.f32; break;
        case Val::Kind::F64: w.kind = WASM_F64; w.of.f64 = v.f64; break;
    }
    return w;
}

Val fromWamr(const wasm_val_t& w) {
    switch (w.kind) {
        case WASM_I32: return Val::makeI32(w.of.i32);
        case WASM_I64: return Val::makeI64(w.of.i64);
        case WASM_F32: return Val::makeF32(w.of.f32);
        default: return Val::makeF64(w.of.f64);
    }
}

class WamrEngine final : public WasmEngine {
public:
    explicit WamrEngine(ExecMode mode) : mode_(mode) {}

    ~WamrEngine() override {
        if (execEnv_ != nullptr) wasm_runtime_destroy_exec_env(execEnv_);
        if (instance_ != nullptr) wasm_runtime_deinstantiate(instance_);
        if (module_ != nullptr) wasm_runtime_unload(module_);
        if (initialized_) wasm_runtime_destroy();
    }

    bool init(std::string& err) {
        RuntimeInitArgs initArgs{};
        initArgs.mem_alloc_type = Alloc_With_System_Allocator;
        if (!wasm_runtime_full_init(&initArgs)) {
            err = "wasm_runtime_full_init failed";
            return false;
        }
        initialized_ = true;
        wasm_runtime_set_log_level(WASM_LOG_LEVEL_ERROR);  // keep advisory mmap warnings off stdout
        modeName_ = (mode_ == ExecMode::Aot) ? "aot" : "fast-interp";
        return true;
    }

    std::string_view name() const override { return "wamr"; }

    std::string version() const override {
        std::uint32_t major = 0, minor = 0, patch = 0;
        wasm_runtime_get_version(&major, &minor, &patch);
        return std::to_string(major) + "." + std::to_string(minor) + "." +
               std::to_string(patch);
    }

    std::string_view mode() const override { return modeName_; }

    bool compile(const std::uint8_t* bytes, std::size_t len, std::string& err) override {
        // WAMR may rewrite the buffer in place (fast-interp pre-processing) and executes out
        // of it for the module's lifetime — keep an owned mutable copy.
        bytes_.assign(bytes, bytes + len);
        char errBuf[256] = {0};
        module_ = wasm_runtime_load(bytes_.data(), static_cast<std::uint32_t>(bytes_.size()),
                                    errBuf, sizeof(errBuf));
        if (module_ == nullptr) {
            err = std::string("compile/load: ") + errBuf;
            return false;
        }
        return true;
    }

    bool instantiate(std::string& err) override {
        char errBuf[256] = {0};
        // stack 64 KiB (matches the guest link stack), app heap 0 (freestanding guest: no
        // libc, no malloc imports — ZERO imports at all).
        instance_ = wasm_runtime_instantiate(module_, 64 * 1024, 0, errBuf, sizeof(errBuf));
        if (instance_ == nullptr) {
            err = std::string("instantiate: ") + errBuf;
            return false;
        }
        execEnv_ = wasm_runtime_create_exec_env(instance_, 64 * 1024);
        if (execEnv_ == nullptr) {
            err = "wasm_runtime_create_exec_env failed";
            return false;
        }
        memory_ = wasm_runtime_get_memory(instance_, 0);
        if (memory_ == nullptr) {
            err = "instantiate: exported linear memory not found";
            return false;
        }
        return true;
    }

    FunctionHandle getFunction(std::string_view exportName) override {
        std::string name(exportName);
        wasm_function_inst_t fn = wasm_runtime_lookup_function(instance_, name.c_str());
        if (fn == nullptr) return kInvalidFunction;
        funcs_.push_back(fn);
        return static_cast<FunctionHandle>(funcs_.size());
    }

    bool call(FunctionHandle fn, const Val* args, std::size_t nargs, Val* results,
              std::size_t nresults, std::string& err) override {
        wasm_val_t wargs[4];
        wasm_val_t wresults[2];
        for (std::size_t i = 0; i < nargs; i++) wargs[i] = toWamr(args[i]);
        if (!wasm_runtime_call_wasm_a(execEnv_, funcs_[fn - 1],
                                      static_cast<std::uint32_t>(nresults), wresults,
                                      static_cast<std::uint32_t>(nargs), wargs)) {
            const char* exception = wasm_runtime_get_exception(instance_);
            err = std::string("trap: ") + (exception != nullptr ? exception : "unknown");
            wasm_runtime_clear_exception(instance_);
            return false;
        }
        for (std::size_t i = 0; i < nresults; i++) results[i] = fromWamr(wresults[i]);
        return true;
    }

    std::uint8_t* memoryData(std::string& err) override {
        void* base = wasm_memory_get_base_address(memory_);
        if (base == nullptr) err = "wasm_memory_get_base_address returned null";
        return static_cast<std::uint8_t*>(base);
    }

    std::size_t memoryBytes() override {
        return static_cast<std::size_t>(wasm_memory_get_cur_page_count(memory_) *
                                        wasm_memory_get_bytes_per_page(memory_));
    }

private:
    ExecMode mode_;
    std::string_view modeName_ = "?";
    bool initialized_ = false;
    std::vector<std::uint8_t> bytes_;
    wasm_module_t module_ = nullptr;
    wasm_module_inst_t instance_ = nullptr;
    wasm_exec_env_t execEnv_ = nullptr;
    wasm_memory_inst_t memory_ = nullptr;
    std::vector<wasm_function_inst_t> funcs_;
};

}  // namespace

std::unique_ptr<WasmEngine> createWamrEngine(const EngineConfig& cfg, std::string& err) {
    if (cfg.mode != ExecMode::Interp && cfg.mode != ExecMode::Aot) {
        err = "wamr backend: only Interp (fast-interp) and Aot modes are built in this spike "
              "(LLVM JIT / Fast-JIT need extra toolchain weight — see FINDINGS)";
        return nullptr;
    }
    auto engine = std::make_unique<WamrEngine>(cfg.mode);
    if (!engine->init(err)) return nullptr;
    return engine;
}

}  // namespace ctx::spike
