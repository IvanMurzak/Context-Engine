// migrate_document tests — the L-37 parse-time engine. Coverage: the per-payload selection matrix
// (unregistered / current / older / newer / gap), single- and multi-step chains over multi-site
// documents, re-stamping + stamp_registered_sites, the execution contract (tier gating, budget,
// id immutability), all-or-nothing rollback integrity, idempotence, and header-shape no-ops.

#include "migrate_test.h"

#include "context/editor/migrate/migrate_document.h"

#include <algorithm>
#include <string>

using namespace context::editor::migrate;
using context::editor::serializer::JsonValue;
using migratetest::canon;
using migratetest::parse;
using migratetest::register_reference_steps;

namespace
{

bool has_code(const DocumentMigrationResult& r, std::string_view code)
{
    return std::any_of(r.diagnostics.begin(), r.diagnostics.end(),
                       [code](const MigrationDiagnostic& d) { return d.code == code; });
}

// --- selection matrix ----------------------------------------------------------------------

void test_no_stamps_is_a_noop()
{
    MigrationSet set;
    register_reference_steps(set);
    JsonValue doc = parse(R"({"entities": [{"components": {"test:sprite": {"tint": "red"}}}]})");
    const std::string before = canon(doc);
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(r.ok);
    CHECK(!r.changed);
    CHECK(r.diagnostics.empty());
    CHECK(canon(doc) == before); // unstamped payloads are untouched at parse time
}

void test_unregistered_type_is_untouched()
{
    MigrationSet set;
    register_reference_steps(set);
    JsonValue doc = parse(
        R"({"componentVersions": {"other:widget": 1}, "x": {"other:widget": {"a": 1}}})");
    const std::string before = canon(doc);
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(r.ok);
    CHECK(!r.changed);
    CHECK(canon(doc) == before);
}

void test_current_version_is_a_noop()
{
    MigrationSet set;
    register_reference_steps(set);
    JsonValue doc = parse(
        R"({"componentVersions": {"test:sprite": 3}, "c": {"test:sprite": {"color": "red"}}})");
    const std::string before = canon(doc);
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(r.ok);
    CHECK(!r.changed);
    CHECK(canon(doc) == before);
}

void test_newer_than_engine_and_package()
{
    MigrationSet set;
    std::string problem;
    CHECK(set.register_component("ctx:transform", 2, problem));
    CHECK(set.register_component("phys:body", 2, problem));

    JsonValue engine_doc = parse(
        R"({"componentVersions": {"ctx:transform": 5}, "c": {"ctx:transform": {"p": 1}}})");
    const std::string engine_before = canon(engine_doc);
    const DocumentMigrationResult engine_r = migrate_document(engine_doc, set);
    CHECK(!engine_r.ok);
    CHECK(has_code(engine_r, "schema.newer_than_engine"));
    CHECK(!has_code(engine_r, "schema.newer_than_package"));
    CHECK(canon(engine_doc) == engine_before); // last-good: the tree is untouched

    JsonValue pkg_doc = parse(
        R"({"componentVersions": {"phys:body": 9}, "c": {"phys:body": {"m": 1}}})");
    const DocumentMigrationResult pkg_r = migrate_document(pkg_doc, set);
    CHECK(!pkg_r.ok);
    CHECK(has_code(pkg_r, "schema.newer_than_package"));
    const MigrationDiagnostic& d = pkg_r.diagnostics.front();
    CHECK(d.blocking);
    CHECK(d.pointer == "/componentVersions/phys:body");
}

void test_chain_gap_is_blocking()
{
    MigrationSet set;
    std::string problem;
    CHECK(set.register_component("test:sprite", 3, problem));
    // Only v2->v3 exists: a v1-stamped document has a gap at v1.
    MigrationStep s;
    s.component_type = "test:sprite";
    s.from_version = 2;
    s.transform = [](JsonValue&) { return true; };
    CHECK(set.register_step(std::move(s), problem));

    JsonValue doc = parse(
        R"({"componentVersions": {"test:sprite": 1}, "c": {"test:sprite": {"tint": "red"}}})");
    const std::string before = canon(doc);
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(!r.ok);
    CHECK(has_code(r, "migration.step_missing"));
    CHECK(canon(doc) == before);
}

// --- chains, sites, stamping -----------------------------------------------------------------

void test_single_step_chain_and_stamp()
{
    MigrationSet set;
    register_reference_steps(set);
    JsonValue doc = parse(
        R"({"componentVersions": {"test:sprite": 2}, "c": {"test:sprite": {"color": "blue", "size": 2}}})");
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(r.ok);
    CHECK(r.changed);
    const std::string bytes = canon(doc);
    CHECK(bytes.find("\"test:sprite\": 3") != std::string::npos); // re-stamped
    CHECK(bytes.find("\"extent\"") != std::string::npos);         // v2->v3 applied
    CHECK(bytes.find("\"opacity\": 1") != std::string::npos);
    CHECK(bytes.find("\"size\"") == std::string::npos);
}

void test_multi_step_chain_over_multiple_sites()
{
    MigrationSet set;
    register_reference_steps(set);
    JsonValue doc = parse(R"({
      "componentVersions": {"test:sprite": 1},
      "entities": [
        {"components": {"test:sprite": {"id": "spr-1", "size": 4, "tint": "red"}}},
        {"components": {"test:sprite": {"id": "spr-2", "size": 2, "tint": "blue"}}}
      ]
    })");
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(r.ok);
    CHECK(r.changed);
    const std::string bytes = canon(doc);
    CHECK(bytes.find("\"test:sprite\": 3") != std::string::npos);
    CHECK(bytes.find("\"tint\"") == std::string::npos); // both sites migrated
    CHECK(bytes.find("\"color\": \"red\"") != std::string::npos);
    CHECK(bytes.find("\"color\": \"blue\"") != std::string::npos);
    // Ids survived verbatim (L-37 identity law).
    CHECK(bytes.find("\"id\": \"spr-1\"") != std::string::npos);
    CHECK(bytes.find("\"id\": \"spr-2\"") != std::string::npos);
}

