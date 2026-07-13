// A test/reference MigrationRunner that stands in for a compiled package guest WITHOUT running any
// wasm. It exercises EXACTLY the frozen guest ABI surface (migration_runner.h) — canonical-JSON
// bytes in, canonical-JSON bytes out, an optional path map — and NOTHING the real wasmtime runner
// (issue #71 PR3) will not also expose. The "guest" callbacks are byte-only ON PURPOSE: they never
// see the host JsonValue tree, so a test cannot accidentally make the mock more capable than the
// sandbox contract permits (the c8aaa0ac lesson — an over-capable mock masks real bugs). Every
// structural invariant (budget, canonical serializability, id immutability) is re-checked host-side
// by the migrate engine, exactly as it will be for the real runner.

#pragma once

#include "context/editor/migrate/migration_runner.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace migratetest
{

class MockMigrationRunner final : public context::editor::migrate::MigrationRunner
{
public:
    using SandboxedStep = context::editor::migrate::SandboxedStep;
    using SandboxedStepResult = context::editor::migrate::SandboxedStepResult;

    // The stand-in for the module's ctx_migrate export: canonical-JSON input bytes -> canonical-JSON
    // output bytes. Returns false to model a guest-reported failure (a non-zero ctx_migrate return).
    using GuestMigrate = std::function<bool(std::string_view input, std::string& output)>;
    // The stand-in for the OPTIONAL ctx_map_path export: pointer bytes -> mapped pointer, or nullopt
    // for "unmapped" (the orphan case). A default-constructed map means the module OMITS the export
    // (identity — every path survives the step unchanged).
    using GuestMapPath = std::function<std::optional<std::string>(std::string_view pointer)>;

    explicit MockMigrationRunner(GuestMigrate migrate, GuestMapPath map = {})
        : migrate_(std::move(migrate)), map_(std::move(map))
    {
    }

    [[nodiscard]] SandboxedStepResult run_step(const SandboxedStep& step,
                                               std::string_view input) override
    {
        ++run_calls;
        SandboxedStepResult result;
        // Model deterministic VM fuel exhaustion (the real runner's K × max_nodes budget->fuel
        // mapping, issue #71 PR3): a budget failure, not a step failure — the engine maps it to
        // migration.budget_exceeded.
        if (report_budget_exceeded)
        {
            result.budget_exceeded = true;
            result.detail = "fuel exhausted (mock)";
            return result;
        }
        // A real runner has nothing to instantiate without a module reference. (register_step
        // rejects an empty wasm ref, but a runner never assumes its caller validated.)
        if (step.wasm_module.empty())
        {
            result.detail = "no wasm module reference to instantiate";
            return result;
        }
        if (!migrate_)
        {
            result.detail = "module exports no ctx_migrate";
            return result;
        }
        std::string output;
        if (!migrate_(input, output))
        {
            result.detail = "guest ctx_migrate returned non-zero";
            return result;
        }
        result.ok = true;
        result.output = std::move(output);
        return result;
    }

    [[nodiscard]] std::optional<std::string> map_path(const SandboxedStep& step,
                                                      std::string_view pointer) override
    {
        ++map_calls;
        (void)step;
        if (!map_)
            return std::string(pointer); // module omits ctx_map_path ⇒ identity (frozen ABI)
        return map_(pointer);
    }

    // Seam observability: tests assert the engine actually ROUTED through the runner (not merely
    // that the outcome looked right).
    int run_calls = 0;
    int map_calls = 0;

    // When true, EVERY run_step reports deterministic fuel exhaustion (budget_exceeded=true) —
    // the mock knob for the budget->fuel failure path.
    bool report_budget_exceeded = false;

private:
    GuestMigrate migrate_;
    GuestMapPath map_;
};

} // namespace migratetest
