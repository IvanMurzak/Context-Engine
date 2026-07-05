// `context set` (composed write) + `context query --overrides` (advisory hygiene) end-to-end through
// the CLI verb grammar over real temp scene files (issue #58). Coverage: the default-outermost
// override write + the resulting file, --edit-template, the labelled hashes, the --if-match CAS
// round-trip (pass then conflict), --dry-run write suppression, the immutable-pointer + missing-arg
// failures, and the `query --overrides redundant|diverged` reads. Happy + failure paths (R-QA-013).

#include "context/cli/app.h"
#include "context/cli/set_command.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"
#include "cli_test.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using context::cli::run;
using context::editor::contract::Envelope;
using context::editor::contract::Json;
namespace fs = std::filesystem;

namespace
{

fs::path unique_temp_dir(const std::string& tag)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("ctx-set-" + tag + "-" + std::to_string(stamp));
    fs::create_directories(dir);
    return dir;
}

void write_file(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// root -> (instance A) mid -> (instance B) child{entity C1 position [0,0,0]}.
void seed_project(const fs::path& dir)
{
    write_file(dir / "child.scene.json", R"({
  "$schema": "ctx:scene",
  "version": 1,
  "entities": [
    {"id": "ccccccccccccccc1", "name": "Light", "components": {"transform": {"position": [0, 0, 0]}}}
  ]
})");
    write_file(dir / "mid.scene.json", R"({
  "$schema": "ctx:scene",
  "version": 1,
  "entities": [],
  "instances": [{"id": "bbbbbbbbbbbbbbb1", "scene": "child.scene.json"}]
})");
    write_file(dir / "root.scene.json", R"({
  "$schema": "ctx:scene",
  "version": 1,
  "entities": [],
  "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "mid.scene.json"}]
})");
}

const std::string kFullId = "aaaaaaaaaaaaaaa1/bbbbbbbbbbbbbbb1/ccccccccccccccc1";
const std::string kPointer = "/components/transform/position";

} // namespace

