// M2 exit criterion 2 — migrate a schema (ROADMAP §1 M2 Exit / issue #68): a REAL component-payload
// version bump round-trips through BOTH migration paths (L-37):
//   parse-time — migrate_document() reads an old-stamped payload and migrates it IN MEMORY without
//                touching disk (the derivation read path: the engine reads old versions transparently);
//   bulk       — the explicit `context migrate` verb rewrites the file on disk, re-stamps
//                componentVersions, and is idempotent (a second run is a no-op).
// The bump is the classic field rename (test:sprite v1 "tint" -> v2 "color"), registered as an
// injected set exactly as production engine bumps register in MigrationSet::engine_set(). The bulk
// path also refuses a newer-than-engine stamp (the L-37 downgrade rule) rather than best-effort
// parsing it.
//
// R-QA-013: happy path (parse-time + bulk migrate + re-stamp) + edge (idempotence fixpoint, dry-run
// suppression) + failure path (a newer-than-current stamp is blocked, file untouched).

#include "context/cli/migrate_command.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"
#include "context/editor/migrate/migrate_document.h"
#include "context/editor/migrate/migration_set.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_tree.h"

#include "m2_exit_test.h"

#include <cstdint>
#include <map>
#include <string>

using context::cli::run_migrate_with;
using context::editor::contract::Envelope;
using context::editor::contract::Json;
namespace migrate = context::editor::migrate;
namespace serializer = context::editor::serializer;
namespace fs = std::filesystem;

namespace
{

// The injected real bump: test:sprite v1 -> v2 renames the "tint" member to "color"; phys:body is a
// second registered type pinned current=1 so a v9 stamp is "newer than package".
migrate::MigrationSet make_set()
{
    migrate::MigrationSet set;
    std::string problem;
    bool ok = set.register_component("test:sprite", 2, problem);
    ok = set.register_component("phys:body", 1, problem) && ok;
    migrate::MigrationStep rename;
    rename.component_type = "test:sprite";
    rename.from_version = 1;
    rename.transform = [](serializer::JsonValue& p) {
        for (serializer::JsonMember& m : p.members)
            if (m.key == "tint")
                m.key = "color";
        return true;
    };
    ok = set.register_step(std::move(rename), problem) && ok;
    CHECK(ok);
    return set;
}

const serializer::JsonValue* member(const serializer::JsonValue& obj, const std::string& key)
{
    for (const serializer::JsonMember& m : obj.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

std::uint64_t count_of(const Envelope& e, const std::string& key)
{
    return static_cast<std::uint64_t>(e.data().at(key).as_int());
}

// A v1-stamped test:sprite payload document (the migrate command's on-disk shape).
const char* kV1Doc = "{\n  \"componentVersions\": {\n    \"test:sprite\": 1\n  },\n  \"c\": {\n"
                     "    \"test:sprite\": {\n      \"tint\": \"red\"\n    }\n  }\n}\n";

} // namespace

int main()
{
    // --- parse-time migration: the old payload migrates IN MEMORY, disk untouched -----------------
    {
        const migrate::MigrationSet set = make_set();
        serializer::CanonicalizeResult parsed = serializer::canonicalize(kV1Doc);
        CHECK(parsed.is_json);
        serializer::JsonValue doc = parsed.root;

        const migrate::DocumentMigrationResult r = migrate::migrate_document(doc, set);
        CHECK(r.ok);
        CHECK(r.changed);

        // The in-memory payload now carries "color" (migrated) and no "tint", and the header is
        // re-stamped to v2 — all without any file IO.
        const serializer::JsonValue* c = member(doc, "c");
        CHECK(c != nullptr);
        const serializer::JsonValue* sprite = c != nullptr ? member(*c, "test:sprite") : nullptr;
        CHECK(sprite != nullptr);
        if (sprite != nullptr)
        {
            CHECK(member(*sprite, "color") != nullptr);
            CHECK(member(*sprite, "tint") == nullptr);
        }
        const serializer::JsonValue* versions = member(doc, "componentVersions");
        CHECK(versions != nullptr);
        const serializer::JsonValue* stamp =
            versions != nullptr ? member(*versions, "test:sprite") : nullptr;
        CHECK(stamp != nullptr && stamp->int_value == 2);
    }

    // --- bulk migration: `context migrate` rewrites + re-stamps the file, then is idempotent -------
    {
        const fs::path project = m2exit::make_temp_project("bulk");
        const migrate::MigrationSet set = make_set();
        m2exit::write_file_raw(project / "scenes" / "old.json", kV1Doc);

        const Envelope r =
            run_migrate_with(std::string(), {{"project", project.string()}}, set);
        CHECK(r.ok());
        CHECK(count_of(r, "scanned") == 1);
        CHECK(count_of(r, "migrated") == 1);
        CHECK(count_of(r, "failed") == 0);

        const std::string migrated = m2exit::read_file(project / "scenes" / "old.json");
        CHECK(migrated.find("\"test:sprite\": 2") != std::string::npos); // re-stamped on disk
        CHECK(migrated.find("\"color\": \"red\"") != std::string::npos); // payload migrated
        CHECK(migrated.find("\"tint\"") == std::string::npos);

        // Idempotence fixpoint: a second bulk run changes nothing (round-trip is stable).
        const Envelope again =
            run_migrate_with(std::string(), {{"project", project.string()}}, set);
        CHECK(again.ok());
        CHECK(count_of(again, "migrated") == 0);
        CHECK(count_of(again, "unchanged") == 1);
        CHECK(m2exit::read_file(project / "scenes" / "old.json") == migrated);

        m2exit::remove_quiet(project);
    }

    // --- dry-run: the migration is reported but NOT written ---------------------------------------
    {
        const fs::path project = m2exit::make_temp_project("dry");
        const migrate::MigrationSet set = make_set();
        m2exit::write_file_raw(project / "old.json", kV1Doc);
        const Envelope r = run_migrate_with(
            std::string(), {{"project", project.string()}, {"dry-run", "true"}}, set);
        CHECK(r.ok());
        CHECK(r.data().at("dryRun").as_bool());
        CHECK(count_of(r, "migrated") == 1);                                 // reported...
        CHECK(m2exit::read_file(project / "old.json") == std::string(kV1Doc)); // ...but not written
        m2exit::remove_quiet(project);
    }

    // --- failure path: a newer-than-current stamp is BLOCKED, the file left byte-for-byte untouched
    {
        const fs::path project = m2exit::make_temp_project("newer");
        const migrate::MigrationSet set = make_set();
        const std::string newer =
            "{\n  \"componentVersions\": {\n    \"phys:body\": 9\n  },\n  \"c\": {\n"
            "    \"phys:body\": {\n      \"m\": 1\n    }\n  }\n}\n";
        m2exit::write_file_raw(project / "newer.json", newer);
        const Envelope r =
            run_migrate_with(std::string(), {{"project", project.string()}}, set);
        CHECK(r.ok()); // the BULK op succeeds; the per-file outcome is the report
        CHECK(count_of(r, "failed") == 1);
        CHECK(m2exit::read_file(project / "newer.json") == newer);
        const Json& files = r.data().at("files");
        CHECK(files.size() == 1);
        CHECK(files.at(std::size_t{0}).at("action").as_string() == "failed");
        CHECK(files.at(std::size_t{0}).at("diagnostics").at(std::size_t{0}).at("code").as_string() ==
              "schema.newer_than_package");
        m2exit::remove_quiet(project);
    }

    M2_EXIT_MAIN_END();
}
