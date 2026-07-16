// `context build` (headless per-agent build) — see build_command.h.

#include "context/cli/build_command.h"

#include "context/editor/build/build_errors.h"
#include "context/editor/build/build_orchestrator.h"
#include "context/editor/build/toolchain_manifest.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/contract/json.h"
#include "context/editor/filesync/native_file_store.h"
#include "context/editor/filesync/path_jail.h"
#include "context/editor/serializer/canonical.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace context::cli
{

using editor::contract::Envelope;
using editor::contract::Json;
namespace build = editor::build;
namespace compose = editor::compose;
namespace filesync = editor::filesync;
namespace serializer = editor::serializer;
namespace fs = std::filesystem;

namespace
{

[[nodiscard]] std::optional<std::string> flag(const std::map<std::string, std::string>& flags,
                                              const std::string& name)
{
    const auto it = flags.find(name);
    if (it == flags.end())
        return std::nullopt;
    return it->second;
}

[[nodiscard]] bool read_file(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Disk-backed, resolve-only scene resolver: reads + canonicalizes + composition-parses scene files
// under the project root on demand (the R-SEC-008 jail enforced lexically; the native FileStore
// re-enforces it). The resolve-only sibling of set_command's ProjectResolver — the build never writes
// authored files, so it needs no WriteResolver tree() side.
class DiskSceneResolver final : public compose::SceneResolver
{
public:
    explicit DiskSceneResolver(fs::path root) : store_(std::move(root)) {}

    [[nodiscard]] const compose::SceneDoc* resolve(std::string_view path) const override
    {
        const std::string key(path);
        if (const auto it = cache_.find(key); it != cache_.end())
            return it->second.has_doc ? &it->second.doc : nullptr;
        Entry entry;
        if (filesync::is_inside_jail(".", key))
        {
            if (const std::optional<std::string> bytes = store_.read(key))
            {
                serializer::CanonicalizeResult canonical = serializer::canonicalize(*bytes);
                if (canonical.is_json)
                {
                    if (std::optional<compose::SceneDoc> doc =
                            compose::build_scene_doc(key, canonical.root))
                    {
                        entry.has_doc = true;
                        entry.doc = std::move(*doc);
                    }
                }
            }
        }
        const auto [inserted, _] = cache_.emplace(key, std::move(entry));
        return inserted->second.has_doc ? &inserted->second.doc : nullptr;
    }

private:
    struct Entry
    {
        bool has_doc = false;
        compose::SceneDoc doc;
    };
    filesync::NativeFileStore store_;
    mutable std::map<std::string, Entry, std::less<>> cache_;
};

} // namespace

Envelope run_build(const std::map<std::string, std::string>& flags)
{
    const std::string target = flag(flags, "target").value_or("");
    if (target.empty())
        return Envelope::failure("usage.missing_argument",
                                 "context build requires --target <windows|linux|macos|web>");
    const std::string project = flag(flags, "project").value_or(".");
    const bool dry_run = flags.find("dry-run") != flags.end();

    // Read the project manifest → the root scene, plus the OPTIONAL scripts / packages declarations the
    // build consumes (absent ⇒ a data-only game: no AOT tier, no linked packages). A missing/malformed
    // manifest is a pre-build template failure.
    std::string manifest_text;
    if (!read_file(fs::path(project) / "project.json", manifest_text))
        return Envelope::failure(std::string(build::kBuildTemplateUnverifiedCode),
                                 "project.json not found under " + project, "project.json");

    build::BuildRequest req;
    req.target = target;
    req.toolchain = build::toolchain_manifest();
    DiskSceneResolver resolver(project);
    req.resolver = &resolver;
    try
    {
        const Json manifest = Json::parse(manifest_text);
        req.root_scene_path = manifest.at("scene").as_string();
        if (manifest.contains("scripts") && manifest.at("scripts").is_array())
        {
            const Json& scripts = manifest.at("scripts");
            for (std::size_t i = 0; i < scripts.size(); ++i)
            {
                const Json& s = scripts.at(i);
                build::BuildScript bs;
                bs.name = s.contains("name") ? s.at("name").as_string() : std::string();
                bs.entrypoint = s.contains("entrypoint") ? s.at("entrypoint").as_string()
                                                         : std::string();
                req.scripts.push_back(std::move(bs));
            }
        }
        if (manifest.contains("packages") && manifest.at("packages").is_array())
        {
            const Json& pkgs = manifest.at("packages");
            for (std::size_t i = 0; i < pkgs.size(); ++i)
                req.referenced_packages.push_back(pkgs.at(i).as_string());
        }
    }
    catch (const std::exception& e)
    {
        return Envelope::failure(std::string(build::kBuildTemplateUnverifiedCode),
                                 std::string("project.json is malformed: ") + e.what(),
                                 "project.json");
    }

    // The engine's registrable first-party package set (packages with a register_<pkg> Module —
    // src/packages/). A manifest referencing anything outside this set is an undefined register_<pkg>
    // (build.link_failed). The default template references none, so its link is a no-op.
    req.registrable_packages = {"spatial",   "simmath",  "physics3d", "physics2d", "particles",
                                "animation", "spline",   "audio",     "input",     "ui"};

    const build::BuildResult result = build::run_build(req);
    if (!result.ok)
    {
        std::optional<std::string> pointer;
        if (!result.error_pointer.empty())
            pointer = result.error_pointer;
        return Envelope::failure(result.error_code, result.error_message, pointer);
    }

    const std::string out_path =
        flag(flags, "out")
            .value_or((fs::path(project) / "build" / (target + std::string(".pack"))).generic_string());

    if (!dry_run)
    {
        const fs::path out_fs(out_path);
        std::error_code ec;
        if (out_fs.has_parent_path())
            fs::create_directories(out_fs.parent_path(), ec);
        std::ofstream os(out_fs, std::ios::binary | std::ios::trunc);
        if (!os)
            return Envelope::failure("internal.error", "could not open output pack path: " + out_path,
                                     out_path);
        os.write(result.pack_bytes.data(), static_cast<std::streamsize>(result.pack_bytes.size()));
        if (!os)
            return Envelope::failure("internal.error", "could not write output pack: " + out_path,
                                     out_path);
    }

    const build::BuildSummary& s = result.summary;
    Json data = Json::object();
    data.set("target", Json(s.target));
    data.set("dryRun", Json(dry_run));
    data.set("engineVersion", Json(static_cast<std::uint64_t>(s.engine_version)));
    // The build's derived-world generation — a deterministic 64-bit content identity of the world this
    // pack was built from (reproducible: the same project always yields the same generation). Reported
    // as a DECIMAL STRING because the R-CLI-008 envelope's JSON numbers are doubles, which would lose
    // precision above 2^53 (the same reason $sidecar hashes / L-33 ids are strings).
    data.set("generation", Json(std::to_string(s.generation)));

    Json artifact = Json::object();
    artifact.set("packPath", Json(out_path));
    artifact.set("written", Json(!dry_run));
    artifact.set("packHash", Json(std::to_string(s.pack_hash))); // 64-bit fingerprint — decimal string
    artifact.set("packSize", Json(static_cast<std::uint64_t>(s.pack_size)));
    artifact.set("unitCount", Json(static_cast<std::uint64_t>(s.unit_count)));
    artifact.set("chunkCount", Json(static_cast<std::uint64_t>(s.chunk_count)));
    artifact.set("sidecarCount", Json(static_cast<std::uint64_t>(s.sidecar_count)));
    artifact.set("entityCount", Json(static_cast<std::uint64_t>(s.entity_count)));
    data.set("artifact", std::move(artifact));

    Json registered = Json::array();
    for (const std::string& p : s.registered_packages)
        registered.push_back(Json(p));
    data.set("registeredPackages", std::move(registered));
    // The platform adapter is a stub until a06 — reported honestly, never a silent fake (R-BUILD-007).
    data.set("adapter", Json(std::string("stub")));

    // A build mutates no authored file, so the envelope's generationAfter (the derived-world generation
    // a WRITE lands in) is 0; the build's own content generation is reported in data above.
    return Envelope::success(std::move(data), 0);
}

} // namespace context::cli
