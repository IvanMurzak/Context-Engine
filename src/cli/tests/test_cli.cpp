// CLI grammar + dispatch tests: verb selection (global + noun-scoped + package ns), core/verb flag
// parsing, required-arg enforcement, the reserved-surface path, and exit-code mapping through the
// R-CLI-008 envelope (R-CLI-007/008/009; happy + failure paths, R-QA-013).

#include "context/cli/app.h"
#include "context/editor/contract/json.h"
#include "cli_test.h"

#include <string>
#include <vector>

using context::cli::run;
using context::editor::contract::Envelope;
using context::editor::contract::Json;

namespace
{
std::string err_code(const Envelope& e)
{
    return e.error().has_value() ? e.error()->code : std::string();
}
} // namespace

int main()
{
    // --- describe: ok, carries the whole contract, re-parses -----------------------------------
    {
        const Envelope e = run({"describe"});
        CHECK(e.ok());
        CHECK(e.exit_code() == 0);
        CHECK(e.data().at("contract").at("protocol").at("protocolMajor").as_int() == 0);
        const Json round = Json::parse(e.dump());
        CHECK(round.at("data").at("contract").contains("verbs"));
    }

    // --- describe honors a core flag with a value (--project /x) --------------------------------
    {
        const Envelope e = run({"describe", "--project", "/some/root"});
        CHECK(e.ok());
    }
    // --- describe honors a boolean core flag (--json) ------------------------------------------
    {
        const Envelope e = run({"describe", "--json"});
        CHECK(e.ok());
    }

    // --- empty args => usage.invalid ------------------------------------------------------------
    {
        const Envelope e = run({});
        CHECK(!e.ok());
        CHECK(err_code(e) == "usage.invalid");
        CHECK(e.exit_code() == 2);
    }

    // --- unknown verb ---------------------------------------------------------------------------
    {
        const Envelope e = run({"frobnicate"});
        CHECK(err_code(e) == "usage.unknown_verb");
        CHECK(e.exit_code() == 2);
    }
    // --- a package-namespaced verb that does not resolve is unknown -----------------------------
    {
        const Envelope e = run({"physics:body", "set"});
        CHECK(err_code(e) == "usage.unknown_verb");
    }

    // --- unknown flag ---------------------------------------------------------------------------
    {
        const Envelope e = run({"describe", "--not-a-flag"});
        CHECK(err_code(e) == "usage.unknown_flag");
        CHECK(e.exit_code() == 2);
    }

    // --- a non-bool flag missing its value ------------------------------------------------------
    {
        const Envelope e = run({"describe", "--project"});
        CHECK(err_code(e) == "usage.missing_argument");
    }

    // --- noun-scoped verb resolves; reserved surface => contract.unimplemented (exit 8) ---------
    {
        const Envelope e = run({"package", "add", "some-pkg"});
        CHECK(!e.ok());
        CHECK(err_code(e) == "contract.unimplemented");
        CHECK(e.exit_code() == 8);
    }
    // --- noun-scoped verb missing its required arg ---------------------------------------------
    {
        const Envelope e = run({"package", "add"});
        CHECK(err_code(e) == "usage.missing_argument");
    }

    // --- reserved file-rewriter verb: unimplemented without --dry-run, plan WITH it -------------
    {
        const Envelope e = run({"set", "scenes/main.scene.json", "42"});
        CHECK(err_code(e) == "contract.unimplemented");
        CHECK(e.exit_code() == 8);
    }
    {
        const Envelope e = run({"set", "scenes/main.scene.json", "42", "--dry-run"});
        CHECK(e.ok());
        CHECK(e.data().at("wouldApply").as_bool() == false);
        CHECK(e.data().at("args").size() == 2);
    }
    // --- set missing its required path arg ------------------------------------------------------
    {
        const Envelope e = run({"set"});
        CHECK(err_code(e) == "usage.missing_argument");
    }

    // --- new missing its required directory arg -------------------------------------------------
    {
        const Envelope e = run({"new"});
        CHECK(err_code(e) == "usage.missing_argument");
    }
    // --- new --dry-run does NO I/O and reports the plan ----------------------------------------
    {
        const Envelope e = run({"new", "some-dir", "--dry-run"});
        CHECK(e.ok());
        CHECK(e.data().at("directory").as_string() == "some-dir");
        // .gitattributes (the R-FILE-001 LF pin) + project.json + scenes/main.scene.json.
        CHECK(e.data().at("files").size() == 3);
        CHECK(e.data().at("files").at(0).as_string() == ".gitattributes");
    }

    CLI_TEST_MAIN_END();
}
