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

std::string project_basename(const std::string& directory)
{
    std::filesystem::path p(directory);
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
} // namespace

const std::vector<std::string>& template_names()
{
    static const std::vector<std::string> names = {"default"};
    return names;
}

bool is_known_template(const std::string& name)
{
    for (const std::string& n : template_names())
        if (n == name)
            return true;
    return false;
}

Json scaffold_plan(const std::string& directory, const std::string& template_name)
{
    Json files = Json::array();
    files.push_back(Json(".gitattributes"));
    files.push_back(Json("project.json"));
    files.push_back(Json("scenes/main.scene.json"));
    Json plan = Json::object();
    plan.set("directory", Json(directory));
    plan.set("template", Json(template_name));
    plan.set("files", std::move(files));
    return plan;
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
        return Envelope::failure("usage.invalid", "unknown template: " + template_name);

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
    // canonical form is byte-exact, so checkout EOL rewriting must never touch authored files.
    const std::string gitattributes_body =
        "# Authored Context files are canonical JSON: UTF-8, LF-only (R-FILE-001).\n"
        "* text=auto\n"
        "*.json text eol=lf\n";

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
    return Envelope::success(std::move(data), runnable.generation_after());
}

} // namespace context::cli
