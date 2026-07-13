// MigrationRunner seam tests — the L-37 sandboxed WASM tier routing (issue #71 PR2). Coverage: a
// package_sandboxed step migrating through an INJECTED runner (the seam happy path), the honest
// migration.runner_unavailable fall-through when NO runner is injected, the host-side contract
// re-checks AROUND the guest (id immutability, budget, non-JSON output, guest-reported failure →
// rollback — the guest is trusted for nothing), and the optional ctx_map_path override rewrite
// (mapped / orphan / identity). The mock (tests/mock_runner.h) is byte-only, strictly to the frozen
// guest ABI — never more capable than the real wasmtime runner (PR3) will be.

#include "migrate_test.h"
#include "mock_runner.h"

#include "context/editor/migrate/migrate_document.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

using namespace context::editor::migrate;
using context::editor::serializer::JsonValue;
using migratetest::canon;
using migratetest::MockMigrationRunner;
using migratetest::parse;

namespace
{

bool has_code(const DocumentMigrationResult& r, std::string_view code)
{
    return std::any_of(r.diagnostics.begin(), r.diagnostics.end(),
                       [code](const MigrationDiagnostic& d) { return d.code == code; });
}

// A byte->byte "guest" (canonical JSON in, canonical JSON out) standing in for a compiled module's
// ctx_migrate: rename a top-level member `from`->`to`. Deliberately parses + reserializes BYTES —
// exactly what a real wasm guest does in its OWN linear memory, never touching the host tree.
bool guest_rename(std::string_view from, std::string_view to, std::string_view input,
                  std::string& output)
{
    context::editor::serializer::ParseResult p = context::editor::serializer::parse_json(input);
    if (!p.ok)
        return false;
    for (auto& m : p.root.members)
        if (m.key == from)
            m.key = std::string(to);
    return context::editor::serializer::serialize_canonical(p.root, output);
}

// Register "phys:body" at version 2 with ONE package_sandboxed step v1->v2 (module ref set).
void register_pkg_step(MigrationSet& set)
{
    std::string problem;
    CHECK(set.register_component("phys:body", 2, problem));
    MigrationStep s;
    s.component_type = "phys:body";
    s.from_version = 1;
    s.tier = MigrationTier::package_sandboxed;
    s.wasm_module = "phys/migrations.wasm";
    CHECK(set.register_step(std::move(s), problem));
}

// --- seam routing --------------------------------------------------------------------------

void test_sandboxed_step_runs_through_injected_runner()
{
    MigrationSet set;
    register_pkg_step(set);
    JsonValue doc =
        parse(R"({"componentVersions": {"phys:body": 1}, "c": {"phys:body": {"hp": 10}}})");

    MockMigrationRunner runner(
        [](std::string_view in, std::string& out) { return guest_rename("hp", "health", in, out); });
    MigrateOptions options;
    options.runner = &runner;

    const DocumentMigrationResult r = migrate_document(doc, set, options);
    CHECK(r.ok);
    CHECK(r.changed);
    CHECK(runner.run_calls == 1); // the seam routed the package step to the runner
    const std::string bytes = canon(doc);
    CHECK(bytes.find("\"health\": 10") != std::string::npos); // payload migrated by the guest
    CHECK(bytes.find("\"hp\"") == std::string::npos);
    CHECK(bytes.find("\"phys:body\": 2") != std::string::npos); // stamp bumped to current
}

void test_no_runner_keeps_honest_refusal()
{
    MigrationSet set;
    register_pkg_step(set);
    JsonValue doc =
        parse(R"({"componentVersions": {"phys:body": 1}, "c": {"phys:body": {"hp": 10}}})");
    const std::string before = canon(doc);
    const DocumentMigrationResult r = migrate_document(doc, set); // no runner injected
    CHECK(!r.ok);
    CHECK(has_code(r, "migration.runner_unavailable"));
    CHECK(canon(doc) == before); // never run unsandboxed, never half-applied
}

// --- host-side contract re-checks around the guest (trusted for nothing) --------------------

void test_guest_failure_is_step_failed_and_rolls_back()
{
    MigrationSet set;
    register_pkg_step(set);
    JsonValue doc =
        parse(R"({"componentVersions": {"phys:body": 1}, "c": {"phys:body": {"hp": 10}}})");
    const std::string before = canon(doc);
    MockMigrationRunner runner([](std::string_view, std::string&) { return false; });
    MigrateOptions options;
    options.runner = &runner;
    const DocumentMigrationResult r = migrate_document(doc, set, options);
    CHECK(!r.ok);
    CHECK(!r.changed);
    CHECK(runner.run_calls == 1);
    CHECK(has_code(r, "migration.step_failed"));
    CHECK(canon(doc) == before);
}

void test_runner_fuel_exhaustion_is_budget_exceeded_and_rolls_back()
{
    // The budget->fuel failure path (issue #71 PR3): a runner reporting DETERMINISTIC fuel
    // exhaustion (SandboxedStepResult::budget_exceeded — the real WasmRunner's K × max_nodes
    // grant ran out) maps to the EXISTING migration.budget_exceeded catalog code — NOT
    // step_failed — with the same all-or-nothing document rollback.
    MigrationSet set;
    register_pkg_step(set);
    JsonValue doc =
        parse(R"({"componentVersions": {"phys:body": 1}, "c": {"phys:body": {"hp": 10}}})");
    const std::string before = canon(doc);
    MockMigrationRunner runner([](std::string_view, std::string&) { return true; });
    runner.report_budget_exceeded = true;
    MigrateOptions options;
    options.runner = &runner;
    const DocumentMigrationResult r = migrate_document(doc, set, options);
    CHECK(!r.ok);
    CHECK(!r.changed);
    CHECK(runner.run_calls == 1);
    CHECK(has_code(r, "migration.budget_exceeded"));
    CHECK(!has_code(r, "migration.step_failed"));
    CHECK(canon(doc) == before);
}

void test_guest_non_json_output_is_step_failed()
{
    MigrationSet set;
    register_pkg_step(set);
    JsonValue doc =
        parse(R"({"componentVersions": {"phys:body": 1}, "c": {"phys:body": {"hp": 10}}})");
    const std::string before = canon(doc);
    MockMigrationRunner runner([](std::string_view, std::string& out) {
        out = "not json at all";
        return true;
    });
    MigrateOptions options;
    options.runner = &runner;
    const DocumentMigrationResult r = migrate_document(doc, set, options);
    CHECK(!r.ok);
    CHECK(has_code(r, "migration.step_failed"));
    CHECK(canon(doc) == before);
}

void test_runner_output_id_mutation_is_rejected_host_side()
{
    MigrationSet set;
    register_pkg_step(set);
    JsonValue doc = parse(
        R"({"componentVersions": {"phys:body": 1}, "c": {"phys:body": {"id": "b-1", "hp": 10}}})");
    const std::string before = canon(doc);
    // A guest that (mis)behaves by mutating an id — the host must reject it (id immutability is
    // re-checked host-side, uniform with the engine_native tier; the guest is trusted for nothing).
    MockMigrationRunner runner([](std::string_view in, std::string& out) {
        context::editor::serializer::ParseResult p = context::editor::serializer::parse_json(in);
        if (!p.ok)
            return false;
        for (auto& m : p.root.members)
            if (m.key == "id")
                m.value.string_value = "TAMPERED";
        return context::editor::serializer::serialize_canonical(p.root, out);
    });
    MigrateOptions options;
    options.runner = &runner;
    const DocumentMigrationResult r = migrate_document(doc, set, options);
    CHECK(!r.ok);
    CHECK(has_code(r, "migration.id_mutated"));
    CHECK(canon(doc) == before);
}

void test_host_budget_refuses_before_calling_the_runner()
{
    MigrationSet set;
    register_pkg_step(set);
    JsonValue doc = parse(
        R"({"componentVersions": {"phys:body": 1}, "c": {"phys:body": {"hp": 10, "a": 1, "b": 2}}})");
    MockMigrationRunner runner(
        [](std::string_view in, std::string& out) { return guest_rename("hp", "health", in, out); });
    MigrateOptions options;
    options.runner = &runner;
    options.budget.max_nodes = 2; // the payload exceeds this
    const DocumentMigrationResult r = migrate_document(doc, set, options);
    CHECK(!r.ok);
    CHECK(has_code(r, "migration.budget_exceeded"));
    CHECK(runner.run_calls == 0); // the host budget gate refused BEFORE the guest ran
}

// --- optional ctx_map_path override rewriting through the runner ----------------------------

void test_sandboxed_map_path_rewrites_override_through_runner()
{
    MigrationSet set;
    register_pkg_step(set);
    JsonValue doc = parse(R"({
      "componentVersions": {"phys:body": 1},
      "entities": [{"components": {"phys:body": {"hp": 10}}}],
      "instances": [
        {"overrides": [
          {"path": "components/phys:body/hp", "value": 5}
        ], "source": "lib/tpl.doc.json"}
      ]
    })");
    MockMigrationRunner runner(
        [](std::string_view in, std::string& out) { return guest_rename("hp", "health", in, out); },
        [](std::string_view p) -> std::optional<std::string> {
            if (p == "/hp")
                return std::string("/health");
            return std::string(p);
        });
    MigrateOptions options;
    options.runner = &runner;
    const DocumentMigrationResult r = migrate_document(doc, set, options);
    CHECK(r.ok);
    CHECK(r.changed);
    CHECK(runner.map_calls >= 1); // override rewrite consulted the guest's ctx_map_path
    const std::string bytes = canon(doc);
    CHECK(bytes.find("\"path\": \"components/phys:body/health\"") != std::string::npos);
    CHECK(bytes.find("components/phys:body/hp") == std::string::npos);
}

