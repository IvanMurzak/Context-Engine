// Scaffold tests (R-QA-006): `context new`'s default template scaffolds a minimal RUNNABLE skeleton
// — a scene + a camera + a startable session such that the first query/step succeeds without error.
// Exercised both through the scaffolder API and end-to-end through the CLI `context new` verb, plus
// failure paths (unknown template, missing directory). Boots a real context_kernel session.

#include "context/cli/app.h"
#include "context/cli/scaffold.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"
#include "context/editor/schema/kind_schema.h"
#include "context/editor/schema/validator.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"
#include "cli_test.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace context::cli;
using context::editor::contract::Envelope;
using context::editor::contract::Json;

namespace
{
// Best-effort recursive delete: never throws, so a transient Windows file lock during cleanup
// cannot abort the test with an uncaught filesystem_error.
void remove_quiet(const std::filesystem::path& p)
{
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
}

std::filesystem::path unique_temp_dir(const std::string& tag)
{
    const auto base = std::filesystem::temp_directory_path();
    static int counter = 0;
    const std::filesystem::path dir =
        base / ("ctx-new-" + tag + "-" + std::to_string(++counter) + "-" +
                std::to_string(static_cast<long long>(
                    std::filesystem::file_time_type::clock::now().time_since_epoch().count() &
                    0xffffff)));
    remove_quiet(dir);
    return dir;
}
} // namespace

int main()
{
    // --- scaffold_project writes the template AND proves it runnable ---------------------------
    {
        const std::filesystem::path dir = unique_temp_dir("api");
        const Envelope e = scaffold_project(dir.string(), "default");
        CHECK(e.ok());
        CHECK(e.data().at("runnable").as_bool() == true);
        CHECK(e.data().at("cameras").as_int() >= 1);
        CHECK(e.data().at("entities").as_int() >= 1);

        // The template files actually landed and are well-formed JSON. Scope the reader so its
        // file handle is closed before remove_all — Windows refuses to delete an open file.
        CHECK(std::filesystem::exists(dir / "project.json"));
        CHECK(std::filesystem::exists(dir / "scenes" / "main.scene.json"));
        {
            std::ifstream scene_in(dir / "scenes" / "main.scene.json", std::ios::binary);
            std::string scene_text((std::istreambuf_iterator<char>(scene_in)),
                                   std::istreambuf_iterator<char>());
            const Json scene = Json::parse(scene_text);
            CHECK(scene.at("kind").as_string() == "scene");
            CHECK(scene.at("entities").size() >= 1);

            // Tool saves canonicalize the whole file they write (R-FILE-001): the scaffolded
            // bytes must already BE the canonical form (a canonicalization fixpoint).
            CHECK(context::editor::serializer::canonicalize(scene_text).bytes == scene_text);

            // M2 wave 2 (R-DATA-006/#47): the template carries the L-32 header binding it to the
            // registered scene kind, exposes the schema-blessed notes affordance, and VALIDATES
            // clean against the engine schema set — the validate node checks it on every attach.
            CHECK(scene.at("$schema").as_string() == "ctx:scene");
            CHECK(scene.at("version").as_int() == 1);
            CHECK(!scene.at("notes").as_string().empty());
            // The units law: the template's camera fov is radians (60 degrees ~ 1.047), never 60.
            const double fov = scene.at("entities")
                                   .at(0)
                                   .at("components")
                                   .at("camera")
                                   .at("fov")
                                   .as_number();
            CHECK(fov > 1.0 && fov < 1.1);
            {
                namespace schema = context::editor::schema;
                namespace serializer = context::editor::serializer;
                auto parsed = serializer::parse_json(scene_text);
                CHECK(parsed.ok);
                const schema::ValidationReport report = schema::validate_document(
                    parsed.root, scene_text, schema::engine_schemas());
                CHECK(report.schema_bound);
                CHECK(report.ok);
                CHECK(report.diagnostics.empty());
            }
        }

        // project.json is likewise schema-bound (ctx:project@1) and validates clean.
        {
            std::ifstream project_in(dir / "project.json", std::ios::binary);
            std::string project_text((std::istreambuf_iterator<char>(project_in)),
                                     std::istreambuf_iterator<char>());
            const Json project = Json::parse(project_text);
            CHECK(project.at("$schema").as_string() == "ctx:project");
            CHECK(project.at("version").as_int() == 1);
            CHECK(project.at("engine").as_string() == "context");
            namespace schema = context::editor::schema;
            namespace serializer = context::editor::serializer;
            auto parsed = serializer::parse_json(project_text);
            CHECK(parsed.ok);
            const schema::ValidationReport report =
                schema::validate_document(parsed.root, project_text, schema::engine_schemas());
            CHECK(report.schema_bound);
            CHECK(report.ok);
            CHECK(report.diagnostics.empty());
        }

        // The template ships .gitattributes pinning authored JSON to LF (R-FILE-001).
        CHECK(std::filesystem::exists(dir / ".gitattributes"));
        {
            std::ifstream attrs_in(dir / ".gitattributes", std::ios::binary);
            std::string attrs_text((std::istreambuf_iterator<char>(attrs_in)),
                                   std::istreambuf_iterator<char>());
            CHECK(attrs_text.find("*.json text eol=lf") != std::string::npos);
        }

        remove_quiet(dir);
    }

    // --- verify_runnable on the scaffolded dir: the startable-session proof ---------------------
    {
        const std::filesystem::path dir = unique_temp_dir("verify");
        CHECK(scaffold_project(dir.string(), "default").ok());
        const Envelope run_env = verify_runnable(dir.string());
        CHECK(run_env.ok());
        CHECK(run_env.data().at("ticks").as_int() >= 1);   // the first step ran
        CHECK(run_env.data().at("cameras").as_int() >= 1); // the first camera query found it
        remove_quiet(dir);
    }

    // --- end-to-end through the CLI `context new <dir>` verb -----------------------------------
    {
        const std::filesystem::path dir = unique_temp_dir("cli");
        const Envelope e = run({"new", dir.string()});
        CHECK(e.ok());
        CHECK(e.exit_code() == 0);
        CHECK(e.data().at("runnable").as_bool() == true);
        CHECK(std::filesystem::exists(dir / "project.json"));
        remove_quiet(dir);
    }

    // --- failure path: an unknown template is rejected -----------------------------------------
    {
        const std::filesystem::path dir = unique_temp_dir("badtmpl");
        const Envelope e = scaffold_project(dir.string(), "does-not-exist");
        CHECK(!e.ok());
        CHECK(e.error()->code == "usage.invalid");
        CHECK(!std::filesystem::exists(dir)); // nothing was created for a bad template
        remove_quiet(dir);
    }

    // --- failure path: verify_runnable on a non-existent project ------------------------------
    {
        const std::filesystem::path dir = unique_temp_dir("missing");
        const Envelope e = verify_runnable(dir.string());
        CHECK(!e.ok());
        CHECK(e.error()->code == "file.not_found");
    }

    CLI_TEST_MAIN_END();
}