void test_stamp_only_migration_with_no_sites()
{
    // A stamped type whose payloads were all deleted still re-stamps (the header is data too).
    MigrationSet set;
    register_reference_steps(set);
    JsonValue doc = parse(R"({"componentVersions": {"test:sprite": 1}, "entities": []})");
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(r.ok);
    CHECK(r.changed);
    CHECK(canon(doc).find("\"test:sprite\": 3") != std::string::npos);
}

void test_stamp_registered_sites_tool_save_rule()
{
    MigrationSet set;
    register_reference_steps(set);

    // An unstamped payload of a REGISTERED type: parse-time leaves it alone...
    JsonValue parse_time = parse(R"({"c": {"test:sprite": {"color": "red"}}})");
    const std::string before = canon(parse_time);
    const DocumentMigrationResult r0 = migrate_document(parse_time, set);
    CHECK(r0.ok);
    CHECK(!r0.changed);
    CHECK(canon(parse_time) == before);

    // ...while the tool-save/bulk path stamps it at current, creating componentVersions.
    JsonValue tool_save = parse(R"({"c": {"test:sprite": {"color": "red"}}})");
    MigrateOptions options;
    options.stamp_registered_sites = true;
    const DocumentMigrationResult r1 = migrate_document(tool_save, set, options);
    CHECK(r1.ok);
    CHECK(r1.changed);
    CHECK(canon(tool_save).find("\"componentVersions\": {\n    \"test:sprite\": 3\n  }") !=
          std::string::npos);

    // Unregistered types never get stamped, even on the tool-save path.
    JsonValue foreign = parse(R"({"c": {"other:widget": {"a": 1}}})");
    const std::string foreign_before = canon(foreign);
    const DocumentMigrationResult r2 = migrate_document(foreign, set, options);
    CHECK(r2.ok);
    CHECK(!r2.changed);
    CHECK(canon(foreign) == foreign_before);
}