int main()
{
    // --- default-outermost write: an override lands in root.scene.json, envelope reports it -------
    {
        const fs::path dir = unique_temp_dir("write");
        seed_project(dir);
        const Envelope env = run({"set", "root.scene.json", "[9, 9, 9]", "--pointer", kPointer,
                                  "--id-path", kFullId, "--project", dir.string()});
        CHECK(env.ok());
        CHECK(env.exit_code() == 0);
        CHECK(env.data().at("file").as_string() == "root.scene.json");
        CHECK(env.data().at("pointer").as_string() == "/overrides/0/value");
        CHECK(env.data().at("target").as_string() == "outermost");
        CHECK(env.data().at("applied").as_bool());
        CHECK(env.data().at("baseRecorded").as_bool());
        // BOTH labelled hashes are present (R-CLI-006) and are non-empty decimal strings.
        CHECK(!env.data().at("rawHash").as_string().empty());
        CHECK(!env.data().at("canonicalHash").as_string().empty());

        // The file on disk now carries the override, addressed by the full id-path, with a base.
        const Json doc = Json::parse(read_file(dir / "root.scene.json"));
        CHECK(doc.at("overrides").size() == 1);
        const Json& entry = doc.at("overrides").at(std::size_t{0});
        CHECK(entry.at("pointer").as_string() == kPointer);
        CHECK(entry.at("value").size() == 3);
        CHECK(entry.at("value").at(std::size_t{0}).as_int() == 9);
        CHECK(entry.at("base").size() == 3);
        CHECK(entry.at("base").at(std::size_t{0}).as_int() == 0); // the child template value
        CHECK(entry.at("path").size() == 3);
    }

    // --- --edit-template writes the DEFINING scene (child.scene.json) in place --------------------
    {
        const fs::path dir = unique_temp_dir("template");
        seed_project(dir);
        const Envelope env = run({"set", "root.scene.json", "[5, 5, 5]", "--pointer", kPointer,
                                  "--id-path", kFullId, "--edit-template", "--project",
                                  dir.string()});
        CHECK(env.ok());
        CHECK(env.data().at("file").as_string() == "child.scene.json");
        CHECK(env.data().at("target").as_string() == "template");
        const Json child = Json::parse(read_file(dir / "child.scene.json"));
        CHECK(child.at("entities").at(std::size_t{0}).at("components").at("transform").at("position").at(std::size_t{0}).as_int() == 5);
        // root.scene.json was NOT touched (no override entry).
        const Json root = Json::parse(read_file(dir / "root.scene.json"));
        CHECK(!root.contains("overrides"));
    }

    // --- --if-match CAS round-trip: matching hash applies, a stale hash conflicts -----------------
    {
        const fs::path dir = unique_temp_dir("cas");
        seed_project(dir);
        const Envelope first = run({"set", "root.scene.json", "[1, 1, 1]", "--pointer", kPointer,
                                    "--id-path", kFullId, "--project", dir.string()});
        CHECK(first.ok());
        const std::string raw_hash = first.data().at("rawHash").as_string();

        // The file's current bytes hash to `raw_hash` -> a matching --if-match applies.
        const Envelope match = run({"set", "root.scene.json", "[2, 2, 2]", "--pointer", kPointer,
                                    "--id-path", kFullId, "--if-match", raw_hash, "--project",
                                    dir.string()});
        CHECK(match.ok());

        // The file has now moved on -> the SAME (now stale) hash conflicts.
        const Envelope stale = run({"set", "root.scene.json", "[3, 3, 3]", "--pointer", kPointer,
                                    "--id-path", kFullId, "--if-match", raw_hash, "--project",
                                    dir.string()});
        CHECK(!stale.ok());
        CHECK(stale.error().has_value() && stale.error()->code == "cas.mismatch");
        CHECK(stale.exit_code() == 4); // conflict class
    }

    // --- --dry-run computes the plan + hashes but writes nothing ---------------------------------
    {
        const fs::path dir = unique_temp_dir("dryrun");
        seed_project(dir);
        const std::string before = read_file(dir / "root.scene.json");
        const Envelope env = run({"set", "root.scene.json", "[9, 9, 9]", "--pointer", kPointer,
                                  "--id-path", kFullId, "--dry-run", "--project", dir.string()});
        CHECK(env.ok());
        CHECK(!env.data().at("applied").as_bool());
        CHECK(env.data().at("dryRun").as_bool());
        CHECK(!env.data().at("canonicalHash").as_string().empty());
        CHECK(read_file(dir / "root.scene.json") == before); // untouched
    }

    // --- failure: an immutable identity pointer is refused ---------------------------------------
    {
        const fs::path dir = unique_temp_dir("immutable");
        seed_project(dir);
        const Envelope env = run({"set", "root.scene.json", "\"nope\"", "--pointer", "/id",
                                  "--id-path", kFullId, "--project", dir.string()});
        CHECK(!env.ok());
        CHECK(env.error().has_value() && env.error()->code == "compose.immutable_pointer");
    }

    // --- failure: --pointer is required ----------------------------------------------------------
    {
        const fs::path dir = unique_temp_dir("nopointer");
        seed_project(dir);
        const Envelope env = run(
            {"set", "root.scene.json", "[9, 9, 9]", "--id-path", kFullId, "--project", dir.string()});
        CHECK(!env.ok());
        CHECK(env.error().has_value() && env.error()->code == "usage.missing_argument");
    }

    // --- query --overrides redundant: a no-op override is listed (advisory) ----------------------
    {
        const fs::path dir = unique_temp_dir("redundant");
        seed_project(dir);
        // Write an override equal to the template value ([0,0,0]) -> redundant.
        const Envelope w = run({"set", "root.scene.json", "[0, 0, 0]", "--pointer", kPointer,
                                "--id-path", kFullId, "--project", dir.string()});
        CHECK(w.ok());
        const Envelope q = run({"query", "--overrides", "redundant", "root.scene.json", "--project",
                                dir.string()});
        CHECK(q.ok());
        CHECK(q.data().at("advisory").as_bool());
        CHECK(q.data().at("count").as_int() == 1);
        CHECK(q.data().at("overrides").at(std::size_t{0}).at("file").as_string() ==
              "root.scene.json");
        // ... and it is not diverged (no template drift).
        const Envelope d = run({"query", "--overrides", "diverged", "root.scene.json", "--project",
                                dir.string()});
        CHECK(d.ok());
        CHECK(d.data().at("count").as_int() == 0);
    }

    // --- query --overrides with a bad mode is a usage error --------------------------------------
    {
        const fs::path dir = unique_temp_dir("badmode");
        seed_project(dir);
        const Envelope q =
            run({"query", "--overrides", "bogus", "root.scene.json", "--project", dir.string()});
        CHECK(!q.ok());
        CHECK(q.error().has_value() && q.error()->code == "usage.invalid");
    }

    // --- a plain `context query` (no --overrides) stays daemon-served (operational-only) ----------
    {
        const fs::path dir = unique_temp_dir("plainquery");
        seed_project(dir);
        const Envelope q = run({"query", "root.scene.json", "--project", dir.string()});
        CHECK(!q.ok());
        CHECK(q.error().has_value() && q.error()->code == "contract.operational_only");
    }

    CLI_TEST_MAIN_END();
}
