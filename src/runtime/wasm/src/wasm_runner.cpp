// WasmRunner — the REAL wasmtime-Cranelift backend (issue #71 PR3). Executes the frozen PR2 guest
// ABI (ctx_alloc / ctx_migrate / optional ctx_map_path over the module's exported linear memory)
// under the deterministic config documented in wasm_runner.h: Cranelift, consume_fuel ON
// (budget->fuel = kWasmFuelPerBudgetNode x max_nodes), NaN canonicalization ON, relaxed-SIMD
// deterministic + disabled, threads OFF, the fixed linear-memory limit, and one FRESH
// Store+instance per call. Zero imports are enforced STRUCTURALLY: instantiation passes an empty
// import list, so any import-declaring module fails to instantiate.

#include "context/runtime/wasm/wasm_runner.h"

#include <wasmtime.h>

#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace context::runtime::wasm
{

namespace
{

// The guest ABI's offsets/lengths are little-endian i32 in linear memory; wasm linear memory is
// little-endian by spec, and every supported host (x86_64, aarch64) is too, so the host reads the
// two out-cells with a plain byte copy. The tripwire keeps a hypothetical big-endian port honest.
static_assert(std::endian::native == std::endian::little,
              "the guest-ABI out-cell reads below assume a little-endian host");

// Consume (and free) a wasmtime error/trap pair into a short human-readable message — the proven
// spikes/wasm pattern. Takes ownership of whichever is non-null.
std::string message_of(wasmtime_error_t* error, wasm_trap_t* trap)
{
    wasm_byte_vec_t msg{};
    std::string out;
    if (error != nullptr)
    {
        wasmtime_error_message(error, &msg);
        out.assign(msg.data, msg.size);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(error);
    }
    else if (trap != nullptr)
    {
        wasm_trap_message(trap, &msg);
        out = "trap: " + std::string(msg.data, msg.size);
        wasm_byte_vec_delete(&msg);
        wasm_trap_delete(trap);
    }
    return out;
}

// True (and frees the trap) when `trap` is the deterministic fuel-exhaustion trap — the
// budget_exceeded signal. Leaves other traps alone for message_of.
bool is_out_of_fuel(wasm_trap_t* trap)
{
    if (trap == nullptr)
        return false;
    wasmtime_trap_code_t code = 0;
    if (!wasmtime_trap_code(trap, &code) || code != WASMTIME_TRAP_CODE_OUT_OF_FUEL)
        return false;
    wasm_trap_delete(trap);
    return true;
}

// fuel = K x max_nodes, saturating (the budget->fuel contract — wasm_runner.h).
std::uint64_t fuel_for(const editor::migrate::MigrationBudget& budget)
{
    if (budget.max_nodes > std::numeric_limits<std::uint64_t>::max() / kWasmFuelPerBudgetNode)
        return std::numeric_limits<std::uint64_t>::max();
    return kWasmFuelPerBudgetNode * budget.max_nodes;
}

struct ModuleGuard
{
    wasmtime_module_t* module = nullptr;
    ~ModuleGuard()
    {
        if (module != nullptr)
            wasmtime_module_delete(module);
    }
};

struct StoreGuard
{
    wasmtime_store_t* store = nullptr;
    ~StoreGuard()
    {
        if (store != nullptr)
            wasmtime_store_delete(store);
    }
};

// How one guest call ended, in seam terms.
enum class GuestStatus
{
    ok,            // the export was called; guest_ret carries its i32 return value
    absent_export, // the module does not provide the (optional) export
    failed,        // resolution / compile / instantiate / call / memory-contract failure
    budget,        // deterministic fuel exhaustion (-> migration.budget_exceeded)
};

struct GuestCall
{
    GuestStatus status = GuestStatus::failed;
    std::int32_t guest_ret = -1; // meaningful iff status == ok
    std::string detail;          // meaningful iff status == failed / budget
    std::string output;          // meaningful iff status == ok && guest_ret == 0
};

// Instantiate `step.wasm_module` on a FRESH deterministic Store and drive one `fn_name` call per
// the frozen guest ABI: ctx_alloc the input + the two i32 out-cells, write the input bytes, call
// fn(in_ptr, in_len, out_ptr_ptr, out_len_ptr), then (on a 0 return) read the (offset, len) the
// guest stored and copy the output bytes out. Every offset/length is bounds-checked against the
// CURRENT memory size — the guest is trusted for nothing.
GuestCall invoke_guest(wasm_engine_t* engine, const ModuleResolver& resolver,
                       const editor::migrate::SandboxedStep& step, const char* fn_name,
                       std::string_view input)
{
    GuestCall call;
    const std::string step_id = "\"" + std::string(step.component_type) + "\" v" +
                                std::to_string(step.from_version);

    // Resolve the module reference to wasm bytes (injected policy — the runner does no IO).
    if (step.wasm_module.empty())
    {
        call.detail = "no wasm module reference to instantiate";
        return call;
    }
    if (!resolver)
    {
        call.detail = "no module resolver injected";
        return call;
    }
    std::string wasm_bytes;
    std::string resolve_problem;
    if (!resolver(step.wasm_module, wasm_bytes, resolve_problem))
    {
        call.detail = "module reference \"" + std::string(step.wasm_module) +
                      "\" did not resolve" +
                      (resolve_problem.empty() ? std::string() : ": " + resolve_problem);
        return call;
    }
    if (input.size() > kWasmMemoryLimitBytes)
    {
        call.detail = "input payload (" + std::to_string(input.size()) +
                      " bytes) exceeds the fixed linear-memory limit";
        return call;
    }

    // Compile (pure — the shared deterministic engine owns codegen config).
    ModuleGuard module;
    if (wasmtime_error_t* error =
            wasmtime_module_new(engine, reinterpret_cast<const std::uint8_t*>(wasm_bytes.data()),
                                wasm_bytes.size(), &module.module);
        error != nullptr)
    {
        call.detail = "module compile failed: " + message_of(error, nullptr);
        return call;
    }

    // One FRESH Store per call (the frozen instance-lifecycle contract): fixed memory limit,
    // exactly one instance/memory, and the deterministic fuel grant — set BEFORE instantiation so
    // data-segment initialization is metered too.
    StoreGuard store;
    store.store = wasmtime_store_new(engine, nullptr, nullptr);
    if (store.store == nullptr)
    {
        call.detail = "wasmtime_store_new failed";
        return call;
    }
    wasmtime_store_limiter(store.store, static_cast<std::int64_t>(kWasmMemoryLimitBytes),
                           /*table_elements=*/10000, /*instances=*/1, /*tables=*/1,
                           /*memories=*/1);
    wasmtime_context_t* context = wasmtime_store_context(store.store);
    if (wasmtime_error_t* error = wasmtime_context_set_fuel(context, fuel_for(step.budget));
        error != nullptr)
    {
        call.detail = "set_fuel failed: " + message_of(error, nullptr);
        return call;
    }

    // Instantiate with an EMPTY import list — a module declaring any import fails right here
    // (the zero-import sandbox, enforced structurally).
    wasmtime_instance_t instance{};
    {
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* error =
            wasmtime_instance_new(context, module.module, nullptr, 0, &instance, &trap);
        if (error != nullptr || trap != nullptr)
        {
            if (is_out_of_fuel(trap))
            {
                call.status = GuestStatus::budget;
                call.detail = "fuel exhausted during instantiation of step " + step_id;
                return call;
            }
            call.detail = "instantiation failed (a migration module imports NOTHING — the "
                          "zero-import guest ABI): " +
                          message_of(error, trap);
            return call;
        }
    }

    // Required exports: the linear memory + ctx_alloc + the requested function.
    wasmtime_extern_t item{};
    if (!wasmtime_instance_export_get(context, &instance, "memory", std::strlen("memory"),
                                      &item) ||
        item.kind != WASMTIME_EXTERN_MEMORY)
    {
        call.detail = "module exports no linear memory \"memory\" (frozen guest ABI)";
        return call;
    }
    wasmtime_memory_t memory = item.of.memory;
    if (!wasmtime_instance_export_get(context, &instance, "ctx_alloc", std::strlen("ctx_alloc"),
                                      &item) ||
        item.kind != WASMTIME_EXTERN_FUNC)
    {
        call.detail = "module exports no ctx_alloc (frozen guest ABI)";
        return call;
    }
    wasmtime_func_t alloc_fn = item.of.func;
    if (!wasmtime_instance_export_get(context, &instance, fn_name, std::strlen(fn_name), &item) ||
        item.kind != WASMTIME_EXTERN_FUNC)
    {
        call.status = GuestStatus::absent_export;
        call.detail = "module exports no " + std::string(fn_name);
        return call;
    }
    wasmtime_func_t guest_fn = item.of.func;

    // ctx_alloc helper (fuel-metered like everything else in the store).
    const auto guest_alloc = [&](std::uint32_t size, std::uint32_t& offset) -> bool {
        wasmtime_val_t arg{};
        arg.kind = WASMTIME_I32;
        arg.of.i32 = static_cast<std::int32_t>(size);
        wasmtime_val_t ret{};
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* error = wasmtime_func_call(context, &alloc_fn, &arg, 1, &ret, 1, &trap);
        if (error != nullptr || trap != nullptr)
        {
            if (is_out_of_fuel(trap))
            {
                call.status = GuestStatus::budget;
                call.detail = "fuel exhausted in ctx_alloc of step " + step_id;
                return false;
            }
            call.status = GuestStatus::failed;
            call.detail = "ctx_alloc failed: " + message_of(error, trap);
            return false;
        }
        offset = static_cast<std::uint32_t>(ret.of.i32);
        if (offset == 0)
        {
            call.status = GuestStatus::failed;
            call.detail = "ctx_alloc reported allocation failure (returned 0)";
            return false;
        }
        return true;
    };

    // Stage the call: allocate + write the input payload and the two i32 out-cells. The memory
    // data pointer/size are re-fetched after EVERY guest call — an alloc may grow (and move) the
    // linear memory.
    std::uint32_t in_ptr = 0;
    if (!guest_alloc(static_cast<std::uint32_t>(input.size()), in_ptr))
        return call;
    std::uint32_t cells_ptr = 0;
    if (!guest_alloc(8, cells_ptr))
        return call;
    {
        std::uint8_t* data = wasmtime_memory_data(context, &memory);
        const std::uint64_t mem_size = wasmtime_memory_data_size(context, &memory);
        if (static_cast<std::uint64_t>(in_ptr) + input.size() > mem_size ||
            static_cast<std::uint64_t>(cells_ptr) + 8 > mem_size)
        {
            call.status = GuestStatus::failed;
            call.detail = "ctx_alloc returned an out-of-bounds region";
            return call;
        }
        if (!input.empty())
            std::memcpy(data + in_ptr, input.data(), input.size());
        std::memset(data + cells_ptr, 0, 8);
    }

    // The guest call proper: fn(in_ptr, in_len, out_ptr_ptr, out_len_ptr) -> i32.
    wasmtime_val_t args[4];
    for (wasmtime_val_t& a : args)
        a.kind = WASMTIME_I32;
    args[0].of.i32 = static_cast<std::int32_t>(in_ptr);
    args[1].of.i32 = static_cast<std::int32_t>(input.size());
    args[2].of.i32 = static_cast<std::int32_t>(cells_ptr);
    args[3].of.i32 = static_cast<std::int32_t>(cells_ptr + 4);
    wasmtime_val_t ret{};
    {
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* error = wasmtime_func_call(context, &guest_fn, args, 4, &ret, 1, &trap);
        if (error != nullptr || trap != nullptr)
        {
            if (is_out_of_fuel(trap))
            {
                call.status = GuestStatus::budget;
                call.detail = "fuel exhausted in step " + step_id + " (deterministic budget: " +
                              std::to_string(kWasmFuelPerBudgetNode) + " fuel x " +
                              std::to_string(step.budget.max_nodes) + " budget nodes)";
                return call;
            }
            call.status = GuestStatus::failed;
            call.detail = std::string(fn_name) + " failed: " + message_of(error, trap);
            return call;
        }
    }
    call.status = GuestStatus::ok;
    call.guest_ret = ret.of.i32;
    if (call.guest_ret != 0)
        return call; // a guest-reported failure/unmapped verdict — no output to read

    // Read back the (offset, len) the guest stored and copy the output bytes, bounds-checked
    // against the CURRENT memory size (the call may have grown memory).
    std::uint8_t* data = wasmtime_memory_data(context, &memory);
    const std::uint64_t mem_size = wasmtime_memory_data_size(context, &memory);
    std::uint32_t out_ptr = 0;
    std::uint32_t out_len = 0;
    std::memcpy(&out_ptr, data + cells_ptr, 4);
    std::memcpy(&out_len, data + cells_ptr + 4, 4);
    if (static_cast<std::uint64_t>(out_ptr) + out_len > mem_size)
    {
        call.status = GuestStatus::failed;
        call.detail = std::string(fn_name) + " stored an out-of-bounds output region";
        return call;
    }
    call.output.assign(reinterpret_cast<const char*>(data + out_ptr), out_len);
    return call;
}

} // namespace

struct WasmRunner::Impl
{
    ModuleResolver resolver;
    wasm_engine_t* engine = nullptr;

    ~Impl()
    {
        if (engine != nullptr)
            wasm_engine_delete(engine);
    }
};

std::unique_ptr<WasmRunner> WasmRunner::create(ModuleResolver resolver, std::string& problem)
{
    // The deterministic VM config (wasm_runner.h): every knob that could make two hosts (or two
    // runs) observe different guest behavior is pinned here, at VM init — the R-FILE-005
    // cold-start "VM" slot.
    wasm_config_t* config = wasm_config_new();
    if (config == nullptr)
    {
        problem = "wasm_config_new failed";
        return nullptr;
    }
    wasmtime_config_strategy_set(config, WASMTIME_STRATEGY_CRANELIFT);
    wasmtime_config_consume_fuel_set(config, true); // budget->fuel: deterministic metering (L-37)
    wasmtime_config_cranelift_nan_canonicalization_set(config, true); // one NaN on every platform
    wasmtime_config_wasm_relaxed_simd_deterministic_set(config, true);
    wasmtime_config_wasm_relaxed_simd_set(config, false); // off AND deterministic (belt+braces)
    wasmtime_config_wasm_threads_set(config, false);      // no shared state, no schedules
    wasmtime_config_shared_memory_set(config, false);

    auto impl = std::make_unique<Impl>();
    impl->resolver = std::move(resolver);
    impl->engine = wasm_engine_new_with_config(config); // takes ownership of config
    if (impl->engine == nullptr)
    {
        problem = "wasm_engine_new_with_config failed";
        return nullptr;
    }
    return std::unique_ptr<WasmRunner>(new WasmRunner(std::move(impl)));
}

bool WasmRunner::runtime_available() noexcept
{
    return true;
}

WasmRunner::WasmRunner(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

WasmRunner::~WasmRunner() = default;

editor::migrate::SandboxedStepResult
WasmRunner::run_step(const editor::migrate::SandboxedStep& step, std::string_view input)
{
    editor::migrate::SandboxedStepResult result;
    GuestCall call = invoke_guest(impl_->engine, impl_->resolver, step, "ctx_migrate", input);
    switch (call.status)
    {
    case GuestStatus::ok:
        if (call.guest_ret != 0)
        {
            // ANY non-zero ctx_migrate return is a guest-reported failure (frozen ABI).
            result.detail = "guest ctx_migrate returned " + std::to_string(call.guest_ret);
            return result;
        }
        result.ok = true;
        result.output = std::move(call.output);
        return result;
    case GuestStatus::absent_export:
        result.detail = call.detail + " (frozen guest ABI)";
        return result;
    case GuestStatus::budget:
        result.budget_exceeded = true;
        result.detail = std::move(call.detail);
        return result;
    case GuestStatus::failed:
        break;
    }
    result.detail = std::move(call.detail);
    return result;
}

std::optional<std::string> WasmRunner::map_path(const editor::migrate::SandboxedStep& step,
                                                std::string_view pointer)
{
    GuestCall call = invoke_guest(impl_->engine, impl_->resolver, step, "ctx_map_path", pointer);
    if (call.status == GuestStatus::absent_export)
        return std::string(pointer); // module omits the optional export => IDENTITY (frozen ABI)
    if (call.status == GuestStatus::ok && call.guest_ret == 0)
        return std::move(call.output); // mapped
    // 1 = unmapped (the orphan case); any other guest error / trap / fuel exhaustion during path
    // mapping is ALSO conservatively "no destination": the seam's map signature has no error
    // channel, and an orphaned override is the non-destructive outcome (preserved verbatim,
    // non-blocking — L-37). run_step is where a misbehaving guest blocks the document.
    return std::nullopt;
}

} // namespace context::runtime::wasm
