// The sandboxed-migration execution seam (L-37 / issue #71): the dependency-inverted boundary the
// package_sandboxed migration tier runs THROUGH. `migrate` defines this interface and DEPENDS ON
// NOTHING to do with WebAssembly — the wasm runtime (wasmtime, PR3 of the #71 chain) implements
// MigrationRunner in a SEPARATE library and is INJECTED into MigrateOptions / migrate_payload. So
// the parse-time migration engine never links a VM: with no runner injected it keeps the honest
// migration.runner_unavailable refusal; with one injected it routes package steps to the guest.
//
// This header also FREEZES the guest ABI (below) — the wire between the host runner and a compiled
// package migration module. The interface is byte-oriented on purpose: a real guest sees ONLY bytes
// in its own linear memory, never the host's JsonValue tree, so a MigrationRunner (and its test
// mock) can be no more capable than the sandbox it stands in for.

#pragma once

#include "context/editor/migrate/migration_set.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace context::editor::migrate
{

// The guest ABI is FROZEN at protocolMajor 1 (the M3-exit contract freeze, PR #113). Adding an
// export or changing an export's signature/data format below is a MAJOR contract change, not an
// additive one. The static_assert is the tripwire: bumping this integer must be a deliberate,
// reviewed protocolMajor bump, never an accident of editing this file.
inline constexpr std::uint32_t kGuestAbiVersion = 1;
static_assert(kGuestAbiVersion == 1,
              "the package_sandboxed guest ABI is frozen at protocolMajor 1 (issue #71 PR2); a "
              "bump is a MAJOR contract change and must be reviewed as one");

// ============================ Frozen guest ABI (protocolMajor 1) ============================
//
// A package ships a WebAssembly module that migrates ONE component payload across ONE schema
// version step (from_version -> from_version + 1). The module is PURE compute: it imports NOTHING
// from the host (ZERO imports — no WASI, no clock, no IO, no randomness, no ambient state), so its
// determinism (L-37) is structural rather than policed. The host drives it entirely through a small
// set of EXPORTS over the module's own exported linear memory. All byte buffers crossing the wire
// are canonical JSON (R-FILE-001) encoded UTF-8; all offsets/lengths are little-endian i32 into
// linear memory.
//
//   Exports the module MUST provide
//   -------------------------------
//     (memory) "memory"                          the module's exported linear memory.
//     ctx_alloc(size: i32) -> i32                allocate `size` bytes, return their offset
//                                                (0 = allocation failure). The host writes the
//                                                input payload bytes there before ctx_migrate.
//     ctx_migrate(in_ptr: i32, in_len: i32,
//                 out_ptr_ptr: i32,
//                 out_len_ptr: i32) -> i32       migrate the canonical-JSON payload at
//                                                [in_ptr, in_ptr+in_len). On success: write the
//                                                migrated canonical-JSON bytes into linear memory,
//                                                store their (offset, len) into the two i32 cells at
//                                                out_ptr_ptr / out_len_ptr, and return 0. ANY
//                                                non-zero return is a guest-reported failure (the
//                                                host maps it to migration.step_failed).
//
//   Exports the module MAY provide (optional)
//   -----------------------------------------
//     ctx_map_path(in_ptr: i32, in_len: i32,
//                  out_ptr_ptr: i32,
//                  out_len_ptr: i32) -> i32       map ONE payload-relative JSON pointer (canonical
//                                                UTF-8 bytes) through this step: return 0 + the
//                                                mapped-pointer (offset, len); 1 for "unmapped" (the
//                                                override addressing it is orphaned —
//                                                migration.orphan_override); any other non-zero for
//                                                a guest error. ABSENT ⇒ IDENTITY (every path
//                                                survives the step unchanged).
//
//   Budget -> fuel
//   --------------
//   Before ctx_migrate the host converts MigrationBudget::max_nodes to VM fuel (K × max_nodes, K
//   pinned per engine version) so a pathological guest cannot hang derivation with a DETERMINISTIC
//   limit (L-37 bans wall-clock limits). Fuel exhaustion is migration.budget_exceeded — the runner
//   reports it via SandboxedStepResult::budget_exceeded below. The K mapping and the fuel
//   machinery live in the RUNNER (kWasmFuelPerBudgetNode in src/runtime/wasm/wasm_runner.h, the
//   PR3 WasmRunner), never in this `migrate` library.
//
//   Host-side contract (identical to the engine_native tier — the guest is trusted for NOTHING)
//   -------------------------------------------------------------------------------------------
//   The engine enforces, AROUND every runner call, exactly the checks it applies to a first-party
//   step: input/output node budget, canonical serializability, and id/guid immutability. The runner
//   returns bytes; the host re-parses them, then runs the SAME structural gate. A guest cannot buy
//   itself out of any invariant by returning bytes the host would reject.
//
//   Instance lifecycle
//   ------------------
//   One FRESH Store + instance PER step invocation — no state survives across steps or across
//   payloads (a runtime concern the RUNNER owns in PR3; frozen here as the contract the seam
//   assumes).
//
// =============================================================================================

// One package_sandboxed step to execute, described in host terms. Byte payloads are passed
// SEPARATELY (see MigrationRunner) so this stays a small value the host fills from a MigrationStep.
struct SandboxedStep
{
    std::string_view wasm_module;    // MigrationStep::wasm_module — the module reference to instantiate
    std::string_view component_type; // "<ns>:<type>" — diagnostics only
    std::int64_t from_version = 0;   // migrates from_version -> from_version + 1
    MigrationBudget budget;          // the runner maps max_nodes -> VM fuel (K × max_nodes)
};

// The outcome of one ctx_migrate call.
struct SandboxedStepResult
{
    bool ok = false;    // true: `output` holds the migrated canonical-JSON bytes
    std::string output; // canonical JSON bytes of the migrated payload (meaningful iff ok)
    std::string detail; // short human-readable failure reason (meaningful iff !ok) — folded into
                        // the host's migration.step_failed / migration.budget_exceeded message
    // The failure was the DETERMINISTIC execution budget (VM fuel exhaustion — K × max_nodes, see
    // the Budget -> fuel section above): the host maps it to the EXISTING
    // migration.budget_exceeded catalog code instead of migration.step_failed, with the same
    // all-or-nothing rollback. Meaningful iff !ok. A HOST-side seam field, additive — NOT part of
    // the frozen guest ABI above (no export/signature/data-format change; kGuestAbiVersion holds).
    bool budget_exceeded = false;
};

// The sandboxed-migration execution seam. Injectable exactly like assetdb's GuidGenerator: the
// deterministic engine defines the boundary; the wasm VM (PR3) implements it; tests drive a mock.
// A MigrationRunner is trusted ONLY to move bytes through the frozen ABI — every structural
// invariant is re-checked host-side, so a runner (real or mock) can never be more capable than the
// sandbox contract permits.
class MigrationRunner
{
public:
    virtual ~MigrationRunner() = default;

    // Execute ctx_migrate for `step` on the canonical-JSON `input` payload bytes. Returns the
    // migrated canonical-JSON bytes (result.ok), or a failure with a short `detail` the host folds
    // into migration.step_failed. The runner instantiates a FRESH Store+instance per call (PR3).
    [[nodiscard]] virtual SandboxedStepResult run_step(const SandboxedStep& step,
                                                       std::string_view input) = 0;

    // Map ONE payload-relative JSON `pointer` through `step` (the optional ctx_map_path export).
    // Mirrors PathTransform exactly: the mapped pointer, or nullopt when the path has no
    // destination after the step (the orphan-override case). A module lacking the optional export
    // is IDENTITY — returns `pointer` unchanged.
    [[nodiscard]] virtual std::optional<std::string> map_path(const SandboxedStep& step,
                                                              std::string_view pointer) = 0;
};

} // namespace context::editor::migrate
