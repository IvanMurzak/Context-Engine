// R-QA-011 fixture round-trips: enumerate every committed fixture pair under fixtures/, migrate
// each input under a set whose current version is the pair's <TO> (bulk-path semantics), and
// byte-compare the canonical output against the golden — then assert the idempotence fixpoint
// (migrating a golden is a no-op). Fixture pairs are versioned deliverables, kept forever; this
// test is what makes forgetting one impossible to miss.

#include "migrate_test.h"

#include "context/editor/migrate/migrate_document.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace context::editor::migrate;
using context::editor::serializer::JsonValue;
using migratetest::canon;
using migratetest::parse;

namespace
{

bool read_file(const std::filesystem::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

// Parse "v<FROM>-to-v<TO>" into its two version numbers.
bool parse_bump_dir(const std::string& name, std::int64_t& from, std::int64_t& to)
{
    if (name.size() < 7 || name[0] != 'v')
        return false;
    const std::size_t dash = name.find("-to-v");
    if (dash == std::string::npos)
        return false;
    try
    {
        from = std::stoll(name.substr(1, dash - 1));
        to = std::stoll(name.substr(dash + 5));
    }
    catch (...)
    {
        return false;
    }
    return from >= 1 && to > from;
}

// The registered set a fixture pair runs under: the family's reference steps with the CURRENT
// version pinned to the pair's <TO> (so a v1-to-v2 pair pins THAT bump in isolation even after
// later bumps exist). New fixture families add their registration here alongside their steps.
MigrationSet set_for(const std::string& family, std::int64_t to, bool& known)
{
    MigrationSet set;
    std::string problem;
    known = false;
    if (family == "test-sprite")
    {
        known = true;
        migratetest::register_reference_steps(set); // registers current=3 + both steps
        if (!set.register_component("test:sprite", to, problem)) // re-pin current to <TO>
        {
            std::fprintf(stderr, "fixture set re-pin failed: %s\n", problem.c_str());
            ++migratetest::g_failures;
        }
    }
    return set;
}

} // namespace

int main()
{
    namespace fs = std::filesystem;
    const fs::path fixtures_root = CONTEXT_MIGRATE_FIXTURES_DIR;
    CHECK(fs::exists(fixtures_root));

    std::size_t pairs_checked = 0;
    for (const fs::directory_entry& family_entry : fs::directory_iterator(fixtures_root))
    {
        if (!family_entry.is_directory())
            continue;
        const std::string family = family_entry.path().filename().string();
        for (const fs::directory_entry& bump_entry : fs::directory_iterator(family_entry.path()))
        {
            if (!bump_entry.is_directory())
                continue;
            const std::string bump = bump_entry.path().filename().string();
            std::int64_t from = 0;
            std::int64_t to = 0;
            if (!parse_bump_dir(bump, from, to))
            {
                std::fprintf(stderr, "fixture dir %s/%s does not match v<FROM>-to-v<TO>\n",
                             family.c_str(), bump.c_str());
                ++migratetest::g_failures;
                continue;
            }

            bool known = false;
            const MigrationSet set = set_for(family, to, known);
            if (!known)
            {
                std::fprintf(stderr,
                             "fixture family \"%s\" has no registered reference set — register it "
                             "in set_for() alongside its steps (R-QA-011: the fixture is a "
                             "deliverable of the bump)\n",
                             family.c_str());
                ++migratetest::g_failures;
                continue;
            }

            std::string input_bytes;
            std::string golden_bytes;
            CHECK(read_file(bump_entry.path() / "input.json", input_bytes));
            CHECK(read_file(bump_entry.path() / "golden.json", golden_bytes));
            if (input_bytes.empty() || golden_bytes.empty())
                continue;

            MigrateOptions options;
            options.stamp_registered_sites = true; // the bulk `context migrate` semantics

            // input --migrate--> golden, byte-exactly.
            JsonValue doc = parse(input_bytes);
            const DocumentMigrationResult r = migrate_document(doc, set, options);
            CHECK(r.ok);
            CHECK(r.changed);
            const std::string migrated = canon(doc);
            if (migrated != golden_bytes)
            {
                std::fprintf(stderr,
                             "fixture %s/%s: migrated output differs from golden.\n--- migrated "
                             "---\n%s--- golden ---\n%s",
                             family.c_str(), bump.c_str(), migrated.c_str(),
                             golden_bytes.c_str());
                ++migratetest::g_failures;
            }

            // golden --migrate--> golden (idempotence fixpoint; a second bulk run is a no-op).
            JsonValue golden_doc = parse(golden_bytes);
            const DocumentMigrationResult again = migrate_document(golden_doc, set, options);
            CHECK(again.ok);
            CHECK(!again.changed);
            CHECK(canon(golden_doc) == golden_bytes);

            // The golden itself must BE canonical bytes (a hand-mangled golden fails loudly).
            JsonValue golden_reparse = parse(golden_bytes);
            CHECK(canon(golden_reparse) == golden_bytes);

            ++pairs_checked;
        }
    }

    // The reference family ships three pairs today; the corpus only ever GROWS (kept forever).
    CHECK(pairs_checked >= 3);
    MIGRATE_TEST_MAIN_END();
}
