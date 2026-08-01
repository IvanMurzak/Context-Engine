// `context new` runnable-template scaffolder (see scaffold.h). Consumes context_kernel to PROVE the
// scaffolded default template yields a startable session (R-QA-006).

#include "context/cli/scaffold.h"

#include "context/editor/contract/json.h"
#include "context/editor/schema/kind_schema.h"
#include "context/editor/serializer/canonical.h"
#include "context/kernel/kernel.h"
#include "context/kernel/scheduler.h"
#include "context/kernel/world.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace context::cli
{

using editor::contract::Envelope;
using editor::contract::Json;

namespace
{
// Components the runnable-template proof materializes in the kernel World. Deliberately tiny — the
// point is that a real World holds a camera entity a query can find after one Scheduler step.
struct Transform
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};
struct Camera
{
    double fov = 60.0;
    double near_plane = 0.1;
    double far_plane = 1000.0;
};
struct Named
{
    std::string name;
};

// The directory's own name. Cosmetic for the default template (it becomes the project's `name`),
// but LOAD-BEARING for `extension-panel`, where the directory name IS the package id.
//
// ⚠ A TRAILING SEPARATOR makes `filename()` EMPTY — `<store>/hello-panel` and `<store>/hello-panel/`
// are the same directory but not the same path — and shell tab-completion appends one. Normalize it
// away BEFORE the fallback, or `context new --template extension-panel <store>/hello-panel/` writes
// a manifest declaring the id `project` into a directory named `hello-panel`: the store derives a
// package's id from the DIRECTORY ENTRY name, so it refuses that package at boot for declaring an id
// its directory does not carry. The CLI could not catch it either, because `verify_extension_package`
// re-derives the expected id through THIS function and so agreed with the writer's own mistake.
std::string project_basename(const std::string& directory)
{
    std::filesystem::path p(directory);
    if (p.filename().empty())
        p = p.parent_path();
    std::string name = p.filename().string();
    if (name.empty())
        name = "project";
    return name;
}

// The default template's two files, as JSON DOM (so they are guaranteed well-formed). Both carry
// the L-32 header ("$schema" + "version") binding them to the registered engine kinds
// (schema::engine_schemas()), so the derivation validate node checks them from their first byte —
// the M1 "schemaVersion" placeholder migrated onto the R-DATA-006 mechanism.
Json default_project_json(const std::string& name)
{
    Json j = Json::object();
    j.set("$schema", Json(std::string(editor::schema::kProjectKindId)));
    j.set("version", Json(std::int64_t{1}));
    j.set("engine", Json("context"));
    j.set("name", Json(name));
    j.set("scene", Json("scenes/main.scene.json"));
    return j;
}

Json default_scene_json()
{
    Json camera = Json::object();
    // The units law (R-DATA-006): authored data is SI + RADIANS everywhere — this is a 60-degree
    // vertical FoV expressed in radians (the scene schema declares fov as x-ctx-units "rad").
    camera.set("fov", Json(1.0471975511965976));
    camera.set("near", Json(0.1));
    camera.set("far", Json(1000.0));

    Json position = Json::array();
    position.push_back(Json(0.0));
    position.push_back(Json(1.0));
    position.push_back(Json(-5.0));
    Json transform = Json::object();
    transform.set("position", std::move(position));

    Json components = Json::object();
    components.set("transform", std::move(transform));
    components.set("camera", std::move(camera));

    Json entity = Json::object();
    entity.set("name", Json("MainCamera"));
    entity.set("components", std::move(components));

    Json entities = Json::array();
    entities.push_back(std::move(entity));

    Json scene = Json::object();
    scene.set("$schema", Json(std::string(editor::schema::kSceneKindId)));
    scene.set("version", Json(std::int64_t{1}));
    scene.set("kind", Json("scene"));
    scene.set("notes",
              Json("Scaffolded by `context new` — human/AI annotations live in schema-blessed "
                   "notes fields (L-32 bans JSON comments)."));
    scene.set("entities", std::move(entities));
    return scene;
}

bool read_file(const std::filesystem::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool write_text_file(const std::filesystem::path& path, const std::string& body)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << body;
    return static_cast<bool>(out);
}

// ------------------------------------------------------- the `extension-panel` template (M9 e13e)

// The `context-ext://` URL prefix a package panel's entry must carry. Mirror 4 of the shell tier's
// vocabulary (`editor::shell::kExtUrlPrefix`) — scaffold.h's MIRROR LIST enumerates all five and
// names where each is pinned. The scheme is the package's ORIGIN, so an entry spelled any other way
// is refused by `read_package_manifest`'s rule (c).
constexpr const char* kExtUrlPrefix = "context-ext://";

bool is_lower_alnum_ascii(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// Does the id's LAST dot-separated label consist entirely of digits? The URL Standard's "ends in a
// number" check hands such a host to the IPv4 parser rather than keeping it a domain, so the id
// could never be named back by a request.
bool last_label_all_digits(const std::string& id)
{
    const std::size_t dot = id.rfind('.');
    const std::string last = dot == std::string::npos ? id : id.substr(dot + 1);
    if (last.empty())
        return false;
    for (char ch : last)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (c < '0' || c > '9')
            return false;
    }
    return true;
}

// The manifest a scaffolded package ships: R-EDIT-001 manifest v2, ONE iframe panel contribution.
// Built as a JSON DOM so it is well-formed by construction, exactly as the default template's two
// files are.
Json extension_manifest_json(const std::string& package_id)
{
    Json dock = Json::object();
    dock.set("zone", Json("right"));
    // One instance, because a second copy of a hello panel says nothing a first does not.
    dock.set("singleton", Json(true));
    dock.set("minWidth", Json(std::int64_t{280}));
    dock.set("minHeight", Json(std::int64_t{160}));

    Json content = Json::object();
    // `iframe` is the ONLY content type a package contribution may declare — `uitree` and `local`
    // both mean "the editor renders this from its own code", which a third-party package does not
    // have, and the store's rule (b) fails closed on anything else.
    content.set("type", Json("iframe"));
    content.set("entry", Json(std::string(kExtUrlPrefix) + package_id + "/" +
                              std::string(kExtensionPanelEntryFileName)));

    Json state = Json::object();
    // The D6 state contract starts at 1; the store refuses 0 or negative.
    state.set("schemaVersion", Json(std::int64_t{1}));

    // The read/query baseline, from the closed manifest vocabulary. DECLARED, never granted — what a
    // package is actually given comes from the install-consent surface, not from this file.
    Json capabilities = Json::array();
    capabilities.push_back(Json("read_query"));

    Json contribution = Json::object();
    // Every contribution id must be NAMESPACED to the package (the package id itself, or a
    // `<package-id>.` prefix), or the store refuses the whole manifest.
    contribution.set("id", Json(package_id + ".panel"));
    contribution.set("kind", Json("panel"));
    contribution.set("title", Json("Hello Panel"));
    contribution.set("icon", Json("puzzle"));
    contribution.set("contractVersion", Json(std::int64_t{kExtensionPanelContractVersion}));
    contribution.set("dock", std::move(dock));
    contribution.set("content", std::move(content));
    contribution.set("state", std::move(state));
    contribution.set("capabilities", std::move(capabilities));

    Json contributions = Json::array();
    contributions.push_back(std::move(contribution));

    Json manifest = Json::object();
    // The manifest's own id must equal the DIRECTORY NAME — the store refuses a disagreement rather
    // than reconciling it, because the directory name is what the `context-ext://` origin will be.
    manifest.set("id", Json(package_id));
    manifest.set("version", Json("0.1.0"));
    manifest.set("contributions", std::move(contributions));
    return manifest;
}

// The hello panel document. NO inline <script> and NO inline style: a panel loads under a strict CSP
// (`script-src 'self'`, `style-src 'self'` — no `'unsafe-inline'` on either), so both would be
// refused and the template would teach a shape that cannot work. The stylesheet and the script are
// this package's OWN same-origin assets, which is what that policy permits.
std::string extension_panel_html(const std::string& package_id)
{
    return "<!doctype html>\n"
           "<html lang=\"en\">\n"
           "  <head>\n"
           "    <meta charset=\"utf-8\" />\n"
           "    <title>Hello Panel</title>\n"
           "    <link rel=\"stylesheet\" href=\"panel.css\" />\n"
           "  </head>\n"
           "  <body>\n"
           "    <main class=\"panel\">\n"
           "      <h1>Hello from " +
           package_id +
           "</h1>\n"
           // `aria-live` because panel.js REPLACES this text as the port connects: without it a
           // screen reader announces the pre-script placeholder and never the outcome. The editor's
           // own webui kit sets the same attribute on its status line.
           "      <p id=\"status\" aria-live=\"polite\">This panel's script has not run yet.</p>\n"
           "    </main>\n"
           "    <script src=\"panel.js\" defer></script>\n"
           "  </body>\n"
           "</html>\n";
}

std::string extension_panel_css()
{
    return "/* The panel's own stylesheet, served from its own origin (the panel CSP is\n"
           "   `style-src 'self'` with no 'unsafe-inline', so a <style> block would be refused). */\n"
           ":root {\n"
           "    /* Declared on the ROOT, which is where it governs the document canvas: the canvas\n"
           "       background and the system colors text resolves against are taken from the ROOT\n"
           "       element's used color scheme, so putting this on `.panel` would leave a light\n"
           "       canvas under text following the host's dark preference. */\n"
           "    color-scheme: light dark;\n"
           "}\n"
           "body {\n"
           "    margin: 0; /* the UA default is 8px; `.panel` owns the padding below */\n"
           "}\n"
           ".panel {\n"
           "    font: 13px/1.5 system-ui, sans-serif;\n"
           "    padding: 12px;\n"
           "}\n"
           ".panel h1 {\n"
           "    font-size: 15px;\n"
           "    margin: 0 0 8px;\n"
           "}\n"
           ".panel p {\n"
           "    margin: 0;\n"
           "    opacity: 0.8;\n"
           "}\n";
}

std::string extension_panel_js(const std::string& package_id)
{
    return "// The panel's own script. The editor injects a port bootstrap as the FIRST script in\n"
           "// every panel document, so by the time this deferred script runs the channel to the\n"
           "// editor is already published on `window.contextPanelPort` -- unless the document was\n"
           "// opened outside the editor, where nothing is minted and the panel must still render.\n"
           "(function () {\n"
           "    \"use strict\";\n"
           "    var status = document.getElementById(\"status\");\n"
           "    var port = window.contextPanelPort;\n"
           "    if (!port) {\n"
           "        status.textContent =\n"
           "            \"Loaded, with no editor port (this document is not framed by the editor).\";\n"
           "        return;\n"
           "    }\n"
           "    port.onmessage = function (event) {\n"
           "        status.textContent = \"Editor said: \" + JSON.stringify(event.data);\n"
           "    };\n"
           "    port.start();\n"
           "    port.postMessage({ type: \"hello\", from: \"" +
           package_id +
           "\" });\n"
           "    status.textContent = \"Loaded, and connected to the editor over the panel port.\";\n"
           "})();\n";
}

// The generated README. It states the THREE steps in the package's own terms, so the budget the
// template exists to meet is documented where the person who ran `context new` will actually see it.
std::string extension_readme(const std::string& package_id)
{
    return "# " + package_id +
           "\n\n"
           "A minimal Context editor package: one panel, rendered in an iframe on this package's own\n"
           "`" +
           std::string(kExtUrlPrefix) + package_id +
           "` origin.\n\n"
           "## Three steps from nothing to a panel\n\n"
           "1. **Scaffold it into the package store.** A package's id IS its directory name, and the\n"
           "   store is `~/.context/packages` (`%USERPROFILE%\\.context\\packages` on Windows), so\n"
           "   scaffolding straight into the store is the install:\n\n"
           "       context new --template extension-panel ~/.context/packages/" +
           package_id +
           "\n\n"
           "2. **Start the editor.** It scans the store at boot, mounts every package it accepts, and\n"
           "   refuses -- by name, never silently -- any it does not.\n\n"
           "3. **Open the panel.** It is contributed as `" +
           package_id +
           ".panel`, docked right.\n\n"
           // One table ROW per line: a markdown table cell may not contain a hard line break, so a
           // wrapped row renders as literal text instead of a table.
           "## What is here\n\n"
           "| File | Why |\n"
           "|---|---|\n"
           "| `" +
           std::string(kExtensionPanelManifestFileName) +
           "` | The manifest. Its `id` must equal this directory's name, and `content.entry` must "
           "name this package's own `" +
           std::string(kExtUrlPrefix) +
           "` origin -- the editor refuses a package that disagrees with either. |\n"
           "| `" +
           std::string(kExtensionPanelEntryFileName) +
           "` | The panel document the manifest's `content.entry` points at. |\n"
           "| `panel.css` | Styling. A separate file because the panel CSP is `style-src 'self'` "
           "with no `'unsafe-inline'`. |\n"
           "| `panel.js` | The panel's script -- likewise a separate file (`script-src 'self'`). It "
           "talks to the editor over `window.contextPanelPort`. |\n\n"
           "## Changing it\n\n"
           "The manifest is what the editor reads; the three assets are yours. Keep `content.entry`\n"
           "pointing at a document that exists, keep every contribution id namespaced to `" +
           package_id +
           "`,\nand keep `content.type` at `iframe` -- the other content types mean \"the editor\n"
           "renders this from its own code\", which a package does not have.\n";
}

// The files the `extension-panel` template writes, in the order the plan and the envelope report
// them. Sorted, so the report is stable and diffable.
std::vector<std::string> extension_panel_files()
{
    return {"README.md", std::string(kExtensionPanelManifestFileName), "panel.css",
            std::string(kExtensionPanelEntryFileName), "panel.js"};
}

// The ONE refusal for a template name `context new` does not offer, and the ONE refusal for a
// directory whose name cannot be a package id. Both are shared by the real scaffold and by
// `scaffold_dry_run`, so a plan can never accept an input the apply refuses.
Envelope refuse_unknown_template(const std::string& template_name)
{
    return Envelope::failure("usage.invalid", "unknown template: " + template_name);
}

Envelope refuse_package_id(const std::string& package_id)
{
    // Leads with the ACTION, the way the CLI's sibling refusals do ("--take must be 'ours' or
    // 'theirs'"): a bare grammar recital leaves the reader to diff their own name against a spec.
    return Envelope::failure(
        "usage.invalid",
        "'" + package_id +
            "' cannot be an editor-package id — rename the target directory. A package's id IS its "
            "directory name; an id is 1-64 bytes of lower-case a-z, 0-9, '.', '-' or '_', must "
            "start and end alphanumeric, may not contain '..', and its last dot-separated label may "
            "not be all digits");
}

// Write the `extension-panel` template into `directory`, then PROVE the result is loadable
// (`verify_extension_package`) before reporting success — the same shape the default template's
// R-QA-006 runnable proof takes, against the property that matters for a package.
Envelope scaffold_extension_package(const std::string& directory)
{
    namespace fs = std::filesystem;
    const std::string package_id = project_basename(directory);

    // FAIL CLOSED, BEFORE ANYTHING IS WRITTEN. A package's id IS its directory name, so a directory
    // the id grammar refuses yields a package the store refuses (`package.id_invalid`) — and a
    // scaffold that emits an unloadable package is worse than no scaffold, because it teaches the
    // wrong shape and the failure surfaces far from its cause.
    if (!is_scaffold_package_id(package_id))
        return refuse_package_id(package_id);

    const fs::path root(directory);
    std::error_code ec;
    // Decides only the WARNING below. The writes truncate either way, exactly as the default
    // template's do — and the generated README tells the reader to re-run this very command as the
    // install step, so REFUSING here would break the flow the template itself teaches.
    const bool overwriting = fs::exists(root / kExtensionPanelManifestFileName, ec) && !ec;
    ec.clear();
    fs::create_directories(root, ec);
    if (ec)
        return Envelope::failure("internal.error",
                                 "could not create the package directory: " + ec.message());

    // A tool save canonicalizes the whole file it writes (R-FILE-001), and `context new` IS a tool
    // save — so the manifest lands in THE canonical form from its first byte, exactly as the default
    // template's project/scene files do.
    const std::string manifest_body =
        editor::serializer::canonicalize(extension_manifest_json(package_id).dump(2)).bytes;

    if (!write_text_file(root / kExtensionPanelManifestFileName, manifest_body) ||
        !write_text_file(root / kExtensionPanelEntryFileName, extension_panel_html(package_id)) ||
        !write_text_file(root / "panel.css", extension_panel_css()) ||
        !write_text_file(root / "panel.js", extension_panel_js(package_id)) ||
        !write_text_file(root / "README.md", extension_readme(package_id)))
    {
        // ABANDON WHAT WAS WRITTEN. `||` short-circuits, so a failure on file N leaves files 1..N-1
        // behind — and the manifest is written FIRST. The store does not open a package's entry
        // document (it validates the entry's URL grammar only), so a half-written package is
        // ACCEPTED at boot and mounts a panel whose document or script 404s: a blank frame with
        // nothing naming why, which is exactly the outcome the id refusal above exists to prevent.
        // The DIRECTORY is left alone — it may have pre-existed, and this call does not own it.
        std::error_code rm_ec;
        for (const std::string& file : extension_panel_files())
            fs::remove(root / file, rm_ec);
        return Envelope::failure("internal.error", "template files failed to write cleanly");
    }

    Envelope loadable = verify_extension_package(directory);
    if (!loadable.ok())
        return loadable;

    Json data = Json::object();
    data.set("directory", Json(directory));
    data.set("template", Json(kExtensionPanelTemplate));
    Json files = Json::array();
    for (const std::string& file : extension_panel_files())
        files.push_back(Json(file));
    data.set("files", std::move(files));
    data.set("packageId", Json(package_id));
    data.set("contributions", loadable.data().at("contributions"));
    data.set("entry", loadable.data().at("entry"));
    data.set("loadable", loadable.data().at("loadable"));
    Envelope out = Envelope::success(std::move(data));
    if (overwriting)
        out.add_warning("a package was already installed in " + directory +
                        "; its manifest and panel assets have been overwritten by the template's. "
                        "Any edits you had made to them are gone.");
    return out;
}

// Auto-install the R-FILE-012 structural merge driver DEFINITION into the project's git config, when
// a repo already exists. L-27: the ENGINE never invokes git — this is a client-side PLAIN-FILE write
// of the `[merge "context"]` stanza git will later invoke (no shell-out, no libgit). Idempotent;
// returns true when the driver is installed (or already present), false when there is no
// .git/config to install into yet (the `.gitattributes` mapping still ships, so the driver activates
// the moment the user `git init`s and re-runs, or adds the stanza by hand).
bool install_merge_driver(const std::filesystem::path& root)
{
    namespace fs = std::filesystem;
    const fs::path config = root / ".git" / "config";
    std::error_code ec;
    if (!fs::exists(config, ec) || ec)
        return false;
    std::string existing;
    if (read_file(config, existing) && existing.find("[merge \"context\"]") != std::string::npos)
        return true; // already installed — idempotent re-run
    std::ofstream out(config, std::ios::binary | std::ios::app);
    if (!out)
        return false;
    out << "\n[merge \"context\"]\n"
        << "\tname = Context structural JSON merge (R-FILE-012)\n"
        << "\tdriver = context merge-file --driver %O %A %B %P\n";
    return static_cast<bool>(out);
}
} // namespace

