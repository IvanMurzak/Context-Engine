// `context session` + `context replay` CLI backend tests (R-QA-013, issue #74).

#include "context/cli/app.h"
#include "context/cli/replay_command.h"
#include "context/cli/session_command.h"
#include "cli_test.h"

#include <filesystem>
#include <map>
#include <string>

using context::cli::run;
using context::cli::run_replay;
using context::cli::run_session;
using Envelope = context::editor::contract::Envelope;

namespace
{
std::string err_code(const Envelope& e)
{
    return e.error().has_value() ? e.error()->code : std::string();
}
} // namespace

int main()
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ctx-session-cli-test";
    fs::create_directories(dir);
    const std::string state = (dir / "s.session.json").string();
    const std::string artifact = (dir / "replay.json").string();

    // --- new creates a session-state file ------------------------------------------------------
    {
        const Envelope e = run_session("new", {{"state", state}}, {{"seed", "5"}});
        CHECK(e.ok());
        CHECK(e.data().at("seed").as_string() == "5");
        CHECK(fs::exists(state));
    }

    // --- seed query reads it back --------------------------------------------------------------
    {
        const Envelope e = run_session("seed", {{"state", state}}, {});
        CHECK(e.ok());
        CHECK(e.data().at("seed").as_string() == "5");
    }

    // --- inject + step: simTick advances, the state hash is present, the injection took effect --
    {
        const Envelope inj =
            run_session("inject", {{"state", state}},
                        {{"action", "move_x"}, {"value", "3"}});
        CHECK(inj.ok());

        const Envelope step = run_session("step", {{"state", state}},
                                          {{"ticks", "20"}, {"trace", "true"}});
        CHECK(step.ok());
        CHECK(step.data().at("simTick").as_int() == 20);
        CHECK(step.data().at("stateHash").contains("root"));
        CHECK(step.data().at("stateHash").at("root").as_string().rfind("0x", 0) == 0);
        CHECK(step.data().at("trace").size() == 20);
    }

    // --- hash query returns the hierarchical hash ----------------------------------------------
    {
        const Envelope e = run_session("hash", {{"state", state}}, {});
        CHECK(e.ok());
        CHECK(e.data().at("stateHash").at("archetypes").size() >= 1);
    }

    // --- record writes a replay artifact -------------------------------------------------------
    {
        const Envelope e = run_session("record", {{"state", state}}, {{"out", artifact}});
        CHECK(e.ok());
        CHECK(fs::exists(artifact));
        CHECK(e.data().at("deterministic").as_bool());
    }

    // --- replay the artifact: verified, no divergence ------------------------------------------
    {
        const Envelope e = run_replay({{"artifact", artifact}}, {});
        CHECK(e.ok());
        CHECK(e.data().at("manifestVerified").as_bool());
    }

    // --- failure paths -------------------------------------------------------------------------
    {
        const Envelope missing_state =
            run_session("step", {{"state", (dir / "nope.json").string()}}, {});
        CHECK(!missing_state.ok());
        CHECK(err_code(missing_state) == "session.state_not_found");

        const Envelope bad_seed = run_session("new", {{"state", state}}, {{"seed", "notanumber"}});
        CHECK(!bad_seed.ok());
        CHECK(err_code(bad_seed) == "session.input_invalid");

        const Envelope bad_scenario =
            run_session("new", {{"state", state}}, {{"scenario", "bogus"}});
        CHECK(!bad_scenario.ok());
        CHECK(err_code(bad_scenario) == "session.input_invalid");

        const Envelope no_input = run_session("inject", {{"state", state}}, {});
        CHECK(!no_input.ok());
        CHECK(err_code(no_input) == "session.input_invalid");

        const Envelope missing_artifact =
            run_replay({{"artifact", (dir / "nope-artifact.json").string()}}, {});
        CHECK(!missing_artifact.ok());
        CHECK(err_code(missing_artifact) == "file.not_found");
    }

    // --- the reserved determinism-diff grammar routes to contract.unimplemented ----------------
    {
        const Envelope e = run({"determinism", "diff", "a", "b"});
        CHECK(!e.ok());
        CHECK(err_code(e) == "contract.unimplemented");
    }

    // --- the full CLI path: `context replay <artifact>` over run() -----------------------------
    {
        const Envelope e = run({"replay", artifact});
        CHECK(e.ok());
    }

    // --- numeric flags parse as decimal, never silent octal/hex (the "unsigned integer" contract) -
    {
        // A leading zero stays decimal ("012" == 12, not octal 8).
        const Envelope dec = run_session("new", {{"state", state}}, {{"seed", "012"}});
        CHECK(dec.ok());
        CHECK(dec.data().at("seed").as_string() == "12");

        // A "0x"-prefixed value is rejected, not silently read as hex.
        const Envelope hex = run_session("new", {{"state", state}}, {{"seed", "0x1f"}});
        CHECK(!hex.ok());
        CHECK(err_code(hex) == "session.input_invalid");
    }

    // --- --trace=false is a real off-switch for the persisted trace toggle ---------------------
    {
        const std::string tstate = (dir / "trace.session.json").string();
        CHECK(run_session("new", {{"state", tstate}}, {{"seed", "1"}}).ok());

        const Envelope on =
            run_session("step", {{"state", tstate}}, {{"ticks", "1"}, {"trace", "true"}});
        CHECK(on.ok());
        CHECK(on.data().contains("trace"));

        // Absent --trace, the persisted trace mode stays on...
        const Envelope still_on = run_session("step", {{"state", tstate}}, {{"ticks", "1"}});
        CHECK(still_on.ok());
        CHECK(still_on.data().contains("trace"));

        // ...and --trace=false turns it back off.
        const Envelope off =
            run_session("step", {{"state", tstate}}, {{"ticks", "1"}, {"trace", "false"}});
        CHECK(off.ok());
        CHECK(!off.data().contains("trace"));
    }

    fs::remove_all(dir);
    CLI_TEST_MAIN_END();
}
