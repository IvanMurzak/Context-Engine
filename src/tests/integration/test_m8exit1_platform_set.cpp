// M8 exit criterion 1 — `m8-exit-1-platform-set` (ROADMAP §1-M8 exit / a14-m8-exit) — the milestone
// bar "one Project → the full v1 platform-set builds, each produced HEADLESS from the CLI per agent"
// as an executable gate. It RIDES the landed a05 build orchestrator (run_build — the pure, deterministic
// per-agent spine) + the a06/a10/a11/a13 export adapters over ONE in-memory project:
//   * the v1 platform set — Linux DESKTOP + Linux SERVER/headless + Windows + macOS + Web — each builds
//     a REAL pack HEADLESS (no filesystem, no GPU) and reports a SUPPORTED, runnable-artifact adapter
//     plan (the shipped runtime binary + pack + launcher + manifest); the Android/iOS legs are out of
//     the v1 exit set per the platform rulings (Android trailing-SHOULD, iOS in v2), so they are NOT
//     asserted here;
//   * the build is per-AGENT + deterministic (R-BUILD-007 / R-FILE-010): run_build builds exactly the
//     ONE requested target, and the same request re-builds byte-identically;
//   * an unsupported (target, flavor) yields the HONEST stub (supported=false, empty layout), never a
//     faked artifact (R-BUILD-007).
// Design refs: ROADMAP §1-M8 exit, R-BUILD-001, R-BUILD-002, R-BUILD-005, R-BUILD-007, R-FILE-010.
//
// Runs in the blocking "M8 exit gate" build-job step on all three OS legs. All in-process, GPU-free.

#include "m8_exit_test.h"

#include "context/editor/build/adapter.h"
#include "context/editor/build/build_orchestrator.h"
#include "context/editor/build/toolchain_manifest.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/pack/pack_reader.h"
#include "context/editor/serializer/canonical.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace build = context::editor::build;
namespace compose = context::editor::compose;
namespace pack = context::editor::pack;
namespace serializer = context::editor::serializer;

using context::tests::m8::report;

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

// The ONE authored project the milestone exit reads: a minimal single-scene game (a camera + a player),
// the runnable-default template shape with no binary sidecar — a data-only game packs with no transcode.
const char* kProject = R"({
  "$schema": "ctx:scene", "version": 1,
  "entities": [
    {"id": "aaaa0000aaaa0001", "name": "Camera", "components": {"camera": {"fov": 1.0}}},
    {"id": "aaaa0000aaaa0002", "name": "Player", "components": {"transform": {}}}
  ]})";

// One v1-export-set target the milestone exit builds, plus what its adapter must report.
struct TargetExpectation
{
    const char* target;
    const char* flavor;
    bool render_present;
    bool requires_signing;    // windows (Authenticode) + macos (Developer ID + notarization) — a10/a13
    const char* runtime_binary;
};

} // namespace