void test_sandboxed_map_path_orphan_is_preserved_and_flagged()
{
    MigrationSet set;
    register_pkg_step(set);
    JsonValue doc = parse(R"({
      "componentVersions": {"phys:body": 1},
      "entities": [{"components": {"phys:body": {"hp": 10}}}],
      "instances": [
        {"overrides": [
          {"path": "components/phys:body/hp", "value": 5}
        ], "source": "lib/tpl.doc.json"}
      ]
    })");
    MockMigrationRunner runner(
        [](std::string_view in, std::string& out) { return guest_rename("hp", "health", in, out); },
        [](std::string_view p) -> std::optional<std::string> {
            if (p == "/hp")
                return std::nullopt; // no destination: the override is orphaned
            return std::string(p);
        });
    MigrateOptions options;
    options.runner = &runner;
    const DocumentMigrationResult r = migrate_document(doc, set, options);
    CHECK(r.ok); // orphans are NON-blocking: the migration lands
    CHECK(has_code(r, "migration.orphan_override"));
    // The entry is preserved VERBATIM (parse-time migration never destroys authored data).
    CHECK(canon(doc).find("\"path\": \"components/phys:body/hp\"") != std::string::npos);
}

void test_sandboxed_map_path_identity_when_export_absent()
{
    MigrationSet set;
    register_pkg_step(set);
    JsonValue doc = parse(R"({
      "componentVersions": {"phys:body": 1},
      "entities": [{"components": {"phys:body": {"hp": 10}}}],
      "instances": [
        {"overrides": [
          {"path": "components/phys:body/extra", "value": 5}
        ], "source": "lib/tpl.doc.json"}
      ]
    })");
    // A module that omits ctx_map_path ⇒ IDENTITY: the override path survives unchanged, no orphan.
    MockMigrationRunner runner(
        [](std::string_view in, std::string& out) { return guest_rename("hp", "health", in, out); });
    MigrateOptions options;
    options.runner = &runner;
    const DocumentMigrationResult r = migrate_document(doc, set, options);
    CHECK(r.ok);
    CHECK(runner.map_calls >= 1); // the (identity) export was still consulted
    const std::string bytes = canon(doc);
    CHECK(bytes.find("\"path\": \"components/phys:body/extra\"") != std::string::npos);
    CHECK(!has_code(r, "migration.orphan_override"));
}

} // namespace

int main()
{
    test_sandboxed_step_runs_through_injected_runner();
    test_no_runner_keeps_honest_refusal();
    test_guest_failure_is_step_failed_and_rolls_back();
    test_runner_fuel_exhaustion_is_budget_exceeded_and_rolls_back();
    test_guest_non_json_output_is_step_failed();
    test_runner_output_id_mutation_is_rejected_host_side();
    test_host_budget_refuses_before_calling_the_runner();
    test_sandboxed_map_path_rewrites_override_through_runner();
    test_sandboxed_map_path_orphan_is_preserved_and_flagged();
    test_sandboxed_map_path_identity_when_export_absent();
    MIGRATE_TEST_MAIN_END();
}
