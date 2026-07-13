// The CROSS-OS determinism gate for the committed WASM migration fixture (issue #71 PR4, L-37 /
// L-62 / R-SIM-005). The deterministic sandboxed tier's cross-platform guarantee is BYTE-IDENTICAL
// migrated OUTPUT: the SAME migration module on the SAME input produces the SAME output bytes on
// every platform of the wedge (Linux-x64 / Win-x64 / macOS-ARM64). That is the determinism contract
// the engine actually depends on — a document migrates to the same canonical bytes everywhere.
//
// Fuel is the OTHER half of L-37 (deterministic VM metering replaces wall-clock limits), but its
// guarantee is narrower, and this gate asserts exactly the part that holds. Fuel is a PURE,
// DETERMINISTIC function of (module, input) WITHIN a given platform/wasmtime build — identical on
// every repeated run — which is what makes the fail-closed budget (fuel = K x max_nodes)
// reproducible on any one machine. Fuel is NOT byte-equal ACROSS the wedge, and cannot be made so:
// the observed fuel carries a small platform-specific memory-INITIALIZATION component that tracks
// the host OS page size and is elided entirely by copy-on-write memory init on Linux. On the pinned
// wasmtime 46.0.1 C-API prebuilt the identical fixture+input spends fuel =
//   { Linux-x64: 24, Win-x64: 4121, macOS-ARM64: 16409 }
// i.e. ~24 guest-execution units PLUS ~one host OS page (4 KiB on x64, 16 KiB on ARM64) on the
// non-CoW platforms. The two x86_64 legs differ too, so this is a per-BUILD / per-OS artifact, NOT a
// per-ISA one — no wasmtime/Cranelift config knob equalizes it without changing the runner's own
// deterministic memory-init config in a fragile, platform-dependent way. Pinning a single cross-OS
// fuel golden was therefore based on a false premise and has been removed. The cross-platform fuel
// delta (~one OS page) is dwarfed by the K x max_nodes budget headroom, so it never changes a
// document's migrate-vs-budget_exceeded verdict in practice.
//
// This gate drives the COMMITTED fixture (src/runtime/wasm/fixtures/migrate_hp.wasm — the reproducible
// checked-in artifact, NOT a runtime-assembled WAT string) two ways:
//   (a) through the REAL WasmRunner + migrate_document, proving the committed bytes slot into the
//       actual runner and the full L-37 document pipeline; and
//   (b) through a direct wasmtime C-API harness under the SAME deterministic config the runner uses,
//       reading the exact fuel spent (the runner's seam surface intentionally hides it).
// It asserts: (1) byte-identical migrated output against a committed golden, EQUAL on every leg (the
// real cross-OS contract); (2) within-platform determinism — three independent fresh engines+stores
// reproduce output AND fuel EXACTLY, fuel > 0 (the basis of the fail-closed budget); and (3) a coarse
// portable sanity ceiling on the fuel so a metering-off / runaway-metering regression still reddens
// without pinning an OS-page-size-dependent magic number. Registered as
// `wasm-runner-test_wasm_determinism`, run by the 3-OS `wasm-runner` CI job.
//
// If a future need ever arises to pin the ABSOLUTE per-platform fuel magnitude, the honest mechanism
// is per-OS goldens (one constant guarded per platform), NOT a single cross-OS value; it is omitted
// here on purpose because that magnitude is an incidental OS-page/CoW artifact, and gating on it
// would flake on a CI-runner image change for no correctness reason.

#include "wasm_test.h"

#include "context/editor/migrate/migrate_document.h"
#include "context/editor/migrate/migration_set.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"
#include "context/runtime/wasm/wasm_runner.h"

#include <wasmtime.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

using namespace context::editor::migrate;
using context::editor::serializer::JsonValue;
using context::runtime::wasm::kWasmMemoryLimitBytes;
using context::runtime::wasm::WasmRunner;

