// End-to-end document migration through the REAL wasmtime WasmRunner (issue #71 PR3): the full
// L-37 chain — migrate_document selects a stamped-older package_sandboxed payload, routes it
// through the injected WasmRunner under the frozen guest ABI, the guest migrates real
// canonical-JSON bytes in its own linear memory, and the host re-checks every structural
// invariant and restamps. Plus the failure paths end to end: deterministic fuel exhaustion ->
// migration.budget_exceeded + all-or-nothing rollback, and the real ctx_map_path override
// rewrite (mapped + orphan). The seam-level equivalents (mock runner) live in
// src/editor/migrate/tests/test_migration_runner.cpp — THIS suite proves the real VM slots into
// the same contract.

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
        built.add("fixed.wasm", wasmtest::kWatFixedOutput);
        built.add("infinite.wasm", wasmtest::kWatInfinite);
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

// Register "phys:body" at version 2 with ONE package_sandboxed step v1->v2 running `module_ref`.
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

void test_document_migrates_through_the_real_vm()
{
    const MigrationSet set = make_set("fixed.wasm");
    JsonValue doc =
        parse(R"({"componentVersions": {"phys:body": 1}, "c": {"phys:body": {"hp": 1}}})");
    MigrateOptions options;
    options.runner = &runner();
    const DocumentMigrationResult r = migrate_document(doc, set, options);
    CHECK(r.ok);
    CHECK(r.changed);
    const std::string bytes = canon(doc);
    // The guest's migrated payload landed ({"hp":1} -> {"hp":2}) and the stamp advanced to v2.
    CHECK(bytes.find("\"hp\": 2") != std::string::npos);
    CHECK(bytes.find("\"phys:body\": 2") != std::string::npos);
}

void test_fuel_exhaustion_maps_to_budget_exceeded_and_rolls_back()
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
    CHECK(canon(doc) == before); // all-or-nothing: the document rolled back
}

void test_override_paths_rewrite_through_the_real_ctx_map_path()
{
    const MigrationSet set = make_set("map-path.wasm");
    JsonValue doc = parse(R"({
      "componentVersions": {"phys:body": 1},
      "entities": [{"components": {"phys:body": {"old": 10}}}],
      "instances": [
        {"overrides": [
          {"path": "components/phys:body/old", "value": 5},
          {"path": "components/phys:body/gone", "value": 6}
        ], "source": "lib/tpl.doc.json"}
      ]
    })");
    MigrateOptions options;
    options.runner = &runner();
    const DocumentMigrationResult r = migrate_document(doc, set, options);
    CHECK(r.ok); // orphans are NON-blocking
    CHECK(r.changed);
    const std::string bytes = canon(doc);
    // /old mapped to /new through the guest's ctx_map_path...
    CHECK(bytes.find("\"path\": \"components/phys:body/new\"") != std::string::npos);
    // ...and /gone (guest returns 1 = unmapped) is PRESERVED verbatim + flagged as an orphan.
    CHECK(bytes.find("\"path\": \"components/phys:body/gone\"") != std::string::npos);
    CHECK(has_code(r, "migration.orphan_override"));
}

} // namespace

int main()
{
    test_document_migrates_through_the_real_vm();
    test_fuel_exhaustion_maps_to_budget_exceeded_and_rolls_back();
    test_override_paths_rewrite_through_the_real_ctx_map_path();
    WASM_TEST_MAIN_END();
}
