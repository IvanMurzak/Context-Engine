// `context build` (headless per-agent build) — see build_command.h.

#include "context/cli/build_command.h"

#include "context/common/subprocess.h"
#include "context/editor/build/adapter.h"
#include "context/editor/build/build_errors.h"
#include "context/editor/build/build_orchestrator.h"
#include "context/editor/build/toolchain_manifest.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/contract/json.h"
#include "context/editor/filesync/native_file_store.h"
#include "context/editor/filesync/path_jail.h"
#include "context/editor/pkg/tar.h"
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
        const auto inserted = cache_.emplace(key, std::move(entry)).first;
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

namespace pkg = editor::pkg;
namespace subprocess = context::common::subprocess;

// The machine-readable adapter plan (a06). supported=false is the honest stub for a target with no real
// adapter yet (R-BUILD-007); a supported plan carries the flavor, render presence, runtime binary, the
// deterministic-modulo-link property, and the documented tarball layout.
[[nodiscard]] Json adapter_json(const build::AdapterPlan& plan)
{
    Json a = Json::object();
    a.set("supported", Json(plan.supported));
    a.set("stub", Json(!plan.supported));
    a.set("target", Json(plan.target));
    a.set("flavor", Json(plan.flavor));
    if (!plan.supported)
        return a;
    a.set("renderPresent", Json(plan.render_present));
    a.set("runtimeBinary", Json(plan.runtime_binary));
    a.set("deterministicModuloLink", Json(true));
    Json layout = Json::array();
    for (const build::ArtifactEntry& e : plan.layout)
    {
        Json entry = Json::object();
        entry.set("path", Json(e.archive_path));
        entry.set("role", Json(e.role));
        layout.push_back(std::move(entry));
    }
    a.set("layout", std::move(layout));
    return a;
}

// Assemble the runnable artifact tarball (bin/<runtime> + content/<pack> + launch.sh + manifest) via
// the deterministic ustar writer (pkg::tar_write). Reports emitted true/false + a reason — never a
// silent skip. NB: tar_write writes mode 0644; a consumer extracting the tarball must `chmod +x` the
// runtime binary + launcher before running (documented in the manifest / README — the exec-bit is a
// tracked tar_write follow-up, like gzip).
[[nodiscard]] Json emit_artifact(const build::AdapterPlan& plan, const std::string& pack_bytes,
                                 std::uint64_t engine_version, std::uint64_t generation,
                                 std::uint64_t pack_hash, const std::optional<std::string>& runtime,
                                 const std::string& out_tar)
{
    Json e = Json::object();
    e.set("path", Json(out_tar));
    if (!plan.supported)
    {
        e.set("emitted", Json(false));
        e.set("reason", Json(std::string("no export adapter for this target/flavor")));
        return e;
    }
    if (!runtime.has_value() || runtime->empty())
    {
        e.set("emitted", Json(false));
        e.set("reason", Json(std::string("--emit-artifact requires --runtime <host-binary>")));
        return e;
    }
    std::string runtime_bytes;
    if (!read_file(fs::path(*runtime), runtime_bytes))
    {
        e.set("emitted", Json(false));
        e.set("reason", Json("could not read --runtime binary: " + *runtime));
        return e;
    }
    std::vector<pkg::TarEntry> entries;
    entries.push_back({"bin/" + plan.runtime_binary, runtime_bytes, false});
    entries.push_back({"content/" + plan.pack_name, pack_bytes, false});
    entries.push_back({plan.launcher_name, build::render_launcher(plan), false});
    entries.push_back(
        {plan.manifest_name, build::render_manifest(plan, engine_version, generation, pack_hash),
         false});
    const std::optional<std::string> tar = pkg::tar_write(entries);
    if (!tar.has_value())
    {
        e.set("emitted", Json(false));
        e.set("reason", Json(std::string("tar assembly failed (a path exceeded the ustar limit)")));
        return e;
    }
    std::error_code ec;
    const fs::path out_fs(out_tar);
    if (out_fs.has_parent_path())
        fs::create_directories(out_fs.parent_path(), ec);
    std::ofstream os(out_fs, std::ios::binary | std::ios::trunc);
    if (!os || !os.write(tar->data(), static_cast<std::streamsize>(tar->size())))
    {
        e.set("emitted", Json(false));
        e.set("reason", Json("could not write artifact tarball: " + out_tar));
        return e;
    }
    e.set("emitted", Json(true));
    e.set("entryCount", Json(static_cast<std::uint64_t>(entries.size())));
    return e;
}