namespace
{

// The committed golden output: the raw canonical-JSON bytes ctx_migrate emits (phys:body v1 -> v2).
// Byte-identical on every platform; a mismatch means the fixture or the guest ABI changed.
constexpr const char* kGoldenOutput = R"({"hp":2})";

// A coarse, PORTABLE ceiling on the fixture's fuel. The fixture's guest work is only a couple dozen
// wasm ops, and even the largest wedge measurement (macOS-ARM64: 16409 — ~one 16 KiB OS page of
// non-CoW memory-init overhead, see the file header) sits far below this. This is deliberately NOT a
// tight golden — fuel's exact magnitude is a per-platform artifact — but a fuel that blows past this
// ceiling means metering broke (a per-instruction trap path / misconfigured strategy / runaway),
// which this gate must still catch. The tight, work-scaled metering-overhead check lives in
// test_wasm_fuel.cpp; here the within-run equality below is the primary determinism assertion.
constexpr std::uint64_t kFuelSanityCeiling = std::uint64_t{1} << 20; // 1,048,576 (~64x the max leg)

// One measured drive of the fixture through the deterministic wasmtime config, returning the migrated
// output bytes AND the fuel spent. Mirrors WasmRunner::create()'s config + invoke_guest()'s ABI
// sequence (alloc input, alloc the two out-cells, write input, call ctx_migrate) so the measurement
// is representative of a real run_step; drives the C-API directly only to read the fuel the seam hides.
struct MeasuredRun
{
    std::string output;
    std::uint64_t fuel_spent = 0;
    bool ok = false;
};

MeasuredRun run_fixture_measured(const std::string& wasm_bytes, std::string_view input)
{
    MeasuredRun run;

    // Deterministic engine config — IDENTICAL to WasmRunner::create() (the observed fuel must match
    // the runner's execution, so every knob that affects codegen/metering is pinned the same way).
    wasm_config_t* config = wasm_config_new();
    CHECK(config != nullptr);
    wasmtime_config_strategy_set(config, WASMTIME_STRATEGY_CRANELIFT);
    wasmtime_config_consume_fuel_set(config, true);
    wasmtime_config_cranelift_nan_canonicalization_set(config, true);
    wasmtime_config_wasm_relaxed_simd_deterministic_set(config, true);
    wasmtime_config_wasm_relaxed_simd_set(config, false);
    wasmtime_config_wasm_threads_set(config, false);
    wasmtime_config_shared_memory_set(config, false);
    wasm_engine_t* engine = wasm_engine_new_with_config(config);
    CHECK(engine != nullptr);

    wasmtime_module_t* module = nullptr;
    CHECK(wasmtime_module_new(engine, reinterpret_cast<const std::uint8_t*>(wasm_bytes.data()),
                              wasm_bytes.size(), &module) == nullptr);

    wasmtime_store_t* store = wasmtime_store_new(engine, nullptr, nullptr);
    CHECK(store != nullptr);
    wasmtime_context_t* ctx = wasmtime_store_context(store);
    wasmtime_store_limiter(store, static_cast<std::int64_t>(kWasmMemoryLimitBytes),
                           /*table_elements=*/10000, /*instances=*/1, /*tables=*/1, /*memories=*/1);
    constexpr std::uint64_t kGrant = 100000000ull; // ample: the fixture never exhausts it
    CHECK(wasmtime_context_set_fuel(ctx, kGrant) == nullptr);

    wasmtime_instance_t instance{};
    {
        wasm_trap_t* trap = nullptr;
        CHECK(wasmtime_instance_new(ctx, module, nullptr, 0, &instance, &trap) == nullptr);
        CHECK(trap == nullptr);
    }

    wasmtime_extern_t item{};
    CHECK(wasmtime_instance_export_get(ctx, &instance, "memory", 6, &item));
    CHECK(item.kind == WASMTIME_EXTERN_MEMORY);
    wasmtime_memory_t memory = item.of.memory;
    CHECK(wasmtime_instance_export_get(ctx, &instance, "ctx_alloc", 9, &item));
    CHECK(item.kind == WASMTIME_EXTERN_FUNC);
    wasmtime_func_t alloc_fn = item.of.func;
    CHECK(wasmtime_instance_export_get(ctx, &instance, "ctx_migrate", 11, &item));
    CHECK(item.kind == WASMTIME_EXTERN_FUNC);
    wasmtime_func_t migrate_fn = item.of.func;

    const auto alloc = [&](std::int32_t size) -> std::int32_t {
        wasmtime_val_t arg{};
        arg.kind = WASMTIME_I32;
        arg.of.i32 = size;
        wasmtime_val_t ret{};
        wasm_trap_t* trap = nullptr;
        CHECK(wasmtime_func_call(ctx, &alloc_fn, &arg, 1, &ret, 1, &trap) == nullptr);
        CHECK(trap == nullptr);
        return ret.of.i32;
    };

    const std::int32_t in_ptr = alloc(static_cast<std::int32_t>(input.size()));
    const std::int32_t cells_ptr = alloc(8);
    {
        std::uint8_t* data = wasmtime_memory_data(ctx, &memory);
        std::memcpy(data + in_ptr, input.data(), input.size());
        std::memset(data + cells_ptr, 0, 8);
    }

    {
        wasmtime_val_t args[4];
        for (wasmtime_val_t& a : args)
            a.kind = WASMTIME_I32;
        args[0].of.i32 = in_ptr;
        args[1].of.i32 = static_cast<std::int32_t>(input.size());
        args[2].of.i32 = cells_ptr;
        args[3].of.i32 = cells_ptr + 4;
        wasmtime_val_t ret{};
        wasm_trap_t* trap = nullptr;
        CHECK(wasmtime_func_call(ctx, &migrate_fn, args, 4, &ret, 1, &trap) == nullptr);
        CHECK(trap == nullptr);
        run.ok = (ret.of.i32 == 0);
    }

    {
        std::uint8_t* data = wasmtime_memory_data(ctx, &memory);
        std::uint32_t out_ptr = 0;
        std::uint32_t out_len = 0;
        std::memcpy(&out_ptr, data + cells_ptr, 4);
        std::memcpy(&out_len, data + cells_ptr + 4, 4);
        run.output.assign(reinterpret_cast<const char*>(data + out_ptr), out_len);
    }

    std::uint64_t remaining = 0;
    CHECK(wasmtime_context_get_fuel(ctx, &remaining) == nullptr);
    run.fuel_spent = kGrant - remaining;

    wasmtime_store_delete(store);
    wasmtime_module_delete(module);
    wasm_engine_delete(engine);
    return run;
}

// The committed fixture also migrates a whole document through the REAL runner (proving the checked-in
// bytes work end to end, not only in the direct-C-API harness).
void test_fixture_migrates_a_document_through_the_real_runner(const std::string& wasm)
{
    context::runtime::wasm::ModuleResolver resolver =
        [&wasm](std::string_view ref, std::string& bytes, std::string& problem) {
            if (ref == "migrate_hp")
            {
                bytes = wasm;
                return true;
            }
            problem = "unknown fixture module";
            return false;
        };
    std::string problem;
    std::unique_ptr<WasmRunner> runner = WasmRunner::create(resolver, problem);
    CHECK(runner != nullptr);

    MigrationSet set;
    CHECK(set.register_component("phys:body", 2, problem));
    MigrationStep step;
    step.component_type = "phys:body";
    step.from_version = 1;
    step.tier = MigrationTier::package_sandboxed;
    step.wasm_module = "migrate_hp";
    CHECK(set.register_step(std::move(step), problem));

    // run_step raw output is the golden bytes.
    SandboxedStep desc;
    desc.wasm_module = "migrate_hp";
    desc.component_type = "phys:body";
    desc.from_version = 1;
    const SandboxedStepResult sr = runner->run_step(desc, R"({"hp":1})");
    CHECK(sr.ok);
    CHECK(sr.output == kGoldenOutput);

    // Whole-document migration: stamped v1 payload lifts to v2, canonically.
    context::editor::serializer::ParseResult doc =
        context::editor::serializer::parse_json(
            R"({"componentVersions": {"phys:body": 1}, "c": {"phys:body": {"hp": 1}}})");
    CHECK(doc.ok);
    MigrateOptions options;
    options.runner = runner.get();
    const DocumentMigrationResult r = migrate_document(doc.root, set, options);
    CHECK(r.ok);
    CHECK(r.changed);
    std::string canonical;
    CHECK(context::editor::serializer::serialize_canonical(doc.root, canonical));
    CHECK(canonical.find("\"hp\": 2") != std::string::npos);
    CHECK(canonical.find("\"phys:body\": 2") != std::string::npos);
}

} // namespace

