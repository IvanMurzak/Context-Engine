// Save-migration runner (R-DATA-005 / L-37): the heavy R-QA-013 suite. The round-trip across a
// schema bump REUSES the migrate module's reference steps (test:sprite v1->v2->v3, the same chain
// the R-QA-011 fixtures under editor/migrate/fixtures/ pin) so a save loads through EXACTLY the
// editor's parse-time mechanism. Coverage: the schema-bump round-trip, composed-identity stability,
// id immutability through a save, back-compat scope, unknown component, newer-than, malformed
// (unstamped payload), a chain gap surfaced through the save path, and all-or-nothing rollback.

#include "context/runtime/save/save_document.h"
#include "context/runtime/save/save_migration.h"

#include "migrate_test.h" // the reference steps + CHECK harness + parse/canon helpers

#include <cstdint>
#include <string>
#include <utility>

using namespace context::runtime::save;
namespace migrate = context::editor::migrate;
namespace serializer = context::editor::serializer;
using serializer::JsonValue;

namespace
{

// The v1 test:sprite payload (matches editor/migrate/fixtures/test-sprite/v1-to-v3/input.json's
// sprite payload) and its v3 golden (the fixture's golden.json sprite payload).
constexpr const char* kSpriteV1 = R"({"id": "spr-1", "size": 4, "tint": "red"})";
constexpr const char* kSpriteV3Golden = R"({"color": "red", "extent": {"h": 4, "w": 4}, "id": "spr-1", "opacity": 1})";

[[nodiscard]] JsonValue one_component(const char* type, const char* payload_json)
{
    JsonValue obj;
    obj.type = JsonValue::Type::object;
    serializer::JsonMember m;
    m.key = type;
    m.value = migratetest::parse(payload_json);
    obj.members.push_back(std::move(m));
    return obj;
}

} // namespace

