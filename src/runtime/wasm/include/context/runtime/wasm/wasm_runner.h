// The deterministic sandboxed WASM migration runner (issue #71 PR3 / L-37 / L-62 / R-SEC-009):
// the concrete MigrationRunner (src/editor/migrate/migration_runner.h) backed by wasmtime-Cranelift
// via the SHA-pinned context_wasm prebuilt (PR1). This header is toolchain-neutral — it never
// includes wasmtime: the backend behind it is either the REAL VM (CONTEXT_WASM_HAS_RUNTIME, the
// 3-OS CI legs + any toolchain that can link the prebuilt) or the honest STUB (the local
// Strawberry-GCC Windows dev host, where the MSVC-ABI wasmtime.dll.lib cannot link — the same
// capability split as context_js), whose create() refuses so callers fall back to the seam's
// migration.runner_unavailable refusal instead of a half-real runner.

#pragma once

#include "context/editor/migrate/migration_runner.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace context::runtime::wasm
{

// ============================ Budget -> fuel (the K contract constant) ============================
//
// L-37 bans wall-clock limits (nondeterministic), so the sandboxed tier meters DETERMINISTIC VM
// fuel: before every ctx_migrate/ctx_map_path call the runner grants the fresh Store exactly
//
//     fuel = kWasmFuelPerBudgetNode × MigrationBudget::max_nodes
//
// (saturating at UINT64_MAX). Fuel exhaustion surfaces as SandboxedStepResult::budget_exceeded,
// which the migrate engine maps to the EXISTING migration.budget_exceeded catalog code + the
// all-or-nothing document rollback — the deterministic sandboxed analog of the engine-native
// tier's post-hoc node-count refusal.
//
// K is a CONTRACT CONSTANT pinned per engine version: wasmtime fuel is deterministic for a given
// module + input (roughly one unit per executed instruction), so changing K deterministically
// changes which guests fit a given budget. Bumping it is a deliberate, reviewed behavior change
// for the engine version that ships it — never an accident of editing this file (the same
// discipline as MigrationStep::revision and kGuestAbiVersion). The static_assert is the tripwire.
inline constexpr std::uint64_t kWasmFuelPerBudgetNode = 10000;
static_assert(kWasmFuelPerBudgetNode == 10000,
              "kWasmFuelPerBudgetNode is the pinned budget->fuel contract constant (issue #71 "
              "PR3); changing it changes which package migrations fit a MigrationBudget and must "
              "be reviewed as a per-engine-version behavior change");

// The fixed linear-memory ceiling of a migration Store (the deterministic config's "fixed
// linear-memory limit"): a guest may grow its exported memory only up to this many bytes; growth
// beyond it is DENIED (memory.grow returns -1 — deterministic, not a trap). 64 MiB comfortably
// holds a canonical-JSON payload at the default 65536-node budget plus guest working state while
// keeping a pathological guest bounded.
inline constexpr std::uint64_t kWasmMemoryLimitBytes = 64ull << 20;

// Resolve a MigrationStep::wasm_module reference to the module's wasm bytes. Injected (the
// GuidGenerator pattern): the runner itself performs NO IO — where module bytes come from (a
// package store when package registration lands, an in-memory table in tests) is the embedder's
// policy, keeping the runner deterministic and ambient-state-free. Return true + `wasm_bytes` on
// success; false + a short `problem` (folded into the step-failure detail) when the reference
// cannot be resolved.
using ModuleResolver =
    std::function<bool(std::string_view module_ref, std::string& wasm_bytes, std::string& problem)>;

// The wasmtime-Cranelift MigrationRunner (the L-37 sandboxed WASM tier, issue #71 PR3).
//
// Deterministic VM config (fixed at create(), asserted by the wasm-runner ctests):
//   - Cranelift strategy, consume_fuel ON (per-instruction metering — the budget->fuel mapping
//     above), NaN canonicalization ON (arithmetic NaN outputs collapse to the canonical pattern
//     on every platform), relaxed-SIMD deterministic AND disabled, threads/shared-memory OFF,
//     and the fixed linear-memory limit above.
//   - One FRESH Store + instance PER step invocation (the frozen seam contract): no state
//     survives across steps or payloads. The engine (compiled config) is shared — compilation is
//     pure; execution state is not.
//   - ZERO imports, structurally: instantiation passes an empty import list, so a module that
//     declares ANY import fails to instantiate (no WASI, no clock, no IO, no randomness — the
//     PR2 guest ABI's sandbox story, enforced rather than policed).
//
// VM init happens HERE, in create() — the R-FILE-005 cold-start "VM" slot (lock → index →
// watcher → VM → registration → parse): the embedder boots the runner once, before EditorKernel
// construction, and injects it via EditorKernelConfig::migration_runner so pass-1 parsing routes
// package_sandboxed steps through it from the first document.
class WasmRunner final : public editor::migrate::MigrationRunner
{
public:
    // Boot the VM (build + validate the deterministic engine config). Returns nullptr + `problem`
    // when the runtime cannot boot — including ALWAYS in the stub build (no linkable wasmtime on
    // this toolchain), so an embedder that cannot get a real runner injects nothing and the seam
    // keeps its honest migration.runner_unavailable refusal (never a half-real runner).
    [[nodiscard]] static std::unique_ptr<WasmRunner> create(ModuleResolver resolver,
                                                            std::string& problem);

    // Whether this build carries the real wasmtime backend (false in the stub build — the local
    // GCC dev host; mirrors the context_js stub split).
    [[nodiscard]] static bool runtime_available() noexcept;

    ~WasmRunner() override;
    WasmRunner(const WasmRunner&) = delete;
    WasmRunner& operator=(const WasmRunner&) = delete;

    // MigrationRunner (the frozen PR2 seam). run_step executes ctx_migrate on a fresh
    // Store+instance under the deterministic config; fuel exhaustion returns
    // budget_exceeded=true (-> migration.budget_exceeded), any other trap/guest failure returns
    // a plain failure with detail (-> migration.step_failed). map_path executes the OPTIONAL
    // ctx_map_path export (absent => identity); a guest error/trap during path mapping returns
    // nullopt — the conservative orphan-override outcome (non-blocking; entries are preserved
    // verbatim, L-37), since the seam's path-map signature deliberately has no error channel.
    [[nodiscard]] editor::migrate::SandboxedStepResult
    run_step(const editor::migrate::SandboxedStep& step, std::string_view input) override;
    [[nodiscard]] std::optional<std::string>
    map_path(const editor::migrate::SandboxedStep& step, std::string_view pointer) override;

private:
    struct Impl;
    explicit WasmRunner(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace context::runtime::wasm
