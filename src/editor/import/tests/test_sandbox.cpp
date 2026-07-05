// Importer isolation v1 slice: jail read/write scoping, scrubbed env, honest OS-support reporting.

#include "context/editor/import/sandbox.h"

#include "import_test.h"

#include <string>

using namespace context::editor::import;

int main()
{
    // R-SEC-010: the scrubbed environment is minimal + non-secret, NOT the parent env.
    {
        const auto env = scrubbed_environment();
        bool has_lang = false;
        for (const auto& [key, value] : env)
        {
            // No secret-shaped variable ever leaks into the importer child.
            CHECK(key.find("TOKEN") == std::string::npos);
            CHECK(key.find("SECRET") == std::string::npos);
            CHECK(key.find("KEY") == std::string::npos);
            if (key == "LANG")
                has_lang = true;
        }
        CHECK(has_lang); // C locale is pinned (determinism aid)
    }

    // R-SEC-008 / R-SEC-006: reads confined to the jail; writes confined to the output key.
    {
        SandboxPolicy policy;
        policy.jail_root = "/project";
        policy.input_path = "/project/assets/hero.png";
        policy.output_key = "/project/.cache/png/texture/abc";

        // Reads: anywhere inside the jail yes; outside no.
        CHECK(read_permitted(policy, "/project/assets/hero.png"));
        CHECK(read_permitted(policy, "/project/other/file"));
        CHECK(!read_permitted(policy, "/etc/passwd"));
        CHECK(!read_permitted(policy, "/elsewhere/secrets")); // outside the jail root

        // Writes: only under the output key.
        CHECK(write_permitted(policy, "/project/.cache/png/texture/abc"));
        CHECK(write_permitted(policy, "/project/.cache/png/texture/abc/blob"));
        CHECK(!write_permitted(policy, "/project/assets/hero.png")); // inside jail but not the key
        CHECK(!write_permitted(policy, "/etc/passwd"));              // outside jail
    }

    // Empty jail / output => nothing permitted (a malformed policy fails closed).
    {
        SandboxPolicy empty;
        CHECK(!read_permitted(empty, "/anything"));
        CHECK(!write_permitted(empty, "/anything"));
        SandboxPolicy no_out;
        no_out.jail_root = "/project";
        CHECK(read_permitted(no_out, "/project/x"));
        CHECK(!write_permitted(no_out, "/project/x")); // no output key -> no writes
    }

    // HONEST staging: a primitive is named, but v1 does NOT claim it is enforced in-process, and a
    // follow-up note is always present (never a silent assumption).
    {
        const OsSandboxSupport support = os_sandbox_support();
        CHECK(!support.primitive.empty());
        CHECK(!support.enforced);       // the subprocess/seccomp lockdown is the staged rollout
        CHECK(!support.follow_up.empty());
    }

    IMPORT_TEST_MAIN_END();
}
