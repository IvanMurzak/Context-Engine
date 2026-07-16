// `context build` CLI tests (M8 task a05, issue #257) — the DoD-1 END-TO-END: a real on-disk project
// builds a real Linux pack headless, the R-CLI-008 envelope carries the build's generation + artifact
// pointers, and the written bytes are a valid pack. Plus the CLI-layer failure paths (missing --target,
// missing project → template_unverified, unknown target → toolchain_fetch_failed), --dry-run (plan
// without writing), and per-verb `--help`. The orchestrator's exhaustive R-QA-011 malformed/failure
// corpus (every build.* code) lives in src/editor/build/tests/test_build_orchestrator.cpp; here we prove
// the CLI wiring + disk IO.
//
// The project is authored with L-33 stable ids (16..32 lowercase hex) on every entity — the correct
// authored form composition requires; an id-less scene is excluded by compose (compose.missing_id) and
// would fail-closed with build.template_unverified.

#include "context/cli/app.h"
#include "context/cli/build_command.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"
#include "context/editor/pack/pack_reader.h"
#include "cli_test.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>

using namespace context::cli;
using context::editor::contract::Envelope;
using context::editor::contract::Json;
namespace pack = context::editor::pack;
namespace fs = std::filesystem;

namespace
{
void remove_quiet(const fs::path& p)
{
    std::error_code ec;
    fs::remove_all(p, ec);
}

fs::path unique_temp_dir(const std::string& tag)
{
    static int counter = 0;
    const fs::path dir =
        fs::temp_directory_path() /
        ("ctx-build-" + tag + "-" + std::to_string(++counter) + "-" +
         std::to_string(static_cast<long long>(
             fs::file_time_type::clock::now().time_since_epoch().count() & 0xffffff)));
    remove_quiet(dir);
    return dir;
}

void write_file(const fs::path& path, const std::string& bytes)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Author a minimal buildable project: project.json + an L-33-id-bearing scene with a camera.
fs::path make_project(const std::string& tag)
{
    const fs::path dir = unique_temp_dir(tag);
    write_file(dir / "project.json",
               R"({"$schema":"ctx:project","scene":"scenes/main.scene.json","version":1})");
    write_file(dir / "scenes" / "main.scene.json",
               R"({"$schema":"ctx:scene","version":1,"entities":[
                    {"id":"aaaa0000aaaa0001","name":"Camera","components":{"camera":{"fov":1.0}}},
                    {"id":"aaaa0000aaaa0002","name":"Player","components":{"transform":{}}}]})");
    return dir;
}

std::string read_bytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

int main()
{
    // --- DoD 1: an authored project builds a real Linux pack end-to-end ------------------------------
    {
        const fs::path dir = make_project("e2e");
        const fs::path out = dir / "build" / "game.pack";
        const std::map<std::string, std::string> flags = {
            {"target", "linux"}, {"project", dir.string()}, {"out", out.generic_string()}};
        const Envelope e = run_build(flags);
        CHECK(e.ok());

        // The envelope carries the build's generation + artifact pointers (the DoD-1 envelope shape).
        const Json& data = e.data();
        CHECK(data.at("target").as_string() == "linux");
        CHECK(!data.at("generation").as_string().empty()); // 64-bit content identity, decimal string
        CHECK(data.at("generation").as_string() != "0");
        CHECK(data.at("adapter").as_string() == "stub"); // a06 lands the real adapter
        const Json& artifact = data.at("artifact");
        CHECK(artifact.at("written").as_bool());
        CHECK(artifact.at("packSize").as_int() > 0);
        CHECK(!artifact.at("packHash").as_string().empty());
        CHECK(artifact.at("entityCount").as_int() == 2);
        CHECK(artifact.at("packPath").as_string() == out.generic_string());

        // The written bytes ARE a valid pack (read_pack verifies every chunk's content hash).
        CHECK(fs::exists(out));
        const pack::ParsedPack parsed = pack::read_pack(read_bytes(out));
        CHECK(parsed.ok);

        // Reproducible: a second build writes the byte-identical pack (R-FILE-010 cache property).
        const std::string first_pack = read_bytes(out);
        const Envelope again = run_build(flags);
        CHECK(again.ok());
        CHECK(read_bytes(out) == first_pack);
        CHECK(again.data().at("generation").as_string() == data.at("generation").as_string());

        remove_quiet(dir);
    }

    // --- --dry-run: plans + verifies without writing the pack ----------------------------------------
    {
        const fs::path dir = make_project("dry");
        const fs::path out = dir / "build" / "game.pack";
        const std::map<std::string, std::string> flags = {{"target", "linux"},
                                                          {"project", dir.string()},
                                                          {"out", out.generic_string()},
                                                          {"dry-run", "true"}};
        const Envelope e = run_build(flags);
        CHECK(e.ok());
        CHECK(!e.data().at("artifact").at("written").as_bool());
        CHECK(!fs::exists(out)); // dry-run wrote nothing
        remove_quiet(dir);
    }

    // --- missing --target is a usage error -----------------------------------------------------------
    {
        const Envelope e = run_build({});
        CHECK(!e.ok());
        CHECK(e.error().has_value());
        CHECK(e.error()->code == "usage.missing_argument");
    }

    // --- a project with no manifest fails the pre-build template verification -------------------------
    {
        const fs::path dir = unique_temp_dir("nomanifest");
        std::error_code ec;
        fs::create_directories(dir, ec);
        const Envelope e = run_build({{"target", "linux"}, {"project", dir.string()}});
        CHECK(!e.ok());
        CHECK(e.error()->code == "build.template_unverified");
        remove_quiet(dir);
    }

    // --- an unknown target has no toolchain manifest entry -------------------------------------------
    {
        const fs::path dir = make_project("badtarget");
        const Envelope e = run_build({{"target", "playstation"}, {"project", dir.string()}});
        CHECK(!e.ok());
        CHECK(e.error()->code == "build.toolchain_fetch_failed");
        remove_quiet(dir);
    }

    // --- through the CLI grammar: `context build --target linux --project <dir>` --------------------
    {
        const fs::path dir = make_project("cli");
        const fs::path out = dir / "build" / "game.pack";
        const Envelope e = run({"build", "--target", "linux", "--project", dir.string(), "--out",
                                out.generic_string()});
        CHECK(e.ok());
        CHECK(fs::exists(out));
        remove_quiet(dir);
    }

    // --- per-verb --help + global --help emit the contract self-description --------------------------
    {
        const Envelope verb_help = run({"build", "--help"});
        CHECK(verb_help.ok());
        CHECK(verb_help.data().at("verb").as_string() == "build");
        CHECK(verb_help.data().at("rpcMethod").as_string() == "build");

        const Envelope global_help = run({"--help"});
        CHECK(global_help.ok());
        CHECK(global_help.data().at("contract").at("verbs").is_array());
    }

    CLI_TEST_MAIN_END();
}