void test_component_versions_header_is_not_a_payload_site()
{
    // The header object's own member ("test:sprite": 1) must not be treated as a payload — it is
    // an integer, but even an OBJECT-valued entry under componentVersions is header, not payload.
    MigrationSet set;
    register_reference_steps(set);
    JsonValue doc = parse(R"({"componentVersions": {"test:sprite": 3}})");
    const std::string before = canon(doc);
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(r.ok);
    CHECK(!r.changed);
    CHECK(canon(doc) == before);
}

void test_malformed_component_versions_is_a_noop_for_migration()
{
    // Header-shape findings are the VALIDATOR's (header.* codes); migration must not half-apply
    // against a header it cannot trust.
    MigrationSet set;
    register_reference_steps(set);
    JsonValue not_object = parse(R"({"componentVersions": 7, "c": {"test:sprite": {"tint": "x"}}})");
    const std::string before = canon(not_object);
    const DocumentMigrationResult r0 = migrate_document(not_object, set);
    CHECK(r0.ok);
    CHECK(!r0.changed);
    CHECK(canon(not_object) == before);

    JsonValue bad_entry = parse(
        R"({"componentVersions": {"test:sprite": "one"}, "c": {"test:sprite": {"tint": "x"}}})");
    const std::string bad_before = canon(bad_entry);
    const DocumentMigrationResult r1 = migrate_document(bad_entry, set);
    CHECK(r1.ok);
    CHECK(!r1.changed);
    CHECK(canon(bad_entry) == bad_before);
}

void test_cross_type_payload_interiors_are_opaque()
{
    // Payload interiors are opaque to site discovery for EVERY type, not just the payload's own:
    // a "test:a"-keyed object INSIDE test:b's payload is b-payload-internal data (a namespaced key
    // collision), never a test:a site — migrating it would corrupt another payload's private data.
    MigrationSet set;
    std::string problem;
    bool ok = set.register_component("test:a", 2, problem);
    ok = set.register_component("test:b", 2, problem) && ok;
    MigrationStep a_step;
    a_step.component_type = "test:a";
    a_step.from_version = 1;
    a_step.transform = [](JsonValue& p) {
        for (auto& m : p.members)
            if (m.key == "x")
                m.key = "y";
        return true;
    };
    ok = set.register_step(std::move(a_step), problem) && ok;
    MigrationStep b_step;
    b_step.component_type = "test:b";
    b_step.from_version = 1;
    b_step.transform = [](JsonValue& p) {
        for (auto& m : p.members)
            if (m.key == "m")
                m.key = "n";
        return true;
    };
    ok = set.register_step(std::move(b_step), problem) && ok;
    CHECK(ok);

    JsonValue doc = parse(R"({
      "componentVersions": {"test:a": 1, "test:b": 1},
      "c": {"test:a": {"x": 2}, "test:b": {"m": 1, "test:a": {"x": 1}}}
    })");
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(r.ok);
    CHECK(r.changed);
    const std::string bytes = canon(doc);
    CHECK(bytes.find("\"y\": 2") != std::string::npos);  // the real test:a site migrated
    CHECK(bytes.find("\"n\": 1") != std::string::npos);  // the real test:b site migrated
    CHECK(bytes.find("\"x\": 1") != std::string::npos);  // b-payload-internal "test:a": untouched
    CHECK(bytes.find("\"x\": 2") == std::string::npos);
}

void test_unstamped_scan_respects_payload_opacity()
{
    // The stamp_registered_sites scan must not descend into a stamped payload and header-stamp a
    // registered type whose only "site" is that payload's internal data.
    MigrationSet set;
    std::string problem;
    register_reference_steps(set); // test:sprite current 3
    CHECK(set.register_component("test:widget", 1, problem));

    JsonValue doc = parse(R"({
      "componentVersions": {"test:sprite": 3},
      "c": {"test:sprite": {"color": "red", "test:widget": {"w": 1}}}
    })");
    const std::string before = canon(doc);
    MigrateOptions options;
    options.stamp_registered_sites = true;
    const DocumentMigrationResult r = migrate_document(doc, set, options);
    CHECK(r.ok);
    CHECK(!r.changed); // nothing to migrate, nothing to stamp: the widget object is sprite-internal
    CHECK(canon(doc) == before);
    CHECK(canon(doc).find("\"test:widget\": 1") == std::string::npos);
}

