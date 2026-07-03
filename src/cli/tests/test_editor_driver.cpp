// `context editor smoke` CLI driver test: the CLI-local operational command boots the composed
// EditorKernel in-process and drives the M1 attach path end to end, returning an R-CLI-008 envelope
// that reports the derived World reflecting BOTH a CLI-verb edit and a raw edit. Plus the subcommand
// grammar's failure paths (R-QA-013 happy + failure coverage).

#include "context/cli/app.h"
#include "context/editor/contract/json.h"

#include "cli_test.h"

#include <filesystem>
#include <fstream>
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
        // canonicalHash is a decimal STRING (a full-range 64-bit hash does not fit a JSON number
        // losslessly — see editor_driver.cpp), so compare the string forms.
        CHECK(!data.at("cliVerbEdit").at("canonicalHash").as_string().empty());
        CHECK(data.at("cliVerbEdit").at("canonicalHash").as_string() !=
              data.at("rawEdit").at("canonicalHash").as_string());
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

    // --- regression: a caller-supplied `--project <dir>` must SURVIVE the smoke run -----------------
    // The RAII cleanup only owns the auto-generated temp dir; a real project passed via --project (its
    // daemon lock lives at <dir>/.editor/lock) must be left intact. Guards against a catastrophic
    // `fs::remove_all(<caller dir>)` data-loss regression in run_smoke's ProjectCleanup.
    {
        namespace fs = std::filesystem;
        // Cast the file-clock rep to a concrete integer before to_string: on macOS libc++
        // fs::file_time_type::clock::rep does not uniquely match a std::to_string overload
        // (ambiguous call), while libstdc++/MSVC accept it. long long is portable + ample
        // for a unique temp-dir suffix.
        const fs::path project =
            fs::temp_directory_path() /
            ("context-editor-smoke-keep-" +
             std::to_string(static_cast<long long>(
                 fs::file_time_type::clock::now().time_since_epoch().count())));
        std::error_code ec;
        fs::remove_all(project, ec);
        fs::create_directories(project, ec);
        const fs::path sentinel = project / "keep.txt";
        {
            std::ofstream out(sentinel);
            out << "precious";
        }

        const Envelope env = context::cli::run({"editor", "smoke", "--project", project.string()});
        CHECK(env.ok()); // the smoke actually ran against the caller-supplied directory
        CHECK(fs::exists(sentinel)); // ...and left it (and its contents) untouched

        fs::remove_all(project, ec); // test-owned cleanup
    }

    CLI_TEST_MAIN_END();
}
