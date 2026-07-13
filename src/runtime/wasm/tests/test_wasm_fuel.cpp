// Budget->fuel suite (issue #71 PR3). Through the RUNNER: fuel = kWasmFuelPerBudgetNode x
// max_nodes scales with the budget (the same guest traps out-of-fuel under a 1-node budget and
// completes under the default), and an ENDLESS guest terminates deterministically as
// budget_exceeded — the L-37 "a pathological migration cannot hang derivation" guarantee, with no
// wall clock anywhere. Through the C API directly: fuel metering is DETERMINISTIC (identical runs
// consume identical fuel; consumption scales linearly with work) and the fuel-cost
// micro-validation — Cranelift's fuel instrumentation is a bounded overhead on guest execution,
// not a multiplicative blowup (the js/webgpu spikes never exercised fuel; this pins it).

#include "wasm_test.h"

#include "context/runtime/wasm/wasm_runner.h"

#include <wasmtime.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using context::editor::migrate::SandboxedStep;
using context::editor::migrate::SandboxedStepResult;
using context::runtime::wasm::kWasmFuelPerBudgetNode;
using context::runtime::wasm::WasmRunner;
using wasmtest::ModuleTable;

namespace
{

// ---- through the runner: budget scaling + deterministic termination -------------------------

ModuleTable& table()
{
    static ModuleTable t = [] {
        ModuleTable built;
        built.add("spin.wasm", wasmtest::kWatSpin);
        built.add("infinite.wasm", wasmtest::kWatInfinite);
        return built;
    }();
    return t;
}

WasmRunner& runner()
{
    static std::unique_ptr<WasmRunner> r = [] {
        std::string problem;
        std::unique_ptr<WasmRunner> created = WasmRunner::create(table().resolver(), problem);
        if (created == nullptr)
            std::fprintf(stderr, "WasmRunner::create failed: %s\n", problem.c_str());
        return created;
    }();
    CHECK(r != nullptr);
    return *r;
}

SandboxedStep step_for(const char* module_ref, std::uint64_t max_nodes)
{
    SandboxedStep step;
    step.wasm_module = module_ref;
    step.component_type = "test:fuel";
    step.from_version = 1;
    step.budget.max_nodes = max_nodes;
    return step;
}

void test_fuel_grant_scales_with_the_budget()
{
    // The spin guest burns a few million fuel units. Under a 1-node budget (fuel = K = 10^4) it
    // must trap out-of-fuel -> budget_exceeded; under the default 65536-node budget (fuel =
    // 6.5x10^8) the SAME guest completes. That is the K x max_nodes mapping, observed end to end.
    const SandboxedStepResult starved = runner().run_step(step_for("spin.wasm", 1), R"({"a":1})");
    CHECK(!starved.ok);
    CHECK(starved.budget_exceeded);
    CHECK(starved.detail.find("fuel exhausted") != std::string::npos);

    const SandboxedStepResult fed = runner().run_step(step_for("spin.wasm", 65536), R"({"a":1})");
    CHECK(fed.ok);
    CHECK(fed.output == R"({"a":1})"); // spin ends in the identity output
}

void test_endless_guest_terminates_as_budget_exceeded()
{
    // An endless loop is EXACTLY what L-37's deterministic budget exists for: it must terminate
    // as budget_exceeded (never hang, never wall-clock), quickly under a small budget.
    const SandboxedStepResult r = runner().run_step(step_for("infinite.wasm", 64), "{}");
    CHECK(!r.ok);
    CHECK(r.budget_exceeded);
}

// ---- through the C API: metering determinism + the fuel-cost micro-validation ----------------

// A plain compute-loop module for direct func calls (not the ctx_* ABI — this half of the suite
// probes the METERING, not the seam).
constexpr const char* kWatBench = R"WAT(
(module
  (func (export "spin") (param $n i32) (result i32)
    (local $acc i32)
    (block $done
      (loop $l
        (br_if $done (i32.eqz (local.get $n)))
        (local.set $acc (i32.add (local.get $acc) (local.get $n)))
        (local.set $n (i32.sub (local.get $n) (i32.const 1)))
        (br $l)))
    (local.get $acc)))
)WAT";

// One engine+module+instance under a Cranelift config with fuel ON or OFF; drives spin(n).
class BenchVm
{
public:
    explicit BenchVm(bool consume_fuel) : fueled_(consume_fuel)
    {
        wasm_config_t* config = wasm_config_new();
        CHECK(config != nullptr);
        wasmtime_config_strategy_set(config, WASMTIME_STRATEGY_CRANELIFT);
        wasmtime_config_consume_fuel_set(config, consume_fuel);
        wasmtime_config_cranelift_nan_canonicalization_set(config, true);
        engine_ = wasm_engine_new_with_config(config);
        CHECK(engine_ != nullptr);
        const std::string bytes = wasmtest::compile_wat(kWatBench);
        CHECK(!bytes.empty());
        CHECK(wasmtime_module_new(engine_,
                                  reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                  bytes.size(), &module_) == nullptr);
        store_ = wasmtime_store_new(engine_, nullptr, nullptr);
        CHECK(store_ != nullptr);
        context_ = wasmtime_store_context(store_);
        if (consume_fuel) // a fueled store starts at 0 fuel — grant before instantiation
            CHECK(wasmtime_context_set_fuel(context_, kFuelTank) == nullptr);
        wasm_trap_t* trap = nullptr;
        CHECK(wasmtime_instance_new(context_, module_, nullptr, 0, &instance_, &trap) == nullptr);
        CHECK(trap == nullptr);
        wasmtime_extern_t item{};
        CHECK(wasmtime_instance_export_get(context_, &instance_, "spin", 4, &item));
        CHECK(item.kind == WASMTIME_EXTERN_FUNC);
        spin_ = item.of.func;
    }

