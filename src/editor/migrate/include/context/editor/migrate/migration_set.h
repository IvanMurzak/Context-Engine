// The registered migration set — per-component-payload schema versions + the ordered migration
// steps between them (L-37 / R-DATA-004). With the kind SchemaSet this IS the R-FILE-005 pass-0
// "registered schema + migration set": its content hash is a pass-1 derivation cache-key component.

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::migrate
{

// Execution tiers (L-37). Engine-shipped migrations are first-party C++ run in-process NOW;
// package-shipped migrations execute ONLY in the sandboxed WASM tier, routed through an INJECTED
// MigrationRunner (migration_runner.h — the wasmtime runtime, issue #71). With no runner injected
// the tier boundary keeps the honest migration.runner_unavailable refusal (the CONTRACT — budget,
// purity, determinism, CI round-trips — is registered; package steps are never run unsandboxed).
// The VM is an EditorKernel component booted before pass-1 parsing (R-FILE-005 cold-start order:
// lock → index → watcher → VM → registration → content parse). See MigrationBudget and
// migrate_document.h for the contract's enforcement points.
enum class MigrationTier
{
    engine_native,     // first-party, in-process; purity is CI-enforced (fixture round-trips)
    package_sandboxed, // WASM sandboxed tier ONLY — routed through the injected MigrationRunner seam
};

// The per-invocation execution budget (L-37: "a pathological migration cannot hang derivation").
// The sandboxed tier maps this to VM fuel/instruction metering when the runner lands; for the
// engine-native tier the engine enforces the deterministic post-hoc analog: a step whose INPUT or
// OUTPUT payload exceeds `max_nodes` JSON nodes is refused (migration.budget_exceeded) and the
// payload rolls back. Wall-clock limits are deliberately NOT part of the contract — they are
// nondeterministic, and determinism is a migration-contract requirement.
struct MigrationBudget
{
    std::uint64_t max_nodes = 65536; // max JSON nodes in a step's input/output payload
};

// A pure payload transform (engine-native tier): mutates the component payload in place, returns
// false on failure. PURITY CONTRACT (L-37): deterministic and side-effect-free — the function sees
// ONLY the payload; no IO, no clock, no randomness, no ambient state. Purity is pinned by the
// R-QA-011 fixture corpus (every registered step ships a pre/post fixture pair, round-tripped in
// CI forever) and the engine's structural checks (id immutability, budget) around every invocation.
using PayloadTransform = std::function<bool(serializer::JsonValue& payload)>;

// The payload-relative path map for one step: where does the value at OLD payload-relative JSON
// pointer `old_pointer` live AFTER the step? Returns the new pointer, or nullopt when the path has
// NO destination in the new schema — an override/reference addressing it becomes an orphan
// (migration.orphan_override, excluded from flatten; L-37). A null map means IDENTITY (a step that
// only adds fields keeps every existing path).
using PathTransform = std::function<std::optional<std::string>(std::string_view old_pointer)>;

// One registered migration step: component payloads of `component_type` stamped `from_version`
// migrate to `from_version + 1`. Steps chain: v1→v2→v3 reaches version 3 from 1.
struct MigrationStep
{
    std::string component_type;  // "<ns>:<type>", e.g. "ctx:transform" (engine) / "phys:body" (pkg)
    std::int64_t from_version = 0; // migrates from_version -> from_version + 1
    MigrationTier tier = MigrationTier::engine_native;
    // The step BODY's identity for the set hash. Native code cannot be content-hashed, so each
    // step carries an explicit revision, bumped whenever the step's behavior changes; the fixture
    // corpus (R-QA-011) pins the behavior each revision must reproduce. For the sandboxed tier the
    // wasm module reference is hashed alongside.
    std::uint64_t revision = 1;
    PayloadTransform transform; // engine_native: the in-process pure function
    PathTransform map_path;     // payload-relative pointer map; null = identity
    std::string wasm_module;    // package_sandboxed: the module reference the injected runner's
                                // ModuleResolver resolves to wasm bytes (issue #71 PR3)
    // The CONTENT hash of the resolved wasm module bytes (issue #71 PR4 / R-FILE-010). set_hash()
    // folds THIS, not just wasm_module, so two package versions that ship DIFFERENT module bytes
    // under the SAME reference string get DIFFERENT pass-1 cache keys — a rebuilt migration module
    // never serves derived state computed under the old bytes. Populated at package registration
    // (pass 0) from the resolved module bytes, e.g. `hash_combine(0, module_bytes)`; 0 until then
    // (an engine_native step, or a package step whose bytes have not been resolved yet). The runner
    // itself never sees this — it is a registration-time identity input to the set hash.
    std::uint64_t wasm_module_hash = 0;
};

// FNV-1a-style 64-bit hash combiner for set-hash composition (order-insensitive folds use XOR of
// per-entry hashes; per-entry hashes chain through this).
[[nodiscard]] std::uint64_t hash_combine(std::uint64_t seed, std::string_view bytes) noexcept;
[[nodiscard]] std::uint64_t hash_combine(std::uint64_t seed, std::uint64_t value) noexcept;

// The registered migration set: the CURRENT schema version per component type + the steps between
// versions. Engine-shipped entries register at startup (engine_set()); package-contributed entries
// join through the same add-path when package registration (pass 0) lands — a package upgrade that
// changes a migration changes set_hash(), which changes the pass-1 cache keys (R-FILE-010: new
// keys, never stale artifacts served under the old migration).
class MigrationSet
{
public:
    // Declare the CURRENT schema version of a component type (>= 1). Re-declaring replaces the
    // previous value (idempotent re-add, the SchemaSet pattern). Returns false (and appends to
    // `problem`) for a malformed type id (no ':') or version < 1.
    bool register_component(std::string component_type, std::int64_t current_version,
                            std::string& problem);

    // Register one migration step. Validates: type id contains ':', from_version >= 1,
    // revision >= 1, engine_native steps carry a transform, package_sandboxed steps carry a
    // wasm module reference, and (type, from_version) is not already registered (a duplicate is a
    // design error, refused — steps are write-once; behavior changes bump `revision` on a NEW
    // registration after removing the old one at design time, never silently).
    bool register_step(MigrationStep step, std::string& problem);

    // The CURRENT registered version of `component_type`; 0 when unregistered (unknown types are
    // never migrated or stamped — packages register incrementally).
    [[nodiscard]] std::int64_t current_version(std::string_view component_type) const noexcept;

    // The step migrating `component_type` FROM `from_version`; nullptr when absent (a gap in the
    // chain — selection surfaces migration.step_missing).
    [[nodiscard]] const MigrationStep* find_step(std::string_view component_type,
                                                 std::int64_t from_version) const noexcept;

    [[nodiscard]] const std::vector<MigrationStep>& steps() const noexcept { return steps_; }
    [[nodiscard]] bool empty() const noexcept { return steps_.empty() && components_.empty(); }

    // The content hash of the registered migration set (the R-FILE-005 pass-0 stratum's migration
    // half): registration-order-insensitive, sensitive to every component's current version and
    // every step's (type, from, tier, revision, wasm module). Combined with the kind-schema hash
    // at the composition layer, it enters every pass-1 derivation cache key (R-FILE-010).
    [[nodiscard]] std::uint64_t set_hash() const noexcept;

    // The engine-shipped migration set. EMPTY today — no engine component-payload schema has ever
    // bumped (the first engine schemas themselves land with the declarative component compiler).
    // The first real bump registers its step HERE and adds its R-QA-011 fixture pair under
    // src/editor/migrate/fixtures/ in the same PR (the fixture is a deliverable of the bump).
    [[nodiscard]] static const MigrationSet& engine_set();

private:
    struct ComponentVersion
    {
        std::string type;
        std::int64_t current = 0;
    };

    std::vector<ComponentVersion> components_;
    std::vector<MigrationStep> steps_;
};

} // namespace context::editor::migrate
