// Path-transform tests (L-37: migrations transform override/reference PATHS as well as payloads).
// Coverage: the transform_payload_path chain matrix, override "path" rewriting through single and
// chained steps, orphan-override preservation + non-blocking diagnostics, non-matching shapes left
// untouched, and the pointer grammar edge cases.

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

std::size_t count_code(const DocumentMigrationResult& r, std::string_view code)
{
    return static_cast<std::size_t>(
        std::count_if(r.diagnostics.begin(), r.diagnostics.end(),
                      [code](const MigrationDiagnostic& d) { return d.code == code; }));
}

void test_transform_payload_path_matrix()
{
    MigrationSet set;
    register_reference_steps(set);

    // Mapped through the whole chain: /tint -> /color (v1->v2), identity (v2->v3).
    CHECK(transform_payload_path(set, "test:sprite", 1, "/tint") == std::string("/color"));
    // Identity fields survive every hop.
    CHECK(transform_payload_path(set, "test:sprite", 1, "/id") == std::string("/id"));
    // Orphaned at the v2->v3 hop.
    CHECK(!transform_payload_path(set, "test:sprite", 1, "/size").has_value());
    CHECK(!transform_payload_path(set, "test:sprite", 2, "/size").has_value());
    // From the current version the chain is empty: identity.
    CHECK(transform_payload_path(set, "test:sprite", 3, "/anything") == std::string("/anything"));
    // Unresolvable chains: unregistered type, from > current, from < 1.
    CHECK(!transform_payload_path(set, "test:unknown", 1, "/a").has_value());
    CHECK(!transform_payload_path(set, "test:sprite", 4, "/a").has_value());
    CHECK(!transform_payload_path(set, "test:sprite", 0, "/a").has_value());

    // A gap in the chain is unresolvable.
    MigrationSet gap;
    std::string problem;
    CHECK(gap.register_component("test:sprite", 3, problem));
    MigrationStep only_v2;
    only_v2.component_type = "test:sprite";
    only_v2.from_version = 2;
    only_v2.transform = [](JsonValue&) { return true; };
    CHECK(gap.register_step(std::move(only_v2), problem));
    CHECK(!transform_payload_path(gap, "test:sprite", 1, "/a").has_value());
}

void test_override_rewrite_through_chain()
{
    MigrationSet set;
    register_reference_steps(set);
    JsonValue doc = parse(R"({
      "componentVersions": {"test:sprite": 1},
      "instances": [
        {"overrides": [
          {"path": "components/test:sprite/tint", "value": "green"},
          {"path": "/entities/0/components/test:sprite/tint", "value": "red"}
        ], "source": "lib/tpl.doc.json"}
      ]
    })");
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(r.ok);
    CHECK(r.changed);
    const std::string bytes = canon(doc);
    // Both the relative and the absolute-style authored prefixes are preserved verbatim.
    CHECK(bytes.find("\"path\": \"components/test:sprite/color\"") != std::string::npos);
    CHECK(bytes.find("\"path\": \"/entities/0/components/test:sprite/color\"") !=
          std::string::npos);
    CHECK(bytes.find("test:sprite/tint") == std::string::npos);
    CHECK(count_code(r, "migration.orphan_override") == 0);
}

void test_orphan_override_is_preserved_and_flagged()
{
    MigrationSet set;
    register_reference_steps(set);
    JsonValue doc = parse(R"({
      "componentVersions": {"test:sprite": 1},
      "instances": [
        {"overrides": [
          {"path": "components/test:sprite/size", "value": 8}
        ], "source": "lib/tpl.doc.json"}
      ]
    })");
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(r.ok); // orphans are NON-blocking: the migration lands
    CHECK(r.changed);
    CHECK(count_code(r, "migration.orphan_override") == 1);
    const MigrationDiagnostic* orphan = nullptr;
    for (const MigrationDiagnostic& d : r.diagnostics)
        if (d.code == "migration.orphan_override")
            orphan = &d;
    CHECK(orphan != nullptr);
    if (orphan != nullptr)
    {
        CHECK(!orphan->blocking);
        CHECK(orphan->pointer == "/instances/0/overrides/0/path");
    }
    // The entry is preserved VERBATIM (parse-time migration never destroys authored data).
    CHECK(canon(doc).find("\"path\": \"components/test:sprite/size\"") != std::string::npos);
}