const std::vector<std::string>& template_names()
{
    static const std::vector<std::string> names = {kDefaultTemplate, kExtensionPanelTemplate};
    return names;
}

bool is_known_template(const std::string& name)
{
    for (const std::string& n : template_names())
        if (n == name)
            return true;
    return false;
}

bool is_scaffold_package_id(const std::string& name)
{
    // The grammar, and each clause's reason, is stated on the declaration (scaffold.h) and on
    // `editor::shell::is_valid_package_id`, which this mirrors and is pinned against.
    if (name.empty() || name.size() > 64)
        return false;
    if (!is_lower_alnum_ascii(static_cast<unsigned char>(name.front())) ||
        !is_lower_alnum_ascii(static_cast<unsigned char>(name.back())))
        return false;
    for (char ch : name)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (is_lower_alnum_ascii(c) || c == '-' || c == '.' || c == '_')
            continue;
        return false;
    }
    if (name.find("..") != std::string::npos)
        return false;
    return !last_label_all_digits(name);
}

Json scaffold_plan(const std::string& directory, const std::string& template_name)
{
    Json files = Json::array();
    if (template_name == kExtensionPanelTemplate)
    {
        for (const std::string& file : extension_panel_files())
            files.push_back(Json(file));
    }
    else
    {
        files.push_back(Json(".gitattributes"));
        files.push_back(Json("project.json"));
        files.push_back(Json("scenes/main.scene.json"));
    }
    Json plan = Json::object();
    plan.set("directory", Json(directory));
    plan.set("template", Json(template_name));
    plan.set("files", std::move(files));
    return plan;
}

