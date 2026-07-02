// spikes/wasm — wasmtime backend (prebuilt C API). THROWAWAY spike code.
//
// Modes: Jit = Cranelift; BaselineJit = Winch; Interp = Pulley (Cranelift's portable bytecode
// interpreter, selected by compiling for the "pulley64" target). Whether the prebuilt supports
// each mode is itself a finding — unavailability comes back as a factory/compile error string.

#include "ctx/wasm_engine.hpp"

#include <wasmtime.h>

#include <cstring>
#include <vector>

namespace ctx::spike {

namespace {

std::string errorToString(wasmtime_error_t* error, wasm_trap_t* trap) {
    wasm_byte_vec_t msg{};
    std::string out;
    if (error != nullptr) {
        wasmtime_error_message(error, &msg);
        out.assign(msg.data, msg.size);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(error);
    } else if (trap != nullptr) {
        wasm_trap_message(trap, &msg);
        out = "trap: " + std::string(msg.data, msg.size);
        wasm_byte_vec_delete(&msg);
        wasm_trap_delete(trap);
    }
    return out;
}

wasmtime_val_t toWasmtime(const Val& v) {
    wasmtime_val_t w{};
    switch (v.kind) {
        case Val::Kind::I32: w.kind = WASMTIME_I32; w.of.i32 = v.i32; break;
        case Val::Kind::I64: w.kind = WASMTIME_I64; w.of.i64 = v.i64; break;
        case Val::Kind::F32: w.kind = WASMTIME_F32; w.of.f32 = v.f32; break;
        case Val::Kind::F64: w.kind = WASMTIME_F64; w.of.f64 = v.f64; break;
    }
    return w;
}

Val fromWasmtime(const wasmtime_val_t& w) {
    switch (w.kind) {
        case WASMTIME_I32: return Val::makeI32(w.of.i32);
        case WASMTIME_I64: return Val::makeI64(w.of.i64);
        case WASMTIME_F32: return Val::makeF32(w.of.f32);
        default: return Val::makeF64(w.of.f64);
    }
}

class WasmtimeEngine final : public WasmEngine {
public:
    explicit WasmtimeEngine(ExecMode mode) : mode_(mode) {}

    ~WasmtimeEngine() override {
        if (module_ != nullptr) wasmtime_module_delete(module_);
        if (store_ != nullptr) wasmtime_store_delete(store_);
        if (engine_ != nullptr) wasm_engine_delete(engine_);
    }

    bool init(std::string& err) {
        wasm_config_t* config = wasm_config_new();
        switch (mode_) {
            case ExecMode::Jit:
                wasmtime_config_strategy_set(config, WASMTIME_STRATEGY_CRANELIFT);
                modeName_ = "cranelift-jit";
                break;
            case ExecMode::BaselineJit:
                wasmtime_config_strategy_set(config, WASMTIME_STRATEGY_WINCH);
                modeName_ = "winch";
                break;
            case ExecMode::Interp: {
                // Pulley: compile for the portable interpreter "target". Cross-"compilation"
                // to pulley64 is how wasmtime spells interpreter mode.
                wasmtime_error_t* terr = wasmtime_config_target_set(config, "pulley64");
                if (terr != nullptr) {
                    err = "pulley target unavailable: " + errorToString(terr, nullptr);
                    wasm_config_delete(config);
                    return false;
                }
                modeName_ = "pulley-interp";
                break;
            }
            case ExecMode::Aot:
                err = "wasmtime backend: AOT mode not wired in this spike (Cranelift .cwasm "
                      "precompilation exists — wasmtime_module_serialize — but the spike's AOT "
                      "leg is WAMR's)";
                wasm_config_delete(config);
                return false;
        }
        engine_ = wasm_engine_new_with_config(config);  // takes ownership of config
        if (engine_ == nullptr) {
            err = "wasm_engine_new_with_config failed";
            return false;
        }
        store_ = wasmtime_store_new(engine_, nullptr, nullptr);
        ctx_ = wasmtime_store_context(store_);
        return true;
    }

    std::string_view name() const override { return "wasmtime"; }
    std::string version() const override { return WASMTIME_VERSION; }
    std::string_view mode() const override { return modeName_; }

    bool compile(const std::uint8_t* bytes, std::size_t len, std::string& err) override {
        wasmtime_error_t* error = wasmtime_module_new(engine_, bytes, len, &module_);
        if (error != nullptr) {
            err = "compile: " + errorToString(error, nullptr);
            return false;
        }
        return true;
    }

    bool instantiate(std::string& err) override {
        wasm_trap_t* trap = nullptr;
        // ZERO imports — the guest declares none; the empty import list IS the sandbox.
        wasmtime_error_t* error =
            wasmtime_instance_new(ctx_, module_, nullptr, 0, &instance_, &trap);
        if (error != nullptr || trap != nullptr) {
            err = "instantiate: " + errorToString(error, trap);
            return false;
        }
        wasmtime_extern_t item{};
        if (!wasmtime_instance_export_get(ctx_, &instance_, "memory", std::strlen("memory"),
                                          &item) ||
            item.kind != WASMTIME_EXTERN_MEMORY) {
            err = "instantiate: exported linear memory 'memory' not found";
            return false;
        }
        memory_ = item.of.memory;
        return true;
    }

    FunctionHandle getFunction(std::string_view exportName) override {
        wasmtime_extern_t item{};
        if (!wasmtime_instance_export_get(ctx_, &instance_, exportName.data(),
                                          exportName.size(), &item) ||
            item.kind != WASMTIME_EXTERN_FUNC) {
            return kInvalidFunction;
        }
        funcs_.push_back(item.of.func);
        return static_cast<FunctionHandle>(funcs_.size());
    }

    bool call(FunctionHandle fn, const Val* args, std::size_t nargs, Val* results,
              std::size_t nresults, std::string& err) override {
        wasmtime_val_t wargs[4];
        wasmtime_val_t wresults[2];
        for (std::size_t i = 0; i < nargs; i++) wargs[i] = toWasmtime(args[i]);
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* error =
            wasmtime_func_call(ctx_, &funcs_[fn - 1], wargs, nargs, wresults, nresults, &trap);
        if (error != nullptr || trap != nullptr) {
            err = errorToString(error, trap);
            return false;
        }
        for (std::size_t i = 0; i < nresults; i++) results[i] = fromWasmtime(wresults[i]);
        return true;
    }

    std::uint8_t* memoryData(std::string& err) override {
        std::uint8_t* data = wasmtime_memory_data(ctx_, &memory_);
        if (data == nullptr) err = "wasmtime_memory_data returned null";
        return data;
    }

    std::size_t memoryBytes() override { return wasmtime_memory_data_size(ctx_, &memory_); }

private:
    ExecMode mode_;
    std::string_view modeName_ = "?";
    wasm_engine_t* engine_ = nullptr;
    wasmtime_store_t* store_ = nullptr;
    wasmtime_context_t* ctx_ = nullptr;
    wasmtime_module_t* module_ = nullptr;
    wasmtime_instance_t instance_{};
    wasmtime_memory_t memory_{};
    std::vector<wasmtime_func_t> funcs_;
};

}  // namespace

std::unique_ptr<WasmEngine> createWasmtimeEngine(const EngineConfig& cfg, std::string& err) {
    auto engine = std::make_unique<WasmtimeEngine>(cfg.mode);
    if (!engine->init(err)) return nullptr;
    return engine;
}

}  // namespace ctx::spike
