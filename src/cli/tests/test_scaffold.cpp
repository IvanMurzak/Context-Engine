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

    // ============================================================================================
    // The M9 e13e `extension-panel` template (issue #467). What is asserted HERE is the CLI's own
    // half: the files land, the manifest says what it must, and the refusals refuse. That the
    // result is a package the EDITOR accepts is asserted where both tiers can be linked —
    // src/tests/integration/test_e13e_ext_scaffold.cpp (ctest editor-shell-ext-package-scaffold),
    // which takes the scaffold through the real store scan + `context-ext://` resolve. Neither
    // suite substitutes for the other: this one cannot see the store, and asserting on the
    // template's TEXT would pass just as well against a template the store rejects.
    // ============================================================================================

    // --- extension-panel writes a complete package and PROVES it loadable -----------------------
    {
        const std::filesystem::path dir = unique_temp_dir("extpanel");
        const Envelope e = scaffold_project(dir.string(), kExtensionPanelTemplate);
        CHECK(e.ok());
        CHECK(e.data().at("loadable").as_bool() == true);
        CHECK(e.data().at("packageId").as_string() == dir.filename().string());
        CHECK(e.data().at("contributions").as_int() == 1);
        CHECK(e.data().at("files").size() == 5);

        // Every file the envelope reports actually exists — a report is not evidence of a write.
        for (std::size_t i = 0; i < e.data().at("files").size(); ++i)
        {
            CHECK(std::filesystem::exists(dir / e.data().at("files").at(i).as_string()));
        }
        CHECK(std::filesystem::exists(dir / kExtensionPanelManifestFileName));
        CHECK(std::filesystem::exists(dir / kExtensionPanelEntryFileName));

        // The manifest is canonical from its first byte (R-FILE-001: `context new` is a tool save)
        // and says what the store requires of it.
        {
            std::ifstream manifest_in(dir / kExtensionPanelManifestFileName, std::ios::binary);
            std::string manifest_text((std::istreambuf_iterator<char>(manifest_in)),
                                      std::istreambuf_iterator<char>());
            // Never hand an EMPTY text to `Json::parse` — it is the only throwing call in this file,
            // and an uncaught parse exception aborts the whole executable, so ctest reports
            // `Subprocess aborted` with no line number and every later case here (the flag/positional
            // precedence, the dry runs, all four refusal paths, the template catalog) dies unreported
            // alongside it. `Json::at` returns a shared null for a missing key, so the assertions
            // below still fail LOUDLY and in place when the manifest was never written.
            CHECK(!manifest_text.empty());
            CHECK(context::editor::serializer::canonicalize(manifest_text).bytes == manifest_text);

            const Json manifest =
                manifest_text.empty() ? Json::object() : Json::parse(manifest_text);
            const std::string package_id = dir.filename().string();
            CHECK(manifest.at("id").as_string() == package_id);
            CHECK(manifest.at("contributions").size() == 1);
            const Json& c = manifest.at("contributions").at(0);
            // Namespaced to the package, iframe content on the package's OWN origin, v2, and the
            // dock/state members the R-EDIT-001 manifest v2 shape carries.
            CHECK(c.at("id").as_string() == package_id + ".panel");
            CHECK(c.at("kind").as_string() == "panel");
            CHECK(c.at("contractVersion").as_int() == kExtensionPanelContractVersion);
            CHECK(c.at("content").at("type").as_string() == "iframe");
            CHECK(c.at("content").at("entry").as_string() ==
                  "context-ext://" + package_id + "/" + kExtensionPanelEntryFileName);
            CHECK(c.at("dock").at("singleton").as_bool() == true);
            CHECK(c.at("state").at("schemaVersion").as_int() == 1);
            CHECK(c.at("capabilities").size() == 1);
            CHECK(c.at("capabilities").at(0).as_string() == "read_query");
        }

        // The panel document loads under a strict CSP (`script-src 'self'` / `style-src 'self'`,
        // neither carrying 'unsafe-inline'), so the template must not teach an inline script or an
        // inline style block — both would be refused and the panel would come up blank.
        {
            std::ifstream html_in(dir / kExtensionPanelEntryFileName, std::ios::binary);
            std::string html((std::istreambuf_iterator<char>(html_in)),
                             std::istreambuf_iterator<char>());
            // Asserted STRUCTURALLY, not against the one literal `<script>`: `<script type=
            // "module">` and `<script defer>` are the likeliest ways this gets "simplified" back to
            // inline, and both slip past an exact-string search. So: the document's ONLY `<script`
            // is the src'd one, there is no `<style` element, and no inline `style=` attribute
            // (`style-src 'self'` blocks those too).
            const std::size_t script_at = html.find("<script");
            CHECK(script_at == html.find("<script src=\"panel.js\""));
            CHECK(script_at != std::string::npos);
            const bool panel_html_has_no_second_script =
                script_at == std::string::npos || html.find("<script", script_at + 1) == std::string::npos;
            CHECK(panel_html_has_no_second_script);
            CHECK(html.find("<link rel=\"stylesheet\" href=\"panel.css\"") != std::string::npos);
            CHECK(html.find("<style") == std::string::npos);
            CHECK(html.find(" style=") == std::string::npos);
            CHECK(html.find("javascript:") == std::string::npos);
        }

        // The README states the three-step budget where the person who ran `context new` sees it.
        {
            std::ifstream readme_in(dir / "README.md", std::ios::binary);
            std::string readme((std::istreambuf_iterator<char>(readme_in)),
                               std::istreambuf_iterator<char>());
            CHECK(readme.find("Three steps") != std::string::npos);
            CHECK(readme.find("context new --template extension-panel") != std::string::npos);
        }

        remove_quiet(dir);
    }

    // --- the documented invocation: `context new --template extension-panel <dir>` --------------
    {
        const std::filesystem::path dir = unique_temp_dir("extflag");
        const Envelope e = run({"new", "--template", kExtensionPanelTemplate, dir.string()});
        CHECK(e.ok());
        CHECK(e.exit_code() == 0);
        CHECK(e.data().at("template").as_string() == kExtensionPanelTemplate);
        CHECK(std::filesystem::exists(dir / kExtensionPanelManifestFileName));
        remove_quiet(dir);
    }

    // --- the M1 positional spelling still works, and the FLAG wins when both are given ----------
    {
        const std::filesystem::path positional = unique_temp_dir("extpos");
        CHECK(run({"new", positional.string(), kExtensionPanelTemplate}).ok());
        CHECK(std::filesystem::exists(positional / kExtensionPanelManifestFileName));
        remove_quiet(positional);

        const std::filesystem::path both = unique_temp_dir("extboth");
        const Envelope e =
            run({"new", both.string(), "default", "--template", kExtensionPanelTemplate});
        CHECK(e.ok());
        CHECK(e.data().at("template").as_string() == kExtensionPanelTemplate);
        // The DEFAULT template's marker file is absent — proof the flag decided, not the positional.
        CHECK(std::filesystem::exists(both / kExtensionPanelManifestFileName));
        CHECK(!std::filesystem::exists(both / "project.json"));
        remove_quiet(both);
    }

    // --- --dry-run reports the extension-panel plan and does NO I/O -----------------------------
    {
        // A unique temp path, like every other fixture here — NOT a fixed relative name in the ctest
        // working directory: one earlier run in which dry-run was broken would leave that directory
        // behind and red this assertion permanently, pointing at a working implementation.
        const std::filesystem::path dir = unique_temp_dir("extdry");
        const Envelope e =
            run({"new", dir.string(), "--template", kExtensionPanelTemplate, "--dry-run"});
        CHECK(e.ok());
        CHECK(e.data().at("template").as_string() == kExtensionPanelTemplate);
        CHECK(e.data().at("files").size() == 5);
        // Searched by NAME, not by index: `files[1]` couples the assertion to the byte-sorted
        // position, which any added file sorting before the manifest would break for no reason.
        bool plan_lists_the_manifest = false;
        for (std::size_t i = 0; i < e.data().at("files").size(); ++i)
            if (e.data().at("files").at(i).as_string() == kExtensionPanelManifestFileName)
                plan_lists_the_manifest = true;
        CHECK(plan_lists_the_manifest);
        CHECK(!std::filesystem::exists(dir));
        remove_quiet(dir);
    }

    // --- --dry-run runs the SAME refusals as the real run --------------------------------------
    // A dry run exists to PREDICT the apply, and it is the surface an agent consults before
    // deciding to write. Both claims below reported a confident `ok` plan before `scaffold_dry_run`.
    {
        // An unknown template reported the DEFAULT template's file list under the typo'd name,
        // because `scaffold_plan`'s else-branch MEANS "default".
        const Envelope e =
            run({"new", "some-package-dir", "--template", "extension", "--dry-run"});
        const bool dry_run_refuses_an_unknown_template = !e.ok();
        CHECK(dry_run_refuses_an_unknown_template);
        CHECK(e.error().has_value());
        if (e.error().has_value())
        {
            CHECK(e.error()->code == "usage.invalid");
            // Attributed: `usage.invalid` is shared with the package-id refusal below.
            CHECK(e.error()->message.find("unknown template") != std::string::npos);
        }
    }
    {
        // An illegal package-directory name reported five files the apply fails closed on.
        const Envelope e = run({"new", "Upper", "--template", kExtensionPanelTemplate, "--dry-run"});
        const bool dry_run_refuses_an_illegal_package_id = !e.ok();
        CHECK(dry_run_refuses_an_illegal_package_id);
        CHECK(e.error().has_value());
        if (e.error().has_value())
        {
            CHECK(e.error()->code == "usage.invalid");
            CHECK(e.error()->message.find("cannot be an editor-package id") != std::string::npos);
        }
        CHECK(!std::filesystem::exists(std::filesystem::path("Upper")));
    }

    // --- failure path: a directory name that cannot be a package id is refused, and NOTHING is
    //     written. A package's id IS its directory name, so scaffolding into such a directory would
    //     produce a package the store refuses (`package.id_invalid`) — worse than no scaffold. The
    //     agreement between this refusal and the store's is pinned in the integration tier.
    {
        const std::filesystem::path parent = unique_temp_dir("extbadid");
        const std::filesystem::path dir = parent / "Upper";
        const Envelope e = scaffold_project(dir.string(), kExtensionPanelTemplate);
        CHECK(!e.ok());
        CHECK(e.error()->code == "usage.invalid");
        // Attributed to the ID grammar: `usage.invalid` is also what an unknown template returns,
        // so the code alone does not say which refusal fired.
        CHECK(e.error()->message.find("cannot be an editor-package id") != std::string::npos);
        CHECK(!std::filesystem::exists(dir));
        remove_quiet(parent);
    }
    {
        // A representative row per clause at the CLI's own predicate. The exhaustive table — both
        // length bounds, the traversal shapes, the byte classes — is the cross-tier one in
        // test_e13e_ext_scaffold.cpp, which checks each id against the SHELL's answer too.
        CHECK(is_scaffold_package_id("hello-panel"));
        CHECK(is_scaffold_package_id("acme.hello-panel"));
        CHECK(!is_scaffold_package_id(""));
        CHECK(!is_scaffold_package_id("Upper"));
        CHECK(!is_scaffold_package_id(".hidden"));
        CHECK(!is_scaffold_package_id("trailing-"));
        CHECK(!is_scaffold_package_id("a..b"));
        CHECK(!is_scaffold_package_id("pkg.2"));
        CHECK(!is_scaffold_package_id("12345"));
        CHECK(!is_scaffold_package_id("has space"));
    }

    // --- failure path: verify_extension_package refuses what the store would refuse -------------
    {
        // The check the STORE cannot make: `read_package_manifest` validates the entry's URL
        // grammar and never opens the file, so a manifest naming a missing document is accepted
        // there and fails only at load, as a blank panel with nothing saying why.
        const std::filesystem::path dir = unique_temp_dir("extnoentry");
        CHECK(scaffold_project(dir.string(), kExtensionPanelTemplate).ok());
        std::error_code ec;
        std::filesystem::remove(dir / kExtensionPanelEntryFileName, ec);
        const Envelope e = verify_extension_package(dir.string());
        CHECK(!e.ok());
        CHECK(e.error()->code == "file.not_found");
        remove_quiet(dir);
    }
    {
        // The manifest's id must equal the directory name — the store refuses a disagreement rather
        // than reconciling it.
        //
        // ⚠ The staged manifest carries TWO independent defects (a wrong `id` AND an empty
        // `contributions`) and BOTH refuse with `file.validation_failed`, so the CODE alone is
        // satisfied by whichever rule fires — deleting the id check outright would leave this green
        // on the contributions rule. The MESSAGE is what attributes it, and the block after this one
        // pins the contributions rule on its own fixture.
        const std::filesystem::path dir = unique_temp_dir("extmismatch");
        CHECK(scaffold_project(dir.string(), kExtensionPanelTemplate).ok());
        {
            std::ofstream out(dir / kExtensionPanelManifestFileName,
                              std::ios::binary | std::ios::trunc);
            out << "{\"id\":\"somebody-else\",\"contributions\":[]}";
        }
        const Envelope e = verify_extension_package(dir.string());
        CHECK(!e.ok());
        CHECK(e.error()->code == "file.validation_failed");
        const bool refused_for_the_id_mismatch =
            e.error().has_value() &&
            e.error()->message.find("must equal the directory name") != std::string::npos;
        CHECK(refused_for_the_id_mismatch);
        remove_quiet(dir);
    }
    {
        // ...and the contributions rule on its OWN fixture, carrying the directory's own id, so the
        // id check above cannot be what refuses it.
        const std::filesystem::path dir = unique_temp_dir("extnocontrib");
        CHECK(scaffold_project(dir.string(), kExtensionPanelTemplate).ok());
        {
            std::ofstream out(dir / kExtensionPanelManifestFileName,
                              std::ios::binary | std::ios::trunc);
            out << "{\"id\":\"" << dir.filename().string() << "\",\"contributions\":[]}";
        }
        const Envelope e = verify_extension_package(dir.string());
        CHECK(!e.ok());
        const bool refused_for_declaring_no_contributions =
            e.error().has_value() &&
            e.error()->message.find("declares no contributions") != std::string::npos;
        CHECK(refused_for_declaring_no_contributions);
        remove_quiet(dir);
    }
    {
        // A FAILED write must not leave a package behind. The manifest is written FIRST and the `||`
        // chain short-circuits, so a failure on a later file leaves the earlier ones on disk — and
        // the store validates the entry's URL GRAMMAR without ever opening the document, so it
        // ACCEPTS such a directory and mounts a panel whose script 404s: the blank frame with
        // nothing naming why. A pre-existing DIRECTORY named `panel.js` is an unwritable name, which
        // forces the fourth write to fail with the first three already written.
        const std::filesystem::path dir = unique_temp_dir("extpartial");
        std::error_code ec;
        std::filesystem::create_directories(dir / "panel.js", ec);
        CHECK(!ec);
        const Envelope e = scaffold_project(dir.string(), kExtensionPanelTemplate);
        CHECK(!e.ok());
        const bool partial_write_left_no_manifest =
            !std::filesystem::exists(dir / kExtensionPanelManifestFileName);
        CHECK(partial_write_left_no_manifest);
        remove_quiet(dir);
    }
    {
        // ...and a manifest that is not JSON at all is a parse failure, not a crash.
        const std::filesystem::path dir = unique_temp_dir("extbadjson");
        CHECK(scaffold_project(dir.string(), kExtensionPanelTemplate).ok());
        {
            std::ofstream out(dir / kExtensionPanelManifestFileName,
                              std::ios::binary | std::ios::trunc);
            out << "{{{";
        }
        const Envelope e = verify_extension_package(dir.string());
        CHECK(!e.ok());
        CHECK(e.error()->code == "file.parse_error");
        remove_quiet(dir);
    }

    // --- both templates are offered, and only they -----------------------------------------------
    {
        CHECK(template_names().size() == 2);
        CHECK(is_known_template(kDefaultTemplate));
        CHECK(is_known_template(kExtensionPanelTemplate));
        CHECK(!is_known_template("extension"));
    }

    CLI_TEST_MAIN_END();
}
