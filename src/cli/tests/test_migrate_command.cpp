// `context migrate` tests — the L-37 explicit bulk migration path, exercised over real temp
// directories through the command API and end-to-end through the CLI verb grammar. Coverage:
// canonicalization rewrites + idempotence under the (empty) engine set, real migrations + stamping
// under an injected set, --dry-run write suppression, blocking findings leaving files untouched,
// non-JSON skips, dot-dir exclusion, file-vs-directory targets, and the missing-target failure.

#include "context/cli/app.h"
#include "context/cli/migrate_command.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"
#include "context/editor/migrate/migration_set.h"
#include "cli_test.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>

using context::cli::run_migrate;
using context::cli::run_migrate_with;
using context::editor::contract::Envelope;
using context::editor::contract::Json;
namespace migrate = context::editor::migrate;
namespace fs = std::filesystem;

namespace
{

fs::path unique_temp_dir(const std::string& tag)
{
    const auto base = fs::temp_directory_path();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = base / ("ctx-migrate-" + tag + "-" + std::to_string(stamp));
    fs::create_directories(dir);
    return dir;
}

void write_file(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// The injected test set: "test:sprite" v1 -> v2 renames "tint" -> "color"; "phys:body" current 1.
migrate::MigrationSet make_set()
{
    migrate::MigrationSet set;
    std::string problem;
    bool ok = set.register_component("test:sprite", 2, problem);
    ok = set.register_component("phys:body", 1, problem) && ok;
    migrate::MigrationStep rename;
    rename.component_type = "test:sprite";
    rename.from_version = 1;
    rename.transform = [](context::editor::serializer::JsonValue& p) {
        for (auto& m : p.members)
            if (m.key == "tint")
                m.key = "color";
        return true;
    };
    ok = set.register_step(std::move(rename), problem) && ok;
    CHECK(ok);
    return set;
}

std::map<std::string, std::string> no_flags()
{
    return {};
}

std::uint64_t count_of(const Envelope& e, const std::string& key)
{
    return static_cast<std::uint64_t>(e.data().at(key).as_int());
}

} // namespace

int main()
{
    // --- canonicalization pass under the (empty) engine set + idempotence -----------------------
    {
        const fs::path dir = unique_temp_dir("canon");
        // Non-canonical formatting: unsorted keys, no indentation, CRLF-free but compact.
        write_file(dir / "a.json", "{\"b\": 1, \"a\": 2}");
        const Envelope first = run_migrate((dir / "a.json").string(), no_flags());
        CHECK(first.ok());
        CHECK(count_of(first, "scanned") == 1);
        CHECK(count_of(first, "canonicalized") == 1);
        CHECK(count_of(first, "migrated") == 0);
        const std::string canonical = read_file(dir / "a.json");
        CHECK(canonical == "{\n  \"a\": 2,\n  \"b\": 1\n}\n"); // sorted, 2-space, one trailing LF

        // The second run is a no-op (the bulk verb re-runs safely).
        const Envelope second = run_migrate((dir / "a.json").string(), no_flags());
        CHECK(second.ok());
        CHECK(count_of(second, "unchanged") == 1);
        CHECK(count_of(second, "canonicalized") == 0);
        CHECK(read_file(dir / "a.json") == canonical);
        fs::remove_all(dir);
    }

    // --- a real migration under an injected set: payload + stamp rewritten on disk --------------
    {
        const fs::path dir = unique_temp_dir("migrate");
        const migrate::MigrationSet set = make_set();
        write_file(dir / "scenes" / "old.json",
                   "{\n  \"componentVersions\": {\n    \"test:sprite\": 1\n  },\n  \"c\": {\n"
                   "    \"test:sprite\": {\n      \"tint\": \"red\"\n    }\n  }\n}\n");
        write_file(dir / ".editor" / "hidden.json", "{\"b\": 1, \"a\": 2}"); // dot-dir: skipped
        write_file(dir / "notes.txt", "not json at all");                    // not *.json: not scanned

        const Envelope r = run_migrate_with(std::string(), {{"project", dir.string()}}, set);
        CHECK(r.ok());
        CHECK(count_of(r, "scanned") == 1); // only scenes/old.json
        CHECK(count_of(r, "migrated") == 1);
        CHECK(count_of(r, "failed") == 0);
        const std::string migrated = read_file(dir / "scenes" / "old.json");
        CHECK(migrated.find("\"test:sprite\": 2") != std::string::npos); // re-stamped
        CHECK(migrated.find("\"color\": \"red\"") != std::string::npos); // payload migrated
        CHECK(migrated.find("\"tint\"") == std::string::npos);
        // The dot-dir file was never touched.
        CHECK(read_file(dir / ".editor" / "hidden.json") == "{\"b\": 1, \"a\": 2}");
        fs::remove_all(dir);
    }

    // --- --dry-run: full report, no writes -------------------------------------------------------
    {
        const fs::path dir = unique_temp_dir("dry");
        const migrate::MigrationSet set = make_set();
        const std::string original =
            "{\n  \"componentVersions\": {\n    \"test:sprite\": 1\n  },\n  \"c\": {\n"
            "    \"test:sprite\": {\n      \"tint\": \"red\"\n    }\n  }\n}\n";
        write_file(dir / "old.json", original);
        const Envelope r =
            run_migrate_with(std::string(), {{"project", dir.string()}, {"dry-run", "true"}}, set);
        CHECK(r.ok());
        CHECK(r.data().at("dryRun").as_bool());
        CHECK(count_of(r, "migrated") == 1);         // reported...
        CHECK(read_file(dir / "old.json") == original); // ...but NOT written
        fs::remove_all(dir);
    }

    // --- blocking findings: the file is reported failed and left byte-for-byte untouched --------
    {
        const fs::path dir = unique_temp_dir("blocked");
        const migrate::MigrationSet set = make_set();
        const std::string newer =
            "{\n  \"componentVersions\": {\n    \"phys:body\": 9\n  },\n  \"c\": {\n"
            "    \"phys:body\": {\n      \"m\": 1\n    }\n  }\n}\n";
        write_file(dir / "newer.json", newer);
        const Envelope r = run_migrate_with(std::string(), {{"project", dir.string()}}, set);
        CHECK(r.ok()); // the BULK op succeeds; per-file outcomes are the report
        CHECK(count_of(r, "failed") == 1);
        CHECK(read_file(dir / "newer.json") == newer);
        const Json& files = r.data().at("files");
        CHECK(files.size() == 1);
        CHECK(files.at(0).at("action").as_string() == "failed");
        CHECK(files.at(0).at("diagnostics").size() >= 1);
        CHECK(files.at(0).at("diagnostics").at(0).at("code").as_string() ==
              "schema.newer_than_package");
        fs::remove_all(dir);
    }

    // --- non-JSON *.json content is skipped verbatim (the L-32 carve-outs) ----------------------
    {
        const fs::path dir = unique_temp_dir("nonjson");
        write_file(dir / "binary.json", std::string("not json {"));
        const Envelope r = run_migrate(dir.string(), no_flags());
        CHECK(r.ok());
        CHECK(count_of(r, "skippedNonJson") == 1);
        CHECK(read_file(dir / "binary.json") == "not json {");
        fs::remove_all(dir);
    }

    // --- a missing target fails with the catalog code --------------------------------------------
    {
        const Envelope r = run_migrate("definitely/does/not/exist-12345", no_flags());
        CHECK(!r.ok());
        CHECK(r.error().has_value());
        CHECK(r.error()->code == "file.not_found");
    }

    // --- end-to-end through the CLI verb grammar (`context migrate <path>` + core --dry-run) -----
    {
        const fs::path dir = unique_temp_dir("cli");
        write_file(dir / "a.json", "{\"b\": 1, \"a\": 2}");
        const Envelope dry = context::cli::run({"migrate", dir.string(), "--dry-run"});
        CHECK(dry.ok());
        CHECK(dry.data().at("dryRun").as_bool());
        CHECK(read_file(dir / "a.json") == "{\"b\": 1, \"a\": 2}"); // untouched
        const Envelope wet = context::cli::run({"migrate", dir.string()});
        CHECK(wet.ok());
        CHECK(read_file(dir / "a.json") == "{\n  \"a\": 2,\n  \"b\": 1\n}\n");
        fs::remove_all(dir);
    }

    CLI_TEST_MAIN_END();
}
