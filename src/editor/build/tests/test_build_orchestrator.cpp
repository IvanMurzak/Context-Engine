// Build orchestration core tests (task a05) — the R-QA-011 malformed/failure corpus for the build
// spine. Coverage (R-QA-013 happy + edge + failure):
//   * HAPPY: a data-only project builds a REAL deterministic Linux pack end-to-end; the envelope-shaped
//     summary carries generation + artifact pointers; a repeat build is byte-identical (R-FILE-010).
//   * Each of the FIVE build.* codes is reached from a real request (template_unverified /
//     toolchain_fetch_failed / aot_failed / transcode_failed / link_failed).
//   * The a03 transcode.* detail is wrapped by build.transcode_failed.
//   * The R-KERNEL-003 generated-registration TU names exactly the referenced packages.
//   * DRIFT GUARD: the embedded R-PKG-002 toolchain manifest tracks the L-42 source of truth
//     (cmake/toolchain-versions.json) — the manifest-consumption proof.

#include "context/editor/build/build_errors.h"
#include "context/editor/build/build_orchestrator.h"
#include "context/editor/build/toolchain_manifest.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/import/artifact_kind.h"
#include "context/editor/import/importer.h"
#include "context/editor/pack/pack_reader.h"
#include "context/editor/serializer/canonical.h"

#include "build_test.h"

#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace build = context::editor::build;
namespace compose = context::editor::compose;
namespace import = context::editor::import;
namespace pack = context::editor::pack;
namespace serializer = context::editor::serializer;

namespace
{

class MapResolver final : public compose::SceneResolver
{
public:
    void add(const char* path, const char* json)
    {
        serializer::CanonicalizeResult r = serializer::canonicalize(json);
        CHECK(r.is_json);
        std::optional<compose::SceneDoc> doc = compose::build_scene_doc(path, r.root);
        CHECK(doc.has_value());
        if (doc.has_value())
            docs_[path] = std::move(*doc);
    }
    [[nodiscard]] const compose::SceneDoc* resolve(std::string_view path) const override
    {
        auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, compose::SceneDoc, std::less<>> docs_;
};

// A minimal valid single-scene project: two entities, one carrying a camera (mirrors the runnable
// default template shape without any binary sidecar — a data-only game packs with no transcode).
const char* kValidScene = R"({
  "$schema": "ctx:scene", "version": 1,
  "entities": [
    {"id": "aaaa0000aaaa0001", "name": "Camera", "components": {"camera": {"fov": 1.0}}},
    {"id": "aaaa0000aaaa0002", "name": "Player", "components": {"transform": {}}}
  ]})";