// --- the execution contract ------------------------------------------------------------------

void test_sandboxed_tier_is_refused_in_process()
{
    MigrationSet set;
    std::string problem;
    CHECK(set.register_component("phys:body", 2, problem));
    MigrationStep s;
    s.component_type = "phys:body";
    s.from_version = 1;
    s.tier = MigrationTier::package_sandboxed;
    s.wasm_module = "phys/migrations.wasm";
    CHECK(set.register_step(std::move(s), problem));

    JsonValue doc = parse(
        R"({"componentVersions": {"phys:body": 1}, "c": {"phys:body": {"m": 1}}})");
    const std::string before = canon(doc);
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(!r.ok);
    CHECK(has_code(r, "migration.runner_unavailable"));
    CHECK(canon(doc) == before); // never silently run unsandboxed, never half-applied
}

void test_failing_step_rolls_back_whole_document()
{
    MigrationSet set;
    std::string problem;
    CHECK(set.register_component("test:sprite", 3, problem));
    MigrationStep ok_step;
    ok_step.component_type = "test:sprite";
    ok_step.from_version = 1;
    ok_step.transform = [](JsonValue& p) {
        for (auto& m : p.members)
            if (m.key == "tint")
                m.key = "color";
        return true;
    };
    CHECK(set.register_step(std::move(ok_step), problem));
    MigrationStep failing;
    failing.component_type = "test:sprite";
    failing.from_version = 2;
    failing.transform = [](JsonValue&) { return false; };
    CHECK(set.register_step(std::move(failing), problem));

    JsonValue doc = parse(
        R"({"componentVersions": {"test:sprite": 1}, "c": {"test:sprite": {"tint": "red"}}})");
    const std::string before = canon(doc);
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(!r.ok);
    CHECK(!r.changed);
    CHECK(has_code(r, "migration.step_failed"));
    CHECK(canon(doc) == before); // v1->v2 succeeded first, then v2->v3 failed: ALL rolled back
}

void test_budget_exceeded()
{
    MigrationSet set;
    std::string problem;
    CHECK(set.register_component("test:big", 2, problem));
    MigrationStep grow;
    grow.component_type = "test:big";
    grow.from_version = 1;
    grow.transform = [](JsonValue& p) {
        // Explode the payload: an array of 100 integers under one member.
        JsonValue arr;
        arr.type = JsonValue::Type::array;
        for (int i = 0; i < 100; ++i)
        {
            JsonValue n;
            n.type = JsonValue::Type::integer;
            n.int_value = i;
            arr.elements.push_back(std::move(n));
        }
        context::editor::serializer::JsonMember m;
        m.key = "blob";
        m.value = std::move(arr);
        p.members.push_back(std::move(m));
        return true;
    };
    CHECK(set.register_step(std::move(grow), problem));

    JsonValue doc = parse(
        R"({"componentVersions": {"test:big": 1}, "c": {"test:big": {"a": 1}}})");
    const std::string before = canon(doc);
    MigrateOptions options;
    options.budget.max_nodes = 16; // output (~104 nodes) blows the budget
    const DocumentMigrationResult r = migrate_document(doc, set, options);
    CHECK(!r.ok);
    CHECK(has_code(r, "migration.budget_exceeded"));
    CHECK(canon(doc) == before);

    // The INPUT side is enforced too: a tiny budget refuses before the transform runs.
    JsonValue doc2 = parse(
        R"({"componentVersions": {"test:big": 1}, "c": {"test:big": {"a": 1, "b": 2, "c": 3}}})");
    MigrateOptions tiny;
    tiny.budget.max_nodes = 2;
    const DocumentMigrationResult r2 = migrate_document(doc2, set, tiny);
    CHECK(!r2.ok);
    CHECK(has_code(r2, "migration.budget_exceeded"));
}

