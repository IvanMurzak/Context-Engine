// `context editor smoke` CLI driver test: the CLI-local operational command boots the composed
// EditorKernel in-process and drives the M1 attach path end to end, returning an R-CLI-008 envelope
// that reports the derived World reflecting BOTH a CLI-verb edit and a raw edit. Plus the subcommand
// grammar's failure paths (R-QA-013 happy + failure coverage).

#include "context/cli/app.h"
#include "context/editor/contract/json.h"

#include "cli_test.h"

#include <string>
#include <vector>

using context::editor::contract::Envelope;
using context::editor::contract::Json;

int main()
{
    // --- happy path: the composed loop runs and both edits are reflected in the derived World -----
    {
        const Envelope env = context::cli::run({"editor", "smoke"});
        CHECK(env.ok());
        CHECK(env.exit_code() == 0);

        const Json& data = env.data();
        CHECK(data.at("attached").as_bool());
        CHECK(data.at("cliVerbEdit").at("reflected").as_bool());
        CHECK(data.at("rawEdit").at("reflected").as_bool());
        // The CLI-verb edit and the raw edit derive to DIFFERENT canonical hashes on the same source.
        CHECK(data.at("cliVerbEdit").at("canonicalHash").as_int() !=
              data.at("rawEdit").at("canonicalHash").as_int());
        CHECK(data.at("worldEntities").as_int() == 1); // one source entity, updated in place
        CHECK(data.at("generation").as_int() >= 1);
        CHECK(env.warnings().empty()); // both edits landed within the read-barrier bound
    }

    // --- failure paths: subcommand grammar --------------------------------------------------------
    {
        const Envelope missing = context::cli::run({"editor"});
        CHECK(!missing.ok());
        CHECK(missing.error()->code == "usage.missing_argument");

        const Envelope unknown = context::cli::run({"editor", "bogus"});
        CHECK(!unknown.ok());
        CHECK(unknown.error()->code == "usage.unknown_verb");
    }

    CLI_TEST_MAIN_END();
}