Envelope scaffold_dry_run(const std::string& directory, const std::string& template_name)
{
    // The SAME refusals `scaffold_project` applies, in the same order, from the same two helpers —
    // sharing them is the point (see scaffold.h). A plan the apply would reject is a false preview,
    // and this is the surface an agent consults before deciding to write.
    if (!is_known_template(template_name))
        return refuse_unknown_template(template_name);
    if (template_name == kExtensionPanelTemplate)
    {
        const std::string package_id = project_basename(directory);
        if (!is_scaffold_package_id(package_id))
            return refuse_package_id(package_id);
    }
    return Envelope::success(scaffold_plan(directory, template_name));
}

Envelope verify_extension_package(const std::string& directory)
{
    namespace fs = std::filesystem;
    const fs::path root(directory);
    const std::string package_id = project_basename(directory);

    std::string manifest_text;
    if (!read_file(root / kExtensionPanelManifestFileName, manifest_text))
        return Envelope::failure("file.not_found", std::string(kExtensionPanelManifestFileName) +
                                                       " not found under " + directory);
    Json manifest;
    try
    {
        manifest = Json::parse(manifest_text);
    }
    catch (const std::exception& e)
    {
        return Envelope::failure("file.parse_error",
                                 std::string("package manifest parse failed: ") + e.what());
    }
    if (!manifest.is_object())
        return Envelope::failure("file.validation_failed",
                                 "the package manifest's top level is not a JSON object");

    // The store derives the id from the DIRECTORY NAME and refuses a manifest that disagrees, so a
    // scaffold whose two halves disagree would produce a package the editor rejects.
    if (!manifest.contains("id") || !manifest.at("id").is_string() ||
        manifest.at("id").as_string() != package_id)
        return Envelope::failure("file.validation_failed",
                                 "the package manifest's `id` must equal the directory name '" +
                                     package_id + "'");

    if (!manifest.contains("contributions") || !manifest.at("contributions").is_array() ||
        manifest.at("contributions").size() == 0)
        return Envelope::failure("file.validation_failed",
                                 "the package manifest declares no contributions");

    const std::string own_origin = std::string(kExtUrlPrefix) + package_id + "/";
    std::string first_entry;
    const Json& contributions = manifest.at("contributions");
    for (std::size_t i = 0; i < contributions.size(); ++i)
    {
        const Json& contribution = contributions.at(i);
        const std::string at = "contributions[" + std::to_string(i) + "]";
        if (!contribution.is_object() || !contribution.contains("id") ||
            !contribution.at("id").is_string())
            return Envelope::failure("file.validation_failed", at + " carries no `id`");
        // Namespaced to the package: the id must BE the package id or start with `<package-id>.`.
        const std::string id = contribution.at("id").as_string();
        if (id != package_id && id.rfind(package_id + ".", 0) != 0)
            return Envelope::failure("file.validation_failed",
                                     at + "'s id '" + id + "' is not namespaced to '" + package_id +
                                         "'");
        if (!contribution.contains("content") || !contribution.at("content").is_object())
            return Envelope::failure("file.validation_failed", at + " declares no `content`");
        const Json& content = contribution.at("content");
        if (!content.contains("type") || !content.at("type").is_string() ||
            content.at("type").as_string() != "iframe")
            return Envelope::failure("file.validation_failed",
                                     at + "'s content.type must be 'iframe'");
        if (!content.contains("entry") || !content.at("entry").is_string())
            return Envelope::failure("file.validation_failed", at + " declares no content.entry");
        const std::string entry = content.at("entry").as_string();
        if (entry.rfind(own_origin, 0) != 0)
            return Envelope::failure("file.validation_failed",
                                     at + "'s content.entry '" + entry + "' must name this "
                                     "package's own origin (" + own_origin + "...)");
        // THE CHECK THE STORE ITSELF DOES NOT MAKE. `read_package_manifest` validates the entry's
        // URL GRAMMAR — it never opens the file — so a manifest naming a document that does not
        // exist is ACCEPTED there and fails only at load, as an empty panel with nothing saying
        // why. Scaffolding is the one moment both halves are in hand, so it is checked here.
        const std::string relative = entry.substr(own_origin.size());
        // `relative` comes out of the MANIFEST, so on a package this module did not write it is
        // caller-controlled: refuse a traversal rather than existence-probing outside the package.
        // `exists` takes the error_code overload — the throwing one would propagate out of a CLI
        // that installs no handler, and this is a public entry point (scaffold.h).
        std::error_code entry_ec;
        if (relative.empty() || relative.find("..") != std::string::npos ||
            !fs::exists(root / relative, entry_ec) || entry_ec)
            return Envelope::failure("file.not_found",
                                     at + "'s content.entry names '" + relative +
                                         "', which does not exist in the package");
        if (first_entry.empty())
            first_entry = entry;
    }

    Json data = Json::object();
    data.set("directory", Json(directory));
    data.set("packageId", Json(package_id));
    data.set("contributions", Json(static_cast<std::uint64_t>(contributions.size())));
    data.set("entry", Json(first_entry));
    data.set("loadable", Json(true));
    return Envelope::success(std::move(data));
}