void test_id_immutability_enforcement()
{
    std::string problem;

    // (a) mutating an id value.
    MigrationSet mutate_set;
    CHECK(mutate_set.register_component("test:sprite", 2, problem));
    MigrationStep mutate;
    mutate.component_type = "test:sprite";
    mutate.from_version = 1;
    mutate.transform = [](JsonValue& p) {
        for (auto& m : p.members)
            if (m.key == "id")
                m.value.string_value = "renamed";
        return true;
    };
    CHECK(mutate_set.register_step(std::move(mutate), problem));
    JsonValue doc_a = parse(
        R"({"componentVersions": {"test:sprite": 1}, "c": {"test:sprite": {"id": "spr-1"}}})");
    const std::string before_a = canon(doc_a);
    const DocumentMigrationResult ra = migrate_document(doc_a, mutate_set);
    CHECK(!ra.ok);
    CHECK(has_code(ra, "migration.id_mutated"));
    CHECK(canon(doc_a) == before_a);

    // (b) removing an id member.
    MigrationSet remove_set;
    CHECK(remove_set.register_component("test:sprite", 2, problem));
    MigrationStep remove;
    remove.component_type = "test:sprite";
    remove.from_version = 1;
    remove.transform = [](JsonValue& p) {
        p.members.erase(std::remove_if(p.members.begin(), p.members.end(),
                                       [](const auto& m) { return m.key == "guid"; }),
                        p.members.end());
        return true;
    };
    CHECK(remove_set.register_step(std::move(remove), problem));
    JsonValue doc_b = parse(
        R"({"componentVersions": {"test:sprite": 1}, "c": {"test:sprite": {"guid": "g-1"}}})");
    const DocumentMigrationResult rb = migrate_document(doc_b, remove_set);
    CHECK(!rb.ok);
    CHECK(has_code(rb, "migration.id_mutated"));

    // (c) MOVING an id (same value, different pointer) is a mutation of composed identity.
    MigrationSet move_set;
    CHECK(move_set.register_component("test:sprite", 2, problem));
    MigrationStep move;
    move.component_type = "test:sprite";
    move.from_version = 1;
    move.transform = [](JsonValue& p) {
        JsonValue hoisted_value;
        bool found = false;
        for (auto& m : p.members)
            if (m.key == "inner" && m.value.type == JsonValue::Type::object)
                for (auto& inner : m.value.members)
                    if (inner.key == "id")
                    {
                        hoisted_value = inner.value;
                        inner.key = "old_id";
                        found = true;
                    }
        if (found)
        {
            context::editor::serializer::JsonMember hoisted;
            hoisted.key = "id";
            hoisted.value = std::move(hoisted_value);
            p.members.push_back(std::move(hoisted)); // AFTER iteration: no invalidation
        }
        return true;
    };
    CHECK(move_set.register_step(std::move(move), problem));
    JsonValue doc_c = parse(
        R"({"componentVersions": {"test:sprite": 1}, "c": {"test:sprite": {"inner": {"id": "i-1"}}}})");
    const DocumentMigrationResult rc = migrate_document(doc_c, move_set);
    CHECK(!rc.ok);
    CHECK(has_code(rc, "migration.id_mutated"));

    // (d) ADDING an id mints identity — refused.
    MigrationSet add_set;
    CHECK(add_set.register_component("test:sprite", 2, problem));
    MigrationStep add;
    add.component_type = "test:sprite";
    add.from_version = 1;
    add.transform = [](JsonValue& p) {
        context::editor::serializer::JsonMember minted;
        minted.key = "id";
        minted.value.type = JsonValue::Type::string;
        minted.value.string_value = "minted";
        p.members.push_back(std::move(minted));
        return true;
    };
    CHECK(add_set.register_step(std::move(add), problem));
    JsonValue doc_d = parse(
        R"({"componentVersions": {"test:sprite": 1}, "c": {"test:sprite": {"a": 1}}})");
    const DocumentMigrationResult rd = migrate_document(doc_d, add_set);
    CHECK(!rd.ok);
    CHECK(has_code(rd, "migration.id_mutated"));

    // (e) renaming a NON-id field around an id is legal.
    MigrationSet legal_set;
    CHECK(legal_set.register_component("test:sprite", 2, problem));
    MigrationStep legal;
    legal.component_type = "test:sprite";
    legal.from_version = 1;
    legal.transform = [](JsonValue& p) {
        for (auto& m : p.members)
            if (m.key == "tint")
                m.key = "color";
        return true;
    };
    CHECK(legal_set.register_step(std::move(legal), problem));
    JsonValue doc_e = parse(
        R"({"componentVersions": {"test:sprite": 1}, "c": {"test:sprite": {"id": "spr", "tint": "r"}}})");
    const DocumentMigrationResult re = migrate_document(doc_e, legal_set);
    CHECK(re.ok);
    CHECK(re.changed);
}