int main()
{
    MapResolver resolver;
    resolver.add("scenes/main.scene.json", kProject);

    const auto request_for = [&resolver](const char* target, const char* flavor) {
        build::BuildRequest req;
        req.target = target;
        req.flavor = flavor;
        req.resolver = &resolver;
        req.root_scene_path = "scenes/main.scene.json";
        req.toolchain = build::toolchain_manifest();
        req.engine_version = 9;
        return req;
    };

    // === The v1 platform-set (the MUST exit set): Linux desktop + Linux server/headless + Windows +
    //     macOS + Web — each produced HEADLESS from ONE project by the per-agent orchestrator ==========
    // (Android is trailing-SHOULD and iOS is v2 per the platform rulings, so they are deliberately not
    // in this exit set — asserting them would encode a bar the milestone does not claim.)
    const TargetExpectation kV1Set[] = {
        {build::kTargetLinux, build::kFlavorDesktop, /*render*/ true, /*sign*/ false, "context-runtime"},
        {build::kTargetLinux, build::kFlavorServer, /*render*/ false, /*sign*/ false,
         "context-runtime-server"},
        {build::kTargetWindows, build::kFlavorDesktop, /*render*/ true, /*sign*/ true,
         "context-runtime.exe"},
        {build::kTargetWindows, build::kFlavorServer, /*render*/ false, /*sign*/ true,
         "context-runtime-server.exe"},
        {build::kTargetMacos, build::kFlavorDesktop, /*render*/ true, /*sign*/ true, "context-runtime"},
        {build::kTargetMacos, build::kFlavorServer, /*render*/ false, /*sign*/ true,
         "context-runtime-server"},
        // Web is WebGPU-only (render always present) and never server-flavored — only the desktop flavor
        // plans a real adapter; the runtime is the Emscripten .wasm module + its .js loader.
        {build::kTargetWeb, build::kFlavorDesktop, /*render*/ true, /*sign*/ false, "context-runtime.wasm"},
    };

    for (const TargetExpectation& e : kV1Set)
    {
        const build::BuildResult r = build::run_build(request_for(e.target, e.flavor));
        CHECK(r.ok);
        CHECK(r.error_code.empty());
        CHECK(!r.pack_bytes.empty());          // a REAL pack was produced headless (no GPU, no disk)
        CHECK(r.summary.target == e.target);
        CHECK(r.summary.generation != 0);
        CHECK(r.summary.pack_hash != 0);
        CHECK(r.summary.pack_size == r.pack_bytes.size());

        // The a06/a10/a11/a13 adapter reports a SUPPORTED, runnable-artifact plan for the target.
        const build::AdapterPlan& a = r.summary.adapter;
        CHECK(a.supported);                    // never the honest stub for a v1-set target
        CHECK(a.target == e.target);
        CHECK(a.flavor == e.flavor);
        CHECK(a.render_present == e.render_present);
        CHECK(a.requires_signing == e.requires_signing); // windows + macos require code-signing (a10/a13)
        CHECK(a.runtime_binary == e.runtime_binary);
        // The runnable artifact carries the R-BUILD-009 boot inputs: bin/ runtime + content/ pack +
        // launcher + manifest (the web target adds the Emscripten .js loader as a 5th entry).
        const std::size_t expected_layout = (std::string_view(e.target) == build::kTargetWeb) ? 5u : 4u;
        CHECK(a.layout.size() == expected_layout);
        CHECK(!a.launcher_name.empty());
        CHECK(!a.manifest_name.empty());
        CHECK(a.launcher_kind != build::LauncherKind::None);

        // The produced bytes ARE a valid v1 pack (read_pack verifies every chunk's content hash).
        const pack::ParsedPack parsed = pack::read_pack(r.pack_bytes);
        CHECK(parsed.ok);
        CHECK(parsed.engine_version == 9);
    }

    // === Per-agent determinism (R-BUILD-007 / R-FILE-010): the same request re-builds byte-identically ==
    {
        const build::BuildResult a = build::run_build(request_for(build::kTargetLinux, build::kFlavorServer));
        const build::BuildResult b = build::run_build(request_for(build::kTargetLinux, build::kFlavorServer));
        CHECK(a.ok && b.ok);
        CHECK(a.pack_bytes == b.pack_bytes);
        CHECK(a.summary.generation == b.summary.generation);
        CHECK(a.summary.pack_hash == b.summary.pack_hash);
    }

    // === Honest stub (R-BUILD-007): an unsupported target/flavor is NEVER a faked artifact =============
    {
        // A supported target with an unknown flavor: the project still packs, but no adapter plan exists.
        build::BuildResult r = build::run_build(request_for(build::kTargetLinux, "console"));
        CHECK(r.ok);
        CHECK(!r.summary.adapter.supported);
        CHECK(r.summary.adapter.layout.empty());

        // Web in the SERVER flavor is nonsensical (the browser is inherently render-present) → honest stub.
        r = build::run_build(request_for(build::kTargetWeb, build::kFlavorServer));
        CHECK(r.ok);
        CHECK(!r.summary.adapter.supported);
    }

    return report("m8-exit-1-platform-set",
                  "the full v1 platform-set (linux desktop+server, windows, macos, web) builds headless "
                  "per-agent, each a supported runnable-artifact plan");
}