Envelope verify_runnable(const std::string& directory)
{
    namespace fs = std::filesystem;
    const fs::path root(directory);

    // Load + parse the project manifest and its scene.
    std::string project_text;
    if (!read_file(root / "project.json", project_text))
        return Envelope::failure("file.not_found", "project.json not found under " + directory);
    std::string scene_text;
    Json project;
    Json scene;
    try
    {
        project = Json::parse(project_text);
        const std::string scene_rel = project.at("scene").as_string();
        if (!read_file(root / scene_rel, scene_text))
            return Envelope::failure("file.not_found", "scene file not found: " + scene_rel);
        scene = Json::parse(scene_text);
    }
    catch (const std::exception& e)
    {
        return Envelope::failure("file.parse_error", std::string("template parse failed: ") +
                                                         e.what());
    }

    if (!scene.at("entities").is_array() || scene.at("entities").size() == 0)
        return Envelope::failure("file.validation_failed", "scene has no entities");

    // Boot a real kernel session and populate the World from the scene — the "startable session".
    kernel::Kernel engine;
    kernel::World& world = engine.world();
    std::size_t cameras_authored = 0;
    const Json& entities = scene.at("entities");
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        const Json& e = entities.at(i);
        const kernel::Entity ent = world.create();
        world.add(ent, Named{e.at("name").as_string()});
        const Json& comps = e.at("components");
        if (comps.contains("transform"))
        {
            const Json& pos = comps.at("transform").at("position");
            world.add(ent, Transform{pos.at(0).as_number(), pos.at(1).as_number(),
                                     pos.at(2).as_number()});
        }
        if (comps.contains("camera"))
        {
            const Json& cam = comps.at("camera");
            world.add(ent, Camera{cam.at("fov").as_number(), cam.at("near").as_number(),
                                  cam.at("far").as_number()});
            ++cameras_authored;
        }
    }

    // The "first query succeeds": a camera query returns the authored camera(s).
    std::size_t cameras_found = 0;
    world.each<Camera>([&](kernel::Entity, Camera&) { ++cameras_found; });
    if (cameras_found == 0)
        return Envelope::failure("file.validation_failed",
                                 "the default template must contain a camera (R-QA-006)");

    // The "first step succeeds": advance the fixed-timestep scheduler once without error.
    int ticks = 0;
    engine.scheduler().run(0.02, [&] { ++ticks; }); // 0.02s @ 60 Hz => one fixed step

    Json data = Json::object();
    data.set("directory", Json(directory));
    data.set("entities", Json(static_cast<std::uint64_t>(world.alive_count())));
    data.set("cameras", Json(static_cast<std::uint64_t>(cameras_found)));
    data.set("camerasAuthored", Json(static_cast<std::uint64_t>(cameras_authored)));
    data.set("ticks", Json(static_cast<std::uint64_t>(ticks)));
    data.set("runnable", Json(ticks >= 1 && cameras_found >= 1));
    return Envelope::success(std::move(data),
                             static_cast<std::uint64_t>(engine.scheduler().tick_count()));
}