    ~BenchVm()
    {
        if (store_ != nullptr)
            wasmtime_store_delete(store_);
        if (module_ != nullptr)
            wasmtime_module_delete(module_);
        if (engine_ != nullptr)
            wasm_engine_delete(engine_);
    }

    BenchVm(const BenchVm&) = delete;
    BenchVm& operator=(const BenchVm&) = delete;

    void refuel() // no-op on the unfueled engine (set_fuel errors when fuel is not configured)
    {
        if (fueled_)
            CHECK(wasmtime_context_set_fuel(context_, kFuelTank) == nullptr);
    }

    [[nodiscard]] std::uint64_t fuel_remaining()
    {
        std::uint64_t fuel = 0;
        CHECK(wasmtime_context_get_fuel(context_, &fuel) == nullptr);
        return fuel;
    }

    std::int32_t spin(std::int32_t n)
    {
        wasmtime_val_t arg{};
        arg.kind = WASMTIME_I32;
        arg.of.i32 = n;
        wasmtime_val_t ret{};
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* error = wasmtime_func_call(context_, &spin_, &arg, 1, &ret, 1, &trap);
        CHECK(error == nullptr);
        CHECK(trap == nullptr);
        return ret.of.i32;
    }

    static constexpr std::uint64_t kFuelTank = 4000000000ull; // never exhausted by these probes

private:
    bool fueled_ = false;
    wasm_engine_t* engine_ = nullptr;
    wasmtime_module_t* module_ = nullptr;
    wasmtime_store_t* store_ = nullptr;
    wasmtime_context_t* context_ = nullptr;
    wasmtime_instance_t instance_{};
    wasmtime_func_t spin_{};
};

void test_fuel_metering_is_deterministic_and_linear()
{
    BenchVm vm(/*consume_fuel=*/true);
    constexpr std::int32_t kReps = 100000;

    vm.refuel();
    (void)vm.spin(kReps);
    const std::uint64_t used_once = BenchVm::kFuelTank - vm.fuel_remaining();

    vm.refuel();
    (void)vm.spin(kReps);
    const std::uint64_t used_again = BenchVm::kFuelTank - vm.fuel_remaining();

    vm.refuel();
    (void)vm.spin(2 * kReps);
    const std::uint64_t used_double = BenchVm::kFuelTank - vm.fuel_remaining();

    // Deterministic: the identical call consumes the IDENTICAL fuel (no wall clock, no sampling).
    CHECK(used_once == used_again);
    CHECK(used_once > 0);
    // Linear: doubling the work lands within a wide [1.5x, 2.5x] band of doubling the fuel (the
    // constant call overhead keeps it off exactly 2.0x).
    CHECK(used_double > used_once + used_once / 2);
    CHECK(used_double < 2 * used_once + used_once / 2);
}

void test_fuel_instrumentation_overhead_is_bounded()
{
    // The requirement-(6) micro-validation: consume_fuel adds decrement-and-check instrumentation
    // to guest code — a small multiplier on a tight loop, NOT a multiplicative blowup. Measured as
    // the median-of-5 wall-time ratio of the SAME module under fuel-on vs fuel-off engines, same
    // process, same machine — a self-relative ratio, so runner speed cancels out. The bound is
    // deliberately generous (8x; expected ~1.5-3x) to stay flake-free on loaded CI runners while
    // still catching a blowup regression (a misconfigured strategy or per-instruction trap path
    // shows up as 50x+). Sanitizer instrumentation skews both sides but ALSO adds noise — widen
    // under sanitizer builds per the repo's TSan-aware budget-assertion convention.
    BenchVm fueled(/*consume_fuel=*/true);
    BenchVm unfueled(/*consume_fuel=*/false);
    constexpr std::int32_t kReps = 20000000; // ~10-30 ms unfueled under Cranelift

    const auto median_ms = [](BenchVm& vm) {
        (void)vm.spin(1000); // warm the code path
        std::vector<double> samples;
        for (int i = 0; i < 5; ++i)
        {
            vm.refuel(); // fueled engine only (no-op on the unfueled one)
            const auto begin = std::chrono::steady_clock::now();
            (void)vm.spin(kReps);
            const auto end = std::chrono::steady_clock::now();
            samples.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
        }
        std::sort(samples.begin(), samples.end());
        return samples[samples.size() / 2];
    };

    const double fueled_ms = median_ms(fueled);
    const double unfueled_ms = median_ms(unfueled);
    CHECK(unfueled_ms > 0.0);
    const double ratio = fueled_ms / unfueled_ms;
    std::fprintf(stderr,
                 "fuel-overhead micro-validation: fueled %.2f ms, unfueled %.2f ms, ratio %.2fx\n",
                 fueled_ms, unfueled_ms, ratio);
#if defined(CONTEXT_TSAN_BUILD) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    const double bound = 32.0; // sanitizer-instrumented builds: widened, still catches blowups
#else
    const double bound = 8.0;
#endif
    CHECK(ratio < bound);
}

} // namespace

int main()
{
    test_fuel_grant_scales_with_the_budget();
    test_endless_guest_terminates_as_budget_exceeded();
    test_fuel_metering_is_deterministic_and_linear();
    test_fuel_instrumentation_overhead_is_bounded();
    WASM_TEST_MAIN_END();
}