int main()
{
    migrate::MigrationSet set;
    migratetest::register_reference_steps(set); // test:sprite current version 3

    // --- round-trip across a schema bump + composed-identity stability + id immutability --------
    {
        SaveDocument save;
        save.component_versions = {{"test:sprite", 1}};
        SaveEntity entity;
        entity.identity = 0xabcdef0123456789ULL;
        entity.components = one_component("test:sprite", kSpriteV1);
        save.entities = {std::move(entity)};
        const std::uint64_t identity_before = save.entities[0].identity;

        const SaveMigrationResult result = migrate_save(save, set);
        CHECK(result.ok);
        CHECK(result.changed);
        CHECK(result.diagnostics.empty());

        // The header is re-stamped to the current version.
        const std::int64_t* stamped = save.saved_version("test:sprite");
        CHECK(stamped != nullptr && *stamped == 3);

        // The payload migrated to the v3 golden shape (byte-exact against the fixture's golden).
        JsonValue* payload = migratetest::member_of(save.entities[0].components, "test:sprite");
        CHECK(payload != nullptr);
        CHECK(migratetest::canon(*payload) == migratetest::canon(migratetest::parse(kSpriteV3Golden)));

        // The composed identity (the save's addressing key) is untouched by migration (L-37).
        CHECK(save.entities[0].identity == identity_before);
        // Id immutability: the payload's "id" member survived the whole chain.
        JsonValue* id = migratetest::member_of(*payload, "id");
        CHECK(id != nullptr && id->type == JsonValue::Type::string && id->string_value == "spr-1");
    }

    // --- no-op: a save already at the current version is unchanged ------------------------------
    {
        SaveDocument save;
        save.component_versions = {{"test:sprite", 3}};
        SaveEntity entity;
        entity.components = one_component("test:sprite", kSpriteV3Golden);
        save.entities = {std::move(entity)};
        const std::string before = migratetest::canon(save.entities[0].components);

        const SaveMigrationResult result = migrate_save(save, set);
        CHECK(result.ok);
        CHECK(!result.changed);
        CHECK(result.diagnostics.empty());
        CHECK(migratetest::canon(save.entities[0].components) == before);
    }

    // --- back-compat scope: too-old is REFUSED (not migrated) and rolled back -------------------
    {
        SaveDocument save;
        save.component_versions = {{"test:sprite", 1}};
        SaveEntity entity;
        entity.components = one_component("test:sprite", kSpriteV1);
        save.entities = {std::move(entity)};
        const std::string before = migratetest::canon(save.entities[0].components);

        MigrateSaveOptions options;
        options.back_compat_scope = 1; // current 3 - saved 1 = 2 > 1
        const SaveMigrationResult result = migrate_save(save, set, options);
        CHECK(!result.ok);
        CHECK(!result.changed);
        CHECK(result.diagnostics.size() == 1);
        CHECK(result.diagnostics[0].code == "save.back_compat_exceeded");
        // Rolled back untouched — header AND payload.
        CHECK(*save.saved_version("test:sprite") == 1);
        CHECK(migratetest::canon(save.entities[0].components) == before);
    }

    // --- back-compat scope: exactly within scope migrates --------------------------------------
    {
        SaveDocument save;
        save.component_versions = {{"test:sprite", 1}};
        SaveEntity entity;
        entity.components = one_component("test:sprite", kSpriteV1);
        save.entities = {std::move(entity)};

        MigrateSaveOptions options;
        options.back_compat_scope = 2; // current 3 - saved 1 = 2, allowed
        const SaveMigrationResult result = migrate_save(save, set, options);
        CHECK(result.ok);
        CHECK(result.changed);
        CHECK(*save.saved_version("test:sprite") == 3);
    }

    // --- unknown component: a payload for a type not in the compiled set is refused -------------
    {
        SaveDocument save;
        save.component_versions = {{"phys:body", 1}};
        SaveEntity entity;
        entity.components = one_component("phys:body", R"({"mass": 1})");
        save.entities = {std::move(entity)};

        const SaveMigrationResult result = migrate_save(save, set);
        CHECK(!result.ok);
        CHECK(result.diagnostics.size() == 1);
        CHECK(result.diagnostics[0].code == "save.unknown_component");
    }

    // --- newer-than: a save from a NEWER build is refused (L-37 downgrade rule) ------------------
    {
        SaveDocument save;
        save.component_versions = {{"test:sprite", 4}}; // > current 3
        SaveEntity entity;
        entity.components = one_component("test:sprite", R"({"id": "spr-1"})");
        save.entities = {std::move(entity)};

        const SaveMigrationResult result = migrate_save(save, set);
        CHECK(!result.ok);
        CHECK(result.diagnostics.size() == 1);
        // "test:" is a package namespace (not "ctx:"), so the package downgrade code applies.
        CHECK(result.diagnostics[0].code == "schema.newer_than_package");
    }

    // --- malformed: a component payload with no header stamp ------------------------------------
    {
        SaveDocument save;
        save.component_versions = {}; // nothing stamped
        SaveEntity entity;
        entity.components = one_component("test:sprite", kSpriteV1);
        save.entities = {std::move(entity)};

        const SaveMigrationResult result = migrate_save(save, set);
        CHECK(!result.ok);
        CHECK(!result.diagnostics.empty());
        CHECK(result.diagnostics[0].code == "save.malformed");
    }

    // --- a chain GAP surfaces through the save path (reuses migrate::migrate_payload findings) ---
    {
        migrate::MigrationSet gapped;
        std::string problem;
        CHECK(gapped.register_component("test:sprite", 3, problem));
        migrate::MigrationStep v1;
        v1.component_type = "test:sprite";
        v1.from_version = 1;
        v1.revision = 1;
        v1.transform = migratetest::sprite_v1_to_v2;
        CHECK(gapped.register_step(std::move(v1), problem)); // only v1->v2; v2->v3 is a GAP

        SaveDocument save;
        save.component_versions = {{"test:sprite", 1}};
        SaveEntity entity;
        entity.components = one_component("test:sprite", kSpriteV1);
        save.entities = {std::move(entity)};
        const std::string before = migratetest::canon(save.entities[0].components);

        const SaveMigrationResult result = migrate_save(save, gapped);
        CHECK(!result.ok);
        bool has_step_missing = false;
        for (const migrate::MigrationDiagnostic& d : result.diagnostics)
            if (d.code == "migration.step_missing")
                has_step_missing = true;
        CHECK(has_step_missing);
        CHECK(migratetest::canon(save.entities[0].components) == before); // rolled back
    }

    // --- all-or-nothing: a blocking type rolls back a sibling type already migrated in place -----
    {
        SaveDocument save;
        // test:sprite (migratable) is iterated FIRST, then phys:body (unknown) blocks.
        save.component_versions = {{"test:sprite", 1}, {"phys:body", 1}};
        SaveEntity entity;
        entity.components =
            migratetest::parse(R"({"phys:body": {"mass": 1}, "test:sprite": {"id": "spr-1", "size": 4, "tint": "red"}})");
        save.entities = {std::move(entity)};
        const std::string before = migratetest::canon(save.entities[0].components);

        const SaveMigrationResult result = migrate_save(save, set);
        CHECK(!result.ok);
        // The whole save is rolled back: the test:sprite payload is NOT left half-migrated.
        CHECK(migratetest::canon(save.entities[0].components) == before);
        CHECK(*save.saved_version("test:sprite") == 1);
    }

    MIGRATE_TEST_MAIN_END();
}
