// Fail-closed document migration through the REAL wasmtime WasmRunner (issue #71 PR4): every guest
// FAILURE MODE routes through migrate_document under the L-37 host contract and leaves the document
// UNCHANGED (all-or-nothing per document). The four modes the DoD enumerates:
//   1. fuel exhaustion       -> migration.budget_exceeded + rollback
//   2. OOB / wasm trap       -> migration.step_failed    + rollback
//   3. guest mutates an id   -> migration.id_mutated     + rollback
//   4. output over budget    -> migration.budget_exceeded + rollback (post-step node-count refusal)
// Each case asserts the migrated tree is byte-identical to its pre-call canonical form: a sandboxed
// guest can never buy itself out of a structural invariant, and a rejected step never half-applies.
// The seam-level (mock-runner) equivalents live in src/editor/migrate/tests; THIS suite proves the
// real VM trips the SAME host gate. (fuel exhaustion is also covered end to end in
// test_wasm_migrate_document.cpp; it is re-asserted here so this file is the complete enumerated
// fail-closed gate.)

#include "wasm_test.h"

#include "context/editor/migrate/migrate_document.h"
#include "context/editor/migrate/migration_set.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"
#include "context/runtime/wasm/wasm_runner.h"

#include <algorithm>
#include <string>
#include <string_view>

using namespace context::editor::migrate;
using context::editor::serializer::JsonValue;
using context::runtime::wasm::WasmRunner;
using wasmtest::ModuleTable;

namespace
{

JsonValue parse(std::string_view text)
{
    context::editor::serializer::ParseResult r = context::editor::serializer::parse_json(text);
    CHECK(r.ok);
    return std::move(r.root);
}

std::string canon(const JsonValue& v)
{
    std::string out;
    CHECK(context::editor::serializer::serialize_canonical(v, out));
    return out;
}

bool has_code(const DocumentMigrationResult& r, std::string_view code)
{
    return std::any_of(r.diagnostics.begin(), r.diagnostics.end(),
                       [code](const MigrationDiagnostic& d) { return d.code == code; });
}

ModuleTable& table()
{
    static ModuleTable t = [] {
        ModuleTable built;
        built.add("infinite.wasm", wasmtest::kWatInfinite);   // endless loop -> fuel exhaustion
        built.add("trap.wasm", wasmtest::kWatTrap);           // unreachable -> non-fuel wasm trap
        built.add("mutates-id.wasm", wasmtest::kWatMutatesId); // changes the payload's id
        built.add("big-output.wasm", wasmtest::kWatBigOutput); // output over the node budget
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

// Register "phys:body" at v2 with ONE package_sandboxed step v1->v2 running `module_ref`.
MigrationSet make_set(const char* module_ref)
{
    MigrationSet set;
    std::string problem;
    CHECK(set.register_component("phys:body", 2, problem));
    MigrationStep s;
    s.component_type = "phys:body";
    s.from_version = 1;
    s.tier = MigrationTier::package_sandboxed;
    s.wasm_module = module_ref;
    CHECK(set.register_step(std::move(s), problem));
    return set;
}

// --- 1. fuel exhaustion -> budget_exceeded + rollback ---------------------------------------------
void test_fuel_exhaustion_rolls_back()
{
    const MigrationSet set = make_set("infinite.wasm");
    JsonValue doc =
        parse(R"({"componentVersions": {"phys:body": 1}, "c": {"phys:body": {"hp": 1}}})");
    const std::string before = canon(doc);
    MigrateOptions options;
    options.runner = &runner();
    options.budget.max_nodes = 64; // small deterministic fuel grant -> the endless guest trips it
    const DocumentMigrationResult r = migrate_document(doc, set, options);
    CHECK(!r.ok);
    CHECK(!r.changed);
    CHECK(has_code(r, "migration.budget_exceeded"));
    CHECK(!has_code(r, "migration.step_failed"));
    CHECK(canon(doc) == before); // all-or-nothing: unchanged
}

// --- 2. OOB / wasm trap -> step_failed + rollback -------------------------------------------------
void test_wasm_trap_rolls_back()
{
    const MigrationSet set = make_set("trap.wasm");
    JsonValue doc =
        parse(R"({"componentVersions": {"phys:body": 1}, "c": {"phys:body": {"hp": 1}}})");
    const std::string before = canon(doc);
    MigrateOptions options;
    options.runner = &runner();
    const DocumentMigrationResult r = migrate_document(doc, set, options);
    CHECK(!r.ok);
    CHECK(!r.changed);
    CHECK(has_code(r, "migration.step_failed"));
    CHECK(!has_code(r, "migration.budget_exceeded")); // a trap is NOT a budget failure
    CHECK(!has_code(r, "migration.id_mutated"));
    CHECK(canon(doc) == before);
}

// --- 3. guest mutates an id -> id_mutated + rollback ----------------------------------------------
void test_id_mutation_rolls_back()
{
    const MigrationSet set = make_set("mutates-id.wasm");
    // The payload carries an id the migration must never touch; the guest emits a payload with a
    // DIFFERENT id -> the host id-multiset check refuses it.
    JsonValue doc = parse(
        R"({"componentVersions": {"phys:body": 1}, "c": {"phys:body": {"id": "aaaaaaaa", "hp": 1}}})");
    const std::string before = canon(doc);
    MigrateOptions options;
    options.runner = &runner();
    const DocumentMigrationResult r = migrate_document(doc, set, options);
    CHECK(!r.ok);
    CHECK(!r.changed);
    CHECK(has_code(r, "migration.id_mutated"));
    CHECK(canon(doc) == before); // the original id (and the whole doc) survives verbatim
    CHECK(canon(doc).find("aaaaaaaa") != std::string::npos);
    CHECK(canon(doc).find("beefbeef") == std::string::npos); // the guest's mutated id never lands
}

// --- 4. output over the node budget -> budget_exceeded + rollback ---------------------------------
void test_output_budget_breach_rolls_back()
{
    const MigrationSet set = make_set("big-output.wasm");
    JsonValue doc =
        parse(R"({"componentVersions": {"phys:body": 1}, "c": {"phys:body": {"hp": 1}}})");
    const std::string before = canon(doc);
    MigrateOptions options;
    options.runner = &runner();
    // Input payload {"hp":1} is 3 nodes (<= 6, so the input-budget gate + the K*6 fuel grant both
    // pass); the guest's 11-node output breaches the SAME budget on the post-step check.
    options.budget.max_nodes = 6;
    const DocumentMigrationResult r = migrate_document(doc, set, options);
    CHECK(!r.ok);
    CHECK(!r.changed);
    CHECK(has_code(r, "migration.budget_exceeded"));
    CHECK(!has_code(r, "migration.step_failed"));
    CHECK(canon(doc) == before);
    CHECK(canon(doc).find("\"hp\": 1") != std::string::npos); // still v1, untouched
}

} // namespace

int main()
{
    test_fuel_exhaustion_rolls_back();
    test_wasm_trap_rolls_back();
    test_id_mutation_rolls_back();
    test_output_budget_breach_rolls_back();
    WASM_TEST_MAIN_END();
}