Envelope scaffold_project(const std::string& directory, const std::string& template_name)
{
    namespace fs = std::filesystem;
    if (directory.empty())
        return Envelope::failure("usage.missing_argument", "a target directory is required");
    if (!is_known_template(template_name))
        return refuse_unknown_template(template_name);

    if (template_name == kExtensionPanelTemplate)
        return scaffold_extension_package(directory);

    const fs::path root(directory);
    std::error_code ec;
    fs::create_directories(root / "scenes", ec);
    if (ec)
        return Envelope::failure("internal.error",
                                 "could not create project directories: " + ec.message());

    // Tool saves canonicalize the whole file they write (R-FILE-001) — and `context new` IS a
    // tool save, so the template files land in THE canonical form from their very first byte.
    const std::string name = project_basename(directory);
    const std::string project_body =
        editor::serializer::canonicalize(default_project_json(name).dump(2)).bytes;
    const std::string scene_body =
        editor::serializer::canonicalize(default_scene_json().dump(2)).bytes;
    // The template ships a .gitattributes pinning authored JSON to LF/text (R-FILE-001): the
    // canonical form is byte-exact, so checkout EOL rewriting must never touch authored files. It
    // ALSO maps authored JSON to the `context` merge driver (R-FILE-012): parallel-worktree merges
    // are structural by default. The driver DEFINITION is installed into .git/config below (L-27:
    // the engine never invokes git; git invokes the driver).
    const std::string gitattributes_body =
        "# Authored Context files are canonical JSON: UTF-8, LF-only (R-FILE-001).\n"
        "* text=auto\n"
        "*.json text eol=lf\n"
        "# Structural three-way merge for authored JSON (R-FILE-012): git invokes `context\n"
        "# merge-file` as the `context` merge driver (defined in .git/config by `context new`).\n"
        "*.json merge=context\n";

    {
        std::ofstream project_out(root / "project.json", std::ios::binary | std::ios::trunc);
        std::ofstream scene_out(root / "scenes" / "main.scene.json",
                                std::ios::binary | std::ios::trunc);
        std::ofstream attributes_out(root / ".gitattributes",
                                     std::ios::binary | std::ios::trunc);
        if (!project_out || !scene_out || !attributes_out)
            return Envelope::failure("internal.error", "could not open template files for writing");
        project_out << project_body;
        scene_out << scene_body;
        attributes_out << gitattributes_body;
        if (!project_out || !scene_out || !attributes_out)
            return Envelope::failure("internal.error", "template files failed to write cleanly");
    }

    // Auto-install the R-FILE-012 structural merge driver definition (L-27: a plain-file write of the
    // git-config stanza, NOT a git invocation). The .gitattributes mapping already shipped above; this
    // wires the driver git will invoke when the project is a repo.
    const bool merge_driver_installed = install_merge_driver(root);

    // Prove the scaffold is runnable before reporting success (R-QA-006).
    Envelope runnable = verify_runnable(directory);
    if (!runnable.ok())
        return runnable;

    Json data = Json::object();
    data.set("directory", Json(directory));
    data.set("template", Json(template_name));
    Json files = Json::array();
    files.push_back(Json(".gitattributes"));
    files.push_back(Json("project.json"));
    files.push_back(Json("scenes/main.scene.json"));
    data.set("files", std::move(files));
    data.set("runnable", runnable.data().at("runnable"));
    data.set("entities", runnable.data().at("entities"));
    data.set("cameras", runnable.data().at("cameras"));
    data.set("mergeDriverInstalled", Json(merge_driver_installed));
    Envelope out = Envelope::success(std::move(data), runnable.generation_after());
    if (!merge_driver_installed)
        out.add_warning("the R-FILE-012 git merge driver was not installed: no .git/config found. "
                        "Run `git init` then re-run `context new`, or add the [merge \"context\"] "
                        "stanza by hand (the .gitattributes mapping is already in place).");
    return out;
}

} // namespace context::cli