int main()
{
    const std::string wasm = wasmtest::load_fixture("migrate_hp.wasm");
    CHECK(!wasm.empty());
    // The committed fixture really is a WASM module (magic "\0asm").
    CHECK(wasm.size() >= 8 && std::memcmp(wasm.data(), "\0asm\1\0\0\0", 8) == 0);

    test_fixture_migrates_a_document_through_the_real_runner(wasm);

    const std::string input = R"({"hp":1})";
    const MeasuredRun a = run_fixture_measured(wasm, input);
    const MeasuredRun b = run_fixture_measured(wasm, input);
    const MeasuredRun c = run_fixture_measured(wasm, input);

    std::fprintf(stderr, "[wasm-determinism] output=%s fuel=%llu\n", a.output.c_str(),
                 static_cast<unsigned long long>(a.fuel_spent));

    // --- the migration succeeded and produced the golden bytes ------------------------------------
    CHECK(a.ok);
    CHECK(a.output == kGoldenOutput);

    // --- within-platform determinism: three INDEPENDENT fresh engines+stores reproduce output AND
    // fuel EXACTLY (run_fixture_measured builds a brand-new engine each call). No wall clock, no
    // sampling — fuel is a pure function of module+input under the pinned config, which is exactly
    // what the fail-closed budget (fuel = K x max_nodes) relies on being reproducible on a machine.
    CHECK(a.output == b.output);
    CHECK(b.output == c.output);
    CHECK(a.fuel_spent == b.fuel_spent);
    CHECK(b.fuel_spent == c.fuel_spent);
    CHECK(a.fuel_spent > 0);

    // --- the CROSS-OS contract: byte-identical migrated OUTPUT on every leg of the wedge -----------
    // Asserted above (a.output == kGoldenOutput) and by the whole-document byte-check in
    // test_fixture_migrates_a_document_through_the_real_runner. Fuel is intentionally NOT asserted
    // equal across the wedge — it carries a platform-specific memory-init component (see the file
    // header: Linux-x64 24 / Win-x64 4121 / macOS-ARM64 16409, all with byte-identical output). What
    // IS portable is that metering stayed ON, deterministic (above), and bounded: the coarse sanity
    // ceiling catches a metering-off (fuel == 0, already checked) or runaway-metering regression
    // without pinning an OS-page-size-dependent magic number.
    CHECK(a.fuel_spent < kFuelSanityCeiling);

    WASM_TEST_MAIN_END();
}
