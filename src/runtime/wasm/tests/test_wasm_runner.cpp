// WasmRunner core suite (issue #71 PR3). REAL backend: the deterministic config observables (NaN
// canonicalization, fixed memory limit, fresh Store+instance per step, run-to-run determinism),
// the frozen guest ABI surface (identity/fixed-output migrate, ctx_alloc failure, out-of-bounds
// output, missing exports, guest-reported failure), the STRUCTURAL zero-import sandbox, non-fuel
// trap mapping, module resolution failures, and the optional ctx_map_path outcomes
// (mapped / unmapped / absent=identity). STUB backend: the honest refusal (create() fails; no
// half-real runner). Budget->fuel scaling and the fuel micro-validation live in test_wasm_fuel.

#include "wasm_test.h"

#include "context/runtime/wasm/wasm_runner.h"

#include <cstring>
#include <optional>
#include <string>

using context::editor::migrate::SandboxedStep;
using context::editor::migrate::SandboxedStepResult;
using context::runtime::wasm::WasmRunner;

#if defined(CONTEXT_WASM_HAS_RUNTIME)

using wasmtest::ModuleTable;

namespace
{

ModuleTable& table()
{
    static ModuleTable t = [] {
        ModuleTable built;
        built.add("identity.wasm", wasmtest::kWatIdentity);
        built.add("fixed.wasm", wasmtest::kWatFixedOutput);
        built.add("counter.wasm", wasmtest::kWatCounter);
        built.add("trap.wasm", wasmtest::kWatTrap);
        built.add("guest-error.wasm", wasmtest::kWatGuestError);
        built.add("no-migrate.wasm", wasmtest::kWatNoMigrate);
        built.add("imports.wasm", wasmtest::kWatImports);
        built.add("alloc-fails.wasm", wasmtest::kWatAllocFails);
        built.add("oob-output.wasm", wasmtest::kWatOobOutput);
        built.add("nan.wasm", wasmtest::kWatNan);
        built.add("grow-beyond.wasm", wasmtest::kWatGrowBeyondLimit);
        built.add("grow-within.wasm", wasmtest::kWatGrowWithinLimit);
        built.add("map-path.wasm", wasmtest::kWatMapPath);
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

SandboxedStep step_for(const char* module_ref)
{
    SandboxedStep step;
    step.wasm_module = module_ref;
    step.component_type = "test:probe";
    step.from_version = 1;
    return step; // default budget: 65536 nodes -> a generous deterministic fuel grant
}

void test_create_boots_the_deterministic_vm()
{
    CHECK(WasmRunner::runtime_available());
    (void)runner(); // CHECKs creation
}

void test_identity_migrate_round_trips_bytes()
{
    const std::string input = R"({"hp":1})";
    const SandboxedStepResult r = runner().run_step(step_for("identity.wasm"), input);
    CHECK(r.ok);
    CHECK(!r.budget_exceeded);
    CHECK(r.output == input);
}

void test_fixed_output_migrate_returns_guest_bytes()
{
    const SandboxedStepResult r = runner().run_step(step_for("fixed.wasm"), R"({"hp":1})");
    CHECK(r.ok);
    CHECK(r.output == R"({"hp":2})");
}

void test_fresh_store_and_instance_per_step()
{
    // The counter guest returns {"n":<calls-within-this-instance>}: any instance/store reuse
    // across run_step calls would surface as {"n":2} on the second call.
    const SandboxedStepResult first = runner().run_step(step_for("counter.wasm"), "{}");
    const SandboxedStepResult second = runner().run_step(step_for("counter.wasm"), "{}");
    CHECK(first.ok);
    CHECK(second.ok);
    CHECK(first.output == R"({"n":1})");
    CHECK(second.output == R"({"n":1})");
}

void test_identical_runs_are_deterministic()
{
    const std::string input = R"({"a":[1,2,3],"b":"x"})";
    const SandboxedStepResult r1 = runner().run_step(step_for("identity.wasm"), input);
    const SandboxedStepResult r2 = runner().run_step(step_for("identity.wasm"), input);
    CHECK(r1.ok);
    CHECK(r2.ok);
    CHECK(r1.output == r2.output);
}

void test_nan_canonicalization_pins_arithmetic_nan()
{
    // The guest computes 0.0/0.0 from runtime values and returns the raw f64 bits. With the
    // deterministic config's NaN canonicalization the result is the canonical positive quiet NaN
    // on every platform (x86 hardware alone would produce the sign-negative pattern).
    const SandboxedStepResult r = runner().run_step(step_for("nan.wasm"), "{}");
    CHECK(r.ok);
    CHECK(r.output.size() == 8);
    std::uint64_t bits = 0;
    std::memcpy(&bits, r.output.data(), 8);
    CHECK(bits == 0x7ff8000000000000ull);
}

void test_memory_growth_is_capped_by_the_fixed_limit()
{
    // 2048 pages = 128 MiB > kWasmMemoryLimitBytes: the store limiter DENIES the grow (the guest
    // observes -1 and reports "1"); a small in-limit grow succeeds ("0"). Deterministic, not a trap.
    const SandboxedStepResult beyond = runner().run_step(step_for("grow-beyond.wasm"), "{}");
    CHECK(beyond.ok);
    CHECK(beyond.output == "1");
    const SandboxedStepResult within = runner().run_step(step_for("grow-within.wasm"), "{}");
    CHECK(within.ok);
    CHECK(within.output == "0");
}

void test_import_declaring_module_fails_to_instantiate()
{
    // The zero-import sandbox is STRUCTURAL: instantiation passes an empty import list, so a
    // module declaring any import never runs at all.
    const SandboxedStepResult r = runner().run_step(step_for("imports.wasm"), "{}");
    CHECK(!r.ok);
    CHECK(!r.budget_exceeded);
    CHECK(r.detail.find("instantiation failed") != std::string::npos);
}

void test_non_fuel_trap_is_a_plain_step_failure()
{
    const SandboxedStepResult r = runner().run_step(step_for("trap.wasm"), "{}");
    CHECK(!r.ok);
    CHECK(!r.budget_exceeded); // unreachable is NOT a budget failure
    CHECK(r.detail.find("trap") != std::string::npos);
}

void test_guest_reported_failure_carries_the_code()
{
    const SandboxedStepResult r = runner().run_step(step_for("guest-error.wasm"), "{}");
    CHECK(!r.ok);
    CHECK(!r.budget_exceeded);
    CHECK(r.detail.find("returned 7") != std::string::npos);
}

void test_missing_ctx_migrate_is_refused()
{
    const SandboxedStepResult r = runner().run_step(step_for("no-migrate.wasm"), "{}");
    CHECK(!r.ok);
    CHECK(r.detail.find("ctx_migrate") != std::string::npos);
}

void test_alloc_failure_is_refused()
{
    const SandboxedStepResult r = runner().run_step(step_for("alloc-fails.wasm"), "{}");
    CHECK(!r.ok);
    CHECK(r.detail.find("ctx_alloc") != std::string::npos);
}

void test_out_of_bounds_output_is_refused()
{
    const SandboxedStepResult r = runner().run_step(step_for("oob-output.wasm"), "{}");
    CHECK(!r.ok);
    CHECK(r.detail.find("out-of-bounds") != std::string::npos);
}

void test_unresolvable_module_reference_is_refused()
{
    const SandboxedStepResult r = runner().run_step(step_for("nope.wasm"), "{}");
    CHECK(!r.ok);
    CHECK(r.detail.find("did not resolve") != std::string::npos);
}

void test_empty_module_reference_is_refused()
{
    const SandboxedStepResult r = runner().run_step(step_for(""), "{}");
    CHECK(!r.ok);
}

void test_map_path_mapped_unmapped_and_identity()
{
    const SandboxedStep with_map = step_for("map-path.wasm");
    // Mapped: /old -> /new (guest returns 0 + the mapped pointer bytes).
    const std::optional<std::string> mapped = runner().map_path(with_map, "/old");
    CHECK(mapped.has_value());
    CHECK(*mapped == "/new");
    // Unmapped: guest returns 1 -> the orphan-override outcome.
    CHECK(!runner().map_path(with_map, "/gone").has_value());
    // Everything else echoes through the guest (its own identity arm).
    const std::optional<std::string> echoed = runner().map_path(with_map, "/kept");
    CHECK(echoed.has_value());
    CHECK(*echoed == "/kept");
    // A module WITHOUT the optional export is IDENTITY by contract — the host never calls in.
    const std::optional<std::string> absent =
        runner().map_path(step_for("identity.wasm"), "/anything");
    CHECK(absent.has_value());
    CHECK(*absent == "/anything");
}

} // namespace

int main()
{
    test_create_boots_the_deterministic_vm();
    test_identity_migrate_round_trips_bytes();
    test_fixed_output_migrate_returns_guest_bytes();
    test_fresh_store_and_instance_per_step();
    test_identical_runs_are_deterministic();
    test_nan_canonicalization_pins_arithmetic_nan();
    test_memory_growth_is_capped_by_the_fixed_limit();
    test_import_declaring_module_fails_to_instantiate();
    test_non_fuel_trap_is_a_plain_step_failure();
    test_guest_reported_failure_carries_the_code();
    test_missing_ctx_migrate_is_refused();
    test_alloc_failure_is_refused();
    test_out_of_bounds_output_is_refused();
    test_unresolvable_module_reference_is_refused();
    test_empty_module_reference_is_refused();
    test_map_path_mapped_unmapped_and_identity();
    WASM_TEST_MAIN_END();
}

#else // stub backend

namespace
{

void test_stub_refuses_honestly()
{
    // The stub build must never hand out a half-real runner: create() fails with a clear problem
    // so embedders keep the seam's migration.runner_unavailable refusal.
    CHECK(!WasmRunner::runtime_available());
    std::string problem;
    const std::unique_ptr<WasmRunner> r = WasmRunner::create(
        [](std::string_view, std::string&, std::string&) { return false; }, problem);
    CHECK(r == nullptr);
    CHECK(problem.find("stub") != std::string::npos);
}

} // namespace

int main()
{
    test_stub_refuses_honestly();
    WASM_TEST_MAIN_END();
}

#endif // CONTEXT_WASM_HAS_RUNTIME
