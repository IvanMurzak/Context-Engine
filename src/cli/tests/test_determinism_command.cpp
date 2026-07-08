// `context determinism diff <left> <right>` CLI backend tests (R-QA-013, issue #74). Drives the
// verb over the full run() grammar path AND the direct backend, asserting the structured triage
// envelope + the failure paths (missing / malformed artifact).

#include "context/cli/app.h"
#include "context/cli/determinism_command.h"
#include "context/runtime/session/replay.h"
#include "cli_test.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>

using context::cli::run;
using context::cli::run_determinism_diff;
using Envelope = context::editor::contract::Envelope;
namespace session = context::runtime::session;

namespace
{
std::string err_code(const Envelope& e)
{
    return e.error().has_value() ? e.error()->code : std::string();
}

void write_file(const std::filesystem::path& p, const std::string& content)
{
    std::ofstream out(p, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

session::SessionConfig cfg(std::uint64_t seed)
{
    session::SessionConfig c;
    c.seed = seed;
    return c;
}

// Write a recorded ctx:replay artifact for (seed, stream, ticks) to `path`.
void write_artifact(const std::filesystem::path& path, std::uint64_t seed,
                    const session::InputStream& stream, std::uint64_t ticks)
{
    const session::ReplayArtifact a = session::record_replay(cfg(seed), stream, ticks, {}, true);
    write_file(path, session::replay_dump(a));
}
} // namespace

int main()
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ctx-determinism-cli-test";
    fs::create_directories(dir);
    const std::string left = (dir / "left.json").string();
    const std::string right = (dir / "right.json").string();

    // --- a DETERMINISTIC PAIR: no divergence, over the full CLI grammar path --------------------
    {
        write_artifact(dir / "left.json", 42, {}, 8);
        write_artifact(dir / "right.json", 42, {}, 8);
        const Envelope e = run({"determinism", "diff", left, right});
        CHECK(e.ok()); // the verb is IMPLEMENTED now (no longer contract.unimplemented)
        CHECK(!e.data().at("diverged").as_bool());
        CHECK(e.data().at("seedMatch").as_bool());
        CHECK(e.data().at("inputMatch").as_bool());
    }

    // --- an injected divergence is pinpointed to (tick, system, entity, componentField) ---------
    {
        session::InputStream rs;
        rs.add_action(3, session::ActionActivation{"move_x", "performed", 7});
        write_artifact(dir / "left.json", 42, {}, 8);
        write_artifact(dir / "right.json", 42, rs, 8);

        const Envelope e = run_determinism_diff({{"left", left}, {"right", right}}, {});
        CHECK(e.ok());
        CHECK(e.data().at("diverged").as_bool());
        CHECK(e.data().at("reproduced").as_bool());
        CHECK(e.data().at("tick").as_int() == 3);
        CHECK(e.data().at("system").as_string() == "input");
        CHECK(e.data().at("component").as_string() == "input_state");
        CHECK(e.data().at("field").as_string() == "move_x");
        CHECK(e.data().at("componentField").as_string() == "input_state.move_x");
        CHECK(e.data().at("leftValue").as_int() == 0);
        CHECK(e.data().at("rightValue").as_int() == 7);
        CHECK(e.data().contains("entity"));
        CHECK(!e.data().at("inputMatch").as_bool()); // different input streams
    }

    // --- failure paths: missing + malformed artifacts -------------------------------------------
    {
        const Envelope missing =
            run_determinism_diff({{"left", (dir / "nope.json").string()}, {"right", right}}, {});
        CHECK(!missing.ok());
        CHECK(err_code(missing) == "file.not_found");

        const std::string bad = (dir / "bad.json").string();
        write_file(dir / "bad.json", "not a replay artifact");
        const Envelope malformed = run_determinism_diff({{"left", left}, {"right", bad}}, {});
        CHECK(!malformed.ok());
        CHECK(err_code(malformed) == "replay.artifact_invalid");

        // Missing a required positional over the grammar path.
        const Envelope one_arg = run({"determinism", "diff", left});
        CHECK(!one_arg.ok());
        CHECK(err_code(one_arg) == "usage.missing_argument");
    }

    fs::remove_all(dir);
    CLI_TEST_MAIN_END();
}
