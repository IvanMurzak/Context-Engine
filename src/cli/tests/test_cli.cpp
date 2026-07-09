// CLI grammar + dispatch tests: verb selection (global + noun-scoped + package ns), core/verb flag
// parsing, required-arg enforcement, the reserved-surface path, and exit-code mapping through the
// R-CLI-008 envelope (R-CLI-007/008/009; happy + failure paths, R-QA-013).

#include "context/cli/app.h"
#include "context/cli/wire_client.h"
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
        CHECK(e.data().at("contract").at("protocol").at("protocolMajor").as_int() == 1);
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

    // --- reserved verb: unimplemented without --dry-run, plan WITH it ----------------------------
    // (`asset move` is a still-reserved stable verb; `set` graduated to the composed write path.)
    {
        const Envelope e = run({"asset", "move", "a.png", "b.png"});
        CHECK(err_code(e) == "contract.unimplemented");
        CHECK(e.exit_code() == 8);
    }
    {
        const Envelope e = run({"asset", "move", "a.png", "b.png", "--dry-run"});
        CHECK(e.ok());
        CHECK(e.data().at("wouldApply").as_bool() == false);
        CHECK(e.data().at("args").size() == 2);
    }
    // --- `set` is now the IMPLEMENTED composed write path (R-CLI-006): it validates its flags
    // --- rather than reporting a reserved surface. Full write coverage lives in test_set_command. -
    {
        const Envelope e = run({"set", "scenes/main.scene.json", "42"}); // no --pointer / --id-path
        CHECK(!e.ok());
        CHECK(err_code(e) == "usage.missing_argument");
    }
    // --- set missing its required positional args ------------------------------------------------
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

    // --- the R-CLI-017 fetch verb: registry-owned alias + canonical form resolve identically -----
    {
        // Both spellings resolve to the SAME registered verb (dry-run: no daemon needed).
        const Envelope alias = run({"fetch", "context-res://v0/i/1?bytes=9", "--dry-run"});
        CHECK(alias.ok());
        CHECK(alias.data().at("verb").as_string() == "context resource read");
        const Envelope canonical =
            run({"resource", "read", "context-res://v0/i/1?bytes=9", "--dry-run"});
        CHECK(canonical.ok());
        CHECK(canonical.data().at("verb").as_string() == "context resource read");

        // Required-arg enforcement holds through the alias too.
        const Envelope missing = run({"fetch"});
        CHECK(err_code(missing) == "usage.missing_argument");

        // Without --project there is no daemon to resolve against (a REAL fetch is the e2e's job).
        const Envelope no_project = run({"fetch", "context-res://v0/i/1?bytes=9"});
        CHECK(err_code(no_project) == "usage.missing_argument");

        // A non-URI handle fails fast with the R-CLI-017 catalog code (not-found exit class 3).
        const Envelope bogus = run({"fetch", "not-a-uri", "--project", "/nowhere"});
        CHECK(err_code(bogus) == "resource.unknown_handle");
        CHECK(bogus.exit_code() == 3);
    }

    // --- operational daemon-driver verbs are registered (describe-honesty) but NOT one-shot CLI
    // --- verbs: invoking one names the wire door instead of pretending "reserved" (R-CLI-009) -----
    {
        const Envelope edit = run({"edit", "proj/a.scene", "entity: 1"});
        CHECK(err_code(edit) == "contract.operational_only");
        CHECK(edit.exit_code() == 2); // usage class
        const Envelope snap = run({"snapshot"});
        CHECK(err_code(snap) == "contract.operational_only");
        // `build` is scope-reserved AND operational — same one-shot rejection.
        const Envelope build = run({"build"});
        CHECK(err_code(build) == "contract.operational_only");
    }

    // --- wire_client parse_u64: strict operational-flag value parsing. Deliberately NOT stoull,
    // --- which wraps "-1" to ~2^64 (silently turning e.g. the --crawl-interval-ms safety net off)
    // --- and ignores trailing junk ("600abc" -> 600).
    {
        using context::cli::parse_u64;
        CHECK(parse_u64("0").has_value() && *parse_u64("0") == 0);
        CHECK(parse_u64("30000").has_value() && *parse_u64("30000") == 30000);
        CHECK(!parse_u64("").has_value());
        CHECK(!parse_u64("-1").has_value());
        CHECK(!parse_u64("600abc").has_value());
        CHECK(!parse_u64(" 5").has_value());
        CHECK(parse_u64("18446744073709551615").has_value()); // 2^64-1 fits
        CHECK(!parse_u64("18446744073709551616").has_value()); // 2^64 overflows
    }

    CLI_TEST_MAIN_END();
}