// Launch the produced artifact against the shipped runtime binary (R-BUILD-009) and fold its boot/state
// signal into the envelope. A target that cannot be smoke-run declares ran=false + a reason (never a
// silent skip). The child's stdout is captured through a scratch file (the subprocess runner returns
// only the exit code); its JSON signal is parsed and surfaced.
[[nodiscard]] Json run_smoke(const build::AdapterPlan& plan, const std::optional<std::string>& runtime,
                             const std::string& pack_path, bool dry_run, int ticks)
{
    Json sm = Json::object();
    const auto not_run = [&sm](const std::string& reason) {
        sm.set("ran", Json(false));
        sm.set("reason", Json(reason));
        return sm;
    };
    if (!plan.supported)
        return not_run("no export adapter for this target/flavor");
    if (!runtime.has_value() || runtime->empty())
        return not_run("--smoke requires --runtime <host-binary>");
    if (dry_run)
        return not_run("--dry-run wrote no pack to smoke-run");
    if (!fs::exists(fs::path(pack_path)))
        return not_run("the built pack is not present at " + pack_path);

    const fs::path out = subprocess::make_scratch_path("ctx-build-smoke", ".json");
    subprocess::ScratchFile out_guard(out);
    std::string cmd;
    try
    {
        cmd = subprocess::quote_argument(*runtime) + " --pack " +
              subprocess::quote_argument(pack_path) + " --ticks " + std::to_string(ticks) + " > " +
              subprocess::quote_argument(out.string());
    }
    catch (const subprocess::MetacharacterError& ex)
    {
        return not_run(std::string("could not build a safe launch command: ") + ex.what());
    }
    const int exit_code = subprocess::run_command(cmd);
    const std::string signal = subprocess::read_file(out);

    sm.set("ran", Json(true));
    sm.set("ticks", Json(static_cast<std::uint64_t>(ticks < 0 ? 0 : ticks)));
    sm.set("exitCode", Json(static_cast<std::int64_t>(exit_code)));
    // Parse the runtime's boot/state signal (best-effort — a launch failure yields empty stdout).
    try
    {
        const Json parsed = Json::parse(signal);
        sm.set("ok", Json(exit_code == 0 && parsed.contains("ok") && parsed.at("ok").as_bool()));
        if (parsed.contains("simTick"))
            sm.set("simTick", Json(static_cast<std::uint64_t>(parsed.at("simTick").as_int())));
        if (parsed.contains("rootScene"))
            sm.set("rootScene", Json(parsed.at("rootScene").as_string()));
        if (parsed.contains("flavor"))
            sm.set("flavor", Json(parsed.at("flavor").as_string()));
        if (parsed.contains("renderPresent"))
            sm.set("renderPresent", Json(parsed.at("renderPresent").as_bool()));
    }
    catch (const std::exception&)
    {
        // The runtime emitted no parseable signal (e.g. it could not be launched on this host).
        sm.set("ok", Json(false));
        sm.set("reason", Json(std::string("the runtime emitted no parseable boot/state signal")));
    }
    return sm;
}

} // namespace

Envelope run_build(const std::map<std::string, std::string>& flags)
{
    const std::string target = flag(flags, "target").value_or("");
    if (target.empty())
        return Envelope::failure("usage.missing_argument",
                                 "context build requires --target <windows|linux|macos|web>");
    const std::string project = flag(flags, "project").value_or(".");
    const bool dry_run = flags.find("dry-run") != flags.end();

    // The a06 export flavor (desktop = render present; server = headless, render absent). Default
    // desktop. A malformed value is a usage error, not a silent fallback (R-CLI-007).
    const std::string flavor = flag(flags, "flavor").value_or(build::kFlavorDesktop);
    if (!build::is_known_flavor(flavor))
        return Envelope::failure("usage.invalid",
                                 "unknown --flavor '" + flavor + "' (expected desktop | server)",
                                 flavor);

    // Read the project manifest → the root scene, plus the OPTIONAL scripts / packages declarations the
    // build consumes (absent ⇒ a data-only game: no AOT tier, no linked packages). A missing/malformed
    // manifest is a pre-build template failure.
    std::string manifest_text;
    if (!read_file(fs::path(project) / "project.json", manifest_text))
        return Envelope::failure(std::string(build::kBuildTemplateUnverifiedCode),
                                 "project.json not found under " + project, "project.json");

    build::BuildRequest req;
    req.target = target;
    req.flavor = flavor;
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

    // The a06 export adapter (Linux desktop + server/headless are real; other targets report the honest
    // stub — R-BUILD-007). The plan is machine-readable in the envelope: an autonomous agent shipping
    // multiple platforms reads whether a runnable artifact is available for (target, flavor).
    data.set("adapter", adapter_json(s.adapter));

    // --emit-artifact: assemble the runnable artifact tarball (R-BUILD-005 minimal packaging) — the
    // shipped runtime binary (--runtime) + this pack + launch.sh + the manifest. Reported machine-
    // readably (emitted true/false + reason), never a silent skip.
    const std::optional<std::string> emit_path = flag(flags, "emit-artifact");
    if (emit_path.has_value())
        data.set("artifactEmit",
                 emit_artifact(s.adapter, result.pack_bytes, s.engine_version, s.generation,
                               s.pack_hash, flag(flags, "runtime"), *emit_path));

    // --smoke (R-BUILD-009): launch the produced artifact against the shipped runtime binary, step N
    // fixed ticks, and fold the boot/state signal into the envelope. A target that cannot be smoke-run
    // (no adapter, no --runtime, or nothing written) declares that machine-readably rather than skipping.
    if (flags.find("smoke") != flags.end())
    {
        int smoke_ticks = 8;
        if (const std::optional<std::string> t = flag(flags, "smoke-ticks"); t.has_value())
        {
            try
            {
                smoke_ticks = std::stoi(*t);
            }
            catch (const std::exception&)
            {
                return Envelope::failure("usage.invalid",
                                         "--smoke-ticks must be a non-negative integer", *t);
            }
        }
        data.set("smoke", run_smoke(s.adapter, flag(flags, "runtime"), out_path, dry_run, smoke_ticks));
    }

    // A build mutates no authored file, so the envelope's generationAfter (the derived-world generation
    // a WRITE lands in) is 0; the build's own content generation is reported in data above.
    return Envelope::success(std::move(data), 0);
}

} // namespace context::cli