[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

int main()
{
    MapResolver resolver;
    resolver.add("scenes/main.scene.json", kValidScene);

    // A well-formed baseline request: the valid project, target linux, the embedded toolchain manifest,
    // no scripts / sidecars / referenced packages (a data-only game).
    const auto base_request = [&resolver]() {
        build::BuildRequest req;
        req.target = "linux";
        req.resolver = &resolver;
        req.root_scene_path = "scenes/main.scene.json";
        req.toolchain = build::toolchain_manifest();
        req.engine_version = 7;
        return req;
    };

    // --- HAPPY PATH: a real deterministic Linux pack end-to-end (DoD 1) ------------------------------
    {
        const build::BuildResult r = build::run_build(base_request());
        CHECK(r.ok);
        CHECK(r.error_code.empty());
        CHECK(!r.pack_bytes.empty());
        // The envelope-shaped summary carries generation + artifact pointers.
        CHECK(r.summary.target == "linux");
        CHECK(r.summary.engine_version == 7);
        CHECK(r.summary.generation != 0);
        CHECK(r.summary.pack_hash != 0);
        CHECK(r.summary.pack_size == r.pack_bytes.size());
        CHECK(r.summary.entity_count == 2);
        CHECK(r.summary.unit_count >= 1);
        CHECK(r.summary.sidecar_count == 0);
        // a06: the Linux adapter is REAL now (no longer a stub). The default flavor is desktop (render
        // subsystem present); the plan describes the runnable-artifact tarball layout.
        CHECK(r.summary.adapter.supported);
        CHECK(r.summary.adapter.target == "linux");
        CHECK(r.summary.adapter.flavor == "desktop");
        CHECK(r.summary.adapter.render_present);
        CHECK(r.summary.adapter.runtime_binary == "context-runtime");
        CHECK(r.summary.adapter.layout.size() == 4); // bin/ + content/ + launcher + manifest

        // The produced bytes ARE a valid pack (read_pack verifies every chunk's content hash).
        const pack::ParsedPack parsed = pack::read_pack(r.pack_bytes);
        CHECK(parsed.ok);
        CHECK(parsed.engine_version == 7);

        // Reproducible: the same request re-builds byte-identically (the R-FILE-010 cache property).
        const build::BuildResult again = build::run_build(base_request());
        CHECK(again.ok);
        CHECK(again.pack_bytes == r.pack_bytes);
        CHECK(again.summary.generation == r.summary.generation);
    }

    // --- a06 adapter: the server/headless flavor omits render (L-5 DCE) -----------------------------
    {
        build::BuildRequest req = base_request();
        req.flavor = "server";
        const build::BuildResult r = build::run_build(req);
        CHECK(r.ok);
        CHECK(r.summary.adapter.supported);
        CHECK(r.summary.adapter.flavor == "server");
        CHECK(!r.summary.adapter.render_present);
        CHECK(r.summary.adapter.runtime_binary == "context-runtime-server");
    }

    // --- a10 adapter: windows is now a REAL adapter (.exe binary + Authenticode signing required) -----
    {
        build::BuildRequest req = base_request();
        req.target = "windows";
        req.flavor = "desktop";
        const build::BuildResult r = build::run_build(req);
        CHECK(r.ok);
        CHECK(r.summary.adapter.supported);
        CHECK(r.summary.adapter.render_present);
        CHECK(r.summary.adapter.requires_signing); // windows requires Authenticode (a10, R-SEC-003)
        CHECK(r.summary.adapter.runtime_binary == "context-runtime.exe");
    }

    // --- adapter: a target with no real adapter yet keeps the HONEST stub (R-BUILD-007) --------------
    {
        // macos has a toolchain entry + no sidecars, so it builds a pack — but no macos adapter has
        // landed yet (a13 scope), so the plan is unsupported (supported=false), never a faked artifact.
        build::BuildRequest req = base_request();
        req.target = "macos";
        const build::BuildResult r = build::run_build(req);
        CHECK(r.ok);
        CHECK(!r.summary.adapter.supported);
        CHECK(r.summary.adapter.layout.empty());
    }

    // --- a06 adapter: an unknown flavor on linux is not a supported adapter (honest stub) -----------
    {
        build::BuildRequest req = base_request();
        req.flavor = "console";
        const build::BuildResult r = build::run_build(req);
        CHECK(r.ok);
        CHECK(!r.summary.adapter.supported);
    }

    // --- build.template_unverified: a missing / malformed / empty project ---------------------------
    {
        // No resolver at all.
        build::BuildRequest req = base_request();
        req.resolver = nullptr;
        const build::BuildResult r = build::run_build(req);
        CHECK(!r.ok);
        CHECK(r.error_code == build::kBuildTemplateUnverifiedCode);
    }
    {
        // A root scene that resolves to nothing (empty composed world).
        build::BuildRequest req = base_request();
        req.root_scene_path = "scenes/does-not-exist.scene.json";
        const build::BuildResult r = build::run_build(req);
        CHECK(!r.ok);
        CHECK(r.error_code == build::kBuildTemplateUnverifiedCode);
        CHECK(r.error_pointer == "scenes/does-not-exist.scene.json");
    }

    // --- build.toolchain_fetch_failed: no manifest entry for the target (R-PKG-002) -----------------
    {
        // An unknown build-target id maps to no manifest triple.
        build::BuildRequest req = base_request();
        req.target = "playstation";
        const build::BuildResult r = build::run_build(req);
        CHECK(!r.ok);
        CHECK(r.error_code == build::kBuildToolchainFetchFailedCode);
        CHECK(r.error_pointer == "playstation");
    }
    {
        // A KNOWN target, but the supplied manifest is incomplete (the corpus's malformed manifest).
        build::BuildRequest req = base_request();
        req.toolchain.clear();
        const build::BuildResult r = build::run_build(req);
        CHECK(!r.ok);
        CHECK(r.error_code == build::kBuildToolchainFetchFailedCode);
    }

    // --- build.aot_failed: a malformed authored-script entrypoint -----------------------------------
    {
        build::BuildRequest req = base_request();
        req.scripts.push_back({"player_controller", ""}); // no entrypoint
        const build::BuildResult r = build::run_build(req);
        CHECK(!r.ok);
        CHECK(r.error_code == build::kBuildAotFailedCode);
        CHECK(r.error_pointer == "player_controller");
    }
    {
        build::BuildRequest req = base_request();
        req.scripts.push_back({"enemy_ai", "scripts/enemy.js"}); // not a .ts entrypoint
        const build::BuildResult r = build::run_build(req);
        CHECK(!r.ok);
        CHECK(r.error_code == build::kBuildAotFailedCode);
    }
    {
        // A well-formed .ts entrypoint passes the AOT gate (the compile itself is the a06+ follow-up).
        build::BuildRequest req = base_request();
        req.scripts.push_back({"player_controller", "scripts/player.ts"});
        const build::BuildResult r = build::run_build(req);
        CHECK(r.ok);
    }

    // --- build.transcode_failed: a sidecar transcode node fails (wraps the a03 transcode.* detail) ---
    {
        build::BuildArtifact art;
        art.relpath = "textures/hero.png.bin";
        art.raw_hash = 0x1234ULL;
        art.common_bytes = "raw";
        // A texture DESCRIPTOR artifact whose bytes are not well-formed JSON → transcode.bad_descriptor.
        art.artifact.kind = import::ArtifactKind::texture;
        art.artifact.name = "texture";
        art.artifact.bytes = "not-a-descriptor";
        build::BuildRequest req = base_request();
        req.artifacts.push_back(art);
        const build::BuildResult r = build::run_build(req);
        CHECK(!r.ok);
        CHECK(r.error_code == build::kBuildTranscodeFailedCode);
        CHECK(r.error_pointer == "textures/hero.png.bin");
        CHECK(contains(r.error_message, "transcode.bad_descriptor")); // the a03 detail is surfaced
    }

    // --- build.link_failed: a referenced package with no registrable module (R-KERNEL-003) ----------
    {
        build::BuildRequest req = base_request();
        req.referenced_packages = {"physics3d", "ghostpkg"};
        req.registrable_packages = {"physics3d", "render"};
        const build::BuildResult r = build::run_build(req);
        CHECK(!r.ok);
        CHECK(r.error_code == build::kBuildLinkFailedCode);
        CHECK(r.error_pointer == "ghostpkg");
    }
    {
        // A referenced set fully covered by registrable modules links; the generated TU names them.
        build::BuildRequest req = base_request();
        req.referenced_packages = {"physics3d"};
        req.registrable_packages = {"physics3d", "render"};
        const build::BuildResult r = build::run_build(req);
        CHECK(r.ok);
        CHECK(r.summary.registered_packages.size() == 1);
        CHECK(r.summary.registered_packages[0] == "physics3d");
        CHECK(contains(r.summary.registration_tu, "register_physics3d(kernel);"));
    }

    // --- DRIFT GUARD: the embedded toolchain manifest tracks the L-42 source of truth ---------------
    // Reads the real cmake/toolchain-versions.json (R-PKG-002 consumption proof): every embedded target
    // key + compiler id must appear in the file, and the four known targets are present.
    {
        std::ifstream in(CONTEXT_TOOLCHAIN_MANIFEST, std::ios::binary);
        CHECK(in.good());
        std::ostringstream buf;
        buf << in.rdbuf();
        const std::string json = buf.str();
        CHECK(!json.empty());

        const std::vector<build::ToolchainEntry>& manifest = build::toolchain_manifest();
        CHECK(manifest.size() == 4);
        for (const build::ToolchainEntry& e : manifest)
        {
            CHECK(contains(json, "\"" + e.target + "\""));   // the manifest triple key is present
            CHECK(contains(json, "\"" + e.compiler + "\"")); // its compiler id is present
        }
        // The build-target → triple mapping resolves each shipped platform.
        CHECK(build::toolchain_target_key("linux") == "linux-x86_64");
        CHECK(build::toolchain_target_key("web") == "web-emscripten");
        CHECK(build::toolchain_target_key("nope").empty());
    }

    BUILD_TEST_MAIN_END();
}