void test_non_matching_shapes_are_untouched()
{
    MigrationSet set;
    register_reference_steps(set);
    JsonValue doc = parse(R"({
      "componentVersions": {"test:sprite": 1},
      "entities": [{"components": {"test:sprite": {"tint": "red"}}}],
      "instances": [
        {"overrides": [
          {"value": "no path member"},
          {"path": 7},
          {"path": "components/other:widget/tint"},
          {"path": "no-component-segment/at/all"}
        ], "source": "x"},
        {"overrides": "not an array"}
      ]
    })");
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(r.ok);
    CHECK(r.changed); // the payload migrated
    const std::string bytes = canon(doc);
    CHECK(bytes.find("\"path\": \"components/other:widget/tint\"") != std::string::npos);
    CHECK(bytes.find("\"path\": \"no-component-segment/at/all\"") != std::string::npos);
    CHECK(bytes.find("\"overrides\": \"not an array\"") != std::string::npos);
    CHECK(count_code(r, "migration.orphan_override") == 0);
}

void test_whole_payload_path_and_last_segment_rule()
{
    MigrationSet set;
    register_reference_steps(set);
    JsonValue doc = parse(R"({
      "componentVersions": {"test:sprite": 1},
      "instances": [
        {"overrides": [
          {"path": "components/test:sprite", "value": {}},
          {"path": "weird/test:sprite/mid/test:sprite/tint", "value": 1}
        ], "source": "x"}
      ]
    })");
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(r.ok);
    const std::string bytes = canon(doc);
    // A path addressing the WHOLE payload has an empty payload-relative pointer: identity.
    CHECK(bytes.find("\"path\": \"components/test:sprite\"") != std::string::npos);
    // The LAST matching segment splits the path (the tail after it is the payload pointer).
    CHECK(bytes.find("\"path\": \"weird/test:sprite/mid/test:sprite/color\"") !=
          std::string::npos);
}

void test_overrides_inside_migrated_payloads_are_opaque()
{
    // A payload's OWN "overrides"-shaped member is payload data, not an override site.
    MigrationSet set;
    register_reference_steps(set);
    JsonValue doc = parse(R"({
      "componentVersions": {"test:sprite": 1},
      "c": {"test:sprite": {"tint": "red", "overrides": [{"path": "components/test:sprite/tint"}]}}
    })");
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(r.ok);
    const std::string bytes = canon(doc);
    // The payload's tint member migrated...
    CHECK(bytes.find("\"color\": \"red\"") != std::string::npos);
    // ...but the payload-internal pseudo-override path was NOT rewritten.
    CHECK(bytes.find("\"path\": \"components/test:sprite/tint\"") != std::string::npos);
}

void test_overrides_inside_current_version_payloads_are_opaque()
{
    // Payload opacity holds for EVERY stamped/registered type, not just the migrated ones: an
    // "overrides"-shaped member inside a CURRENT-version payload (no plan for it this pass) is
    // that payload's private data — the rewriter must not reach in and rewrite its paths.
    MigrationSet set;
    std::string problem;
    register_reference_steps(set); // test:sprite current 3, migrated below
    CHECK(set.register_component("test:panel", 1, problem));

    JsonValue doc = parse(R"({
      "componentVersions": {"test:panel": 1, "test:sprite": 1},
      "c": {"test:sprite": {"tint": "red"}},
      "ui": {"test:panel": {"overrides": [{"path": "components/test:sprite/tint", "value": 1}]}}
    })");
    const DocumentMigrationResult r = migrate_document(doc, set);
    CHECK(r.ok);
    CHECK(r.changed);
    const std::string bytes = canon(doc);
    // The real sprite payload migrated...
    CHECK(bytes.find("\"color\": \"red\"") != std::string::npos);
    // ...but the panel-payload-internal pseudo-override path survived verbatim.
    CHECK(bytes.find("\"path\": \"components/test:sprite/tint\"") != std::string::npos);
    CHECK(count_code(r, "migration.orphan_override") == 0);
}

} // namespace

int main()
{
    test_transform_payload_path_matrix();
    test_override_rewrite_through_chain();
    test_orphan_override_is_preserved_and_flagged();
    test_non_matching_shapes_are_untouched();
    test_whole_payload_path_and_last_segment_rule();
    test_overrides_inside_migrated_payloads_are_opaque();
    test_overrides_inside_current_version_payloads_are_opaque();
    MIGRATE_TEST_MAIN_END();
}