void test_non_finite_output_is_step_failed()
{
    MigrationSet set;
    std::string problem;
    CHECK(set.register_component("test:sprite", 2, problem));
    MigrationStep bad;
    bad.component_type = "test:sprite";
    bad.from_version = 1;
    bad.transform = [](JsonValue& p) {
        JsonValue inf;
        inf.type = JsonValue::Type::number;
        inf.number_value = 1e308 * 10; // +inf
        context::editor::serializer::JsonMember m;
        m.key = "broken";
        m.value = std::move(inf);
        p.members.push_back(std::move(m));
        return true;
    };
    CHECK(set.register_step(std::move(bad), problem));
    JsonValue doc = parse(
        R"({"componentVersions": {"test:sprite": 1}, "c": {"test:sprite": {"a": 1}}})");
    const std::string before = canon(doc);
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(!r.ok);
    CHECK(has_code(r, "migration.step_failed"));
    CHECK(canon(doc) == before);
}

// --- idempotence -----------------------------------------------------------------------------

void test_idempotence()
{
    MigrationSet set;
    register_reference_steps(set);
    JsonValue doc = parse(R"({
      "componentVersions": {"test:sprite": 1},
      "entities": [{"components": {"test:sprite": {"id": "spr-1", "size": 4, "tint": "red"}}}]
    })");
    MigrateOptions options;
    options.stamp_registered_sites = true;

    const DocumentMigrationResult first = migrate_document(doc, set, options);
    CHECK(first.ok);
    CHECK(first.changed);
    const std::string once = canon(doc);

    const DocumentMigrationResult second = migrate_document(doc, set, options);
    CHECK(second.ok);
    CHECK(!second.changed);
    CHECK(second.diagnostics.empty());
    CHECK(canon(doc) == once); // migrate ∘ migrate == migrate (the bulk verb re-runs safely)
}

} // namespace

int main()
{
    test_no_stamps_is_a_noop();
    test_unregistered_type_is_untouched();
    test_current_version_is_a_noop();
    test_newer_than_engine_and_package();
    test_chain_gap_is_blocking();
    test_single_step_chain_and_stamp();
    test_multi_step_chain_over_multiple_sites();
    test_stamp_only_migration_with_no_sites();
    test_stamp_registered_sites_tool_save_rule();
    test_component_versions_header_is_not_a_payload_site();
    test_malformed_component_versions_is_a_noop_for_migration();
    test_cross_type_payload_interiors_are_opaque();
    test_unstamped_scan_respects_payload_opacity();
    test_sandboxed_tier_is_refused_in_process();
    test_failing_step_rolls_back_whole_document();
    test_budget_exceeded();
    test_id_immutability_enforcement();
    test_non_finite_output_is_step_failed();
    test_idempotence();
    MIGRATE_TEST_MAIN_END();
}
