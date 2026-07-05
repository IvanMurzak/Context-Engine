// M2 exit criterion 5 — a per-payload migration round-trips against its R-QA-011 fixtures (ROADMAP §1
// M2 Exit / issue #68; L-37 / R-DATA-004 / R-QA-011). Two legs, both over the COMMITTED fixture corpus
// (src/editor/migrate/fixtures/, the versioned deliverables kept forever):
//   corpus round-trip — enumerate every committed fixture pair, migrate the input under the family's
//                       reference set pinned to the pair's <TO> version, and byte-compare the canonical
//                       output against the golden, then assert the idempotence fixpoint (a golden
//                       re-migrates to itself). Forgetting a fixture is impossible to miss.
//   per-payload primitive — the shared migrate_payload() primitive the RuntimeKernel save-migration
//                       runner (R-DATA-005) reuses migrates ONE extracted component payload from the
//                       fixture and reproduces the golden payload byte-for-byte, proving a shipped build
//                       loads old data through EXACTLY the editor's parse-time mechanism.
//
// This gate reuses the migrate module's reference steps (migrate_test.h) so it can never drift from the
// canonical bump the fixtures pin. R-QA-013: happy path (both legs) + edge (idempotence) + failure-path
// control (an unregistered fixture family fails loudly).

#include "context/editor/migrate/migrate_document.h"
#include "context/editor/migrate/migration_set.h"

#include "migrate_test.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace context::editor::migrate;
using context::editor::serializer::JsonValue;
using migratetest::canon;
using migratetest::member_of;
using migratetest::parse;

#ifndef CONTEXT_M2_FIXTURES_DIR
#error "CONTEXT_M2_FIXTURES_DIR (the migrate fixtures corpus path) must be defined by the build."
#endif

namespace
{

namespace fs = std::filesystem;

bool read_file(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

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

// The registered reference set for a family, with the CURRENT version pinned to `to` (so a v1-to-v2
// pair pins THAT bump in isolation even though the reference chain reaches v3). Mirrors the migrate
// module's set_for(), by design — the exit gate rides the same canonical steps.
MigrationSet set_for(const std::string& family, std::int64_t to, bool& known)
{
    MigrationSet set;
    std::string problem;
    known = false;
    if (family == "test-sprite")
    {
        known = true;
        migratetest::register_reference_steps(set);
        if (!set.register_component("test:sprite", to, problem))
        {
            std::fprintf(stderr, "fixture set re-pin failed: %s\n", problem.c_str());
            ++migratetest::g_failures;
        }
    }
    return set;
}

// The first test:sprite component payload of a fixture document (entities[0].components.test:sprite),
// as a detached copy — the per-payload primitive operates on one payload in isolation.
bool extract_first_payload(const std::string& doc_bytes, JsonValue& out)
{
    JsonValue doc = parse(doc_bytes);
    JsonValue* entities = member_of(doc, "entities");
    if (entities == nullptr || entities->elements.empty())
        return false;
    JsonValue* components = member_of(entities->elements[0], "components");
    if (components == nullptr)
        return false;
    JsonValue* sprite = member_of(*components, "test:sprite");
    if (sprite == nullptr)
        return false;
    out = *sprite;
    return true;
}

} // namespace

int main()
{
    const fs::path fixtures_root = CONTEXT_M2_FIXTURES_DIR;
    CHECK(fs::exists(fixtures_root));

    std::size_t pairs_checked = 0;
    std::size_t payload_legs = 0;

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
            CHECK(known); // failure-path control: an unregistered family is a loud failure
            if (!known)
                continue;

            std::string input_bytes;
            std::string golden_bytes;
            CHECK(read_file(bump_entry.path() / "input.json", input_bytes));
            CHECK(read_file(bump_entry.path() / "golden.json", golden_bytes));
            if (input_bytes.empty() || golden_bytes.empty())
                continue;

            MigrateOptions options;
            options.stamp_registered_sites = true; // the bulk `context migrate` semantics

            // --- corpus round-trip: input --migrate--> golden, byte-exactly -----------------------
            JsonValue doc = parse(input_bytes);
            const DocumentMigrationResult r = migrate_document(doc, set, options);
            CHECK(r.ok);
            CHECK(r.changed);
            CHECK(canon(doc) == golden_bytes);

            // idempotence fixpoint: a golden re-migrates to itself.
            JsonValue golden_doc = parse(golden_bytes);
            const DocumentMigrationResult again = migrate_document(golden_doc, set, options);
            CHECK(again.ok);
            CHECK(!again.changed);
            CHECK(canon(golden_doc) == golden_bytes);

            // --- per-payload primitive: migrate ONE payload, reproduce the golden payload ---------
            // Only for families whose golden keeps the payload at entities[0] (the reference family).
            JsonValue input_payload;
            JsonValue golden_payload;
            if (extract_first_payload(input_bytes, input_payload) &&
                extract_first_payload(golden_bytes, golden_payload))
            {
                std::vector<MigrationDiagnostic> diags;
                const bool ok = migrate_payload(set, "test:sprite", from, input_payload,
                                                MigrationBudget{}, "/entities/0/components/test:sprite",
                                                diags);
                CHECK(ok);
                CHECK(diags.empty());
                // The migrated payload equals the golden's payload byte-for-byte — the primitive the
                // save-migration runner reuses reproduces the same result as document migration.
                CHECK(canon(input_payload) == canon(golden_payload));
                ++payload_legs;
            }

            ++pairs_checked;
        }
    }

    // The reference family ships three pairs today; the corpus only ever GROWS.
    CHECK(pairs_checked >= 3);
    CHECK(payload_legs >= 3);
    MIGRATE_TEST_MAIN_END();
}
