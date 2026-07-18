// M8 exit criterion 5 — `m8-exit-5-seam-checklist` (ROADMAP §1-M8 exit / a14-m8-exit) — the M8
// build-pipeline SEAM CHECKLIST as an executable audit, the milestone-closing mirror of m2-exit-6 /
// m5-exit-3 / m6-exit-5 / m7-exit-5: ONE assertion per M8 seam, exercising the real public surface, so a
// regression that quietly drops a seam turns this milestone gate red. The seams:
//
//   1  per-agent build orchestration (a05, R-BUILD-002/007) — run_build is deterministic (byte-identical
//      re-build, R-FILE-010) AND fails CLOSED with a build.* code on a malformed request (never throws)
//   2  full v1 export adapter set (a06/a10/a11/a13, R-BUILD-001/005) — linux/windows/macos (desktop +
//      server) + web(desktop) plan a supported adapter; an unknown target/flavor is the HONEST stub
//   3  desktop code-signing MUST split (a10 Authenticode + a13 Developer-ID/notarization, R-SEC-003) —
//      each never-silent on an unsigned required target; linux/web not-required
//   4  verify-before-use fails CLOSED under the a08 pinned trust root (R-SEC-009 / L-58)
//   5  pack format v1 frozen (R-ASSET-005) — the orchestrator's pack reads back as a valid v1 pack
//   6  the R-BUILD-009 headless-smoke inputs are complete — the runnable-artifact layout is a shipped
//      runtime binary + a content pack + a launcher + a manifest (what the packed boot consumes)
//   7  wedge determinism (L-54 / R-QA-005) — the shipped Session is bit-reproducible from (seed +
//      scenario); the packed-vs-shipped state-hash + L-48/R-NET-001 replication are the m8-exit-4 siblings
//   8  OPS1 HONESTY (R-BUILD-006 / R-QA-012) — the committed build-time budget is declared
//      advisory-until-provisioned (the perf-isolated runner is unprovisioned), never a blocking number
//
// Runs in the blocking "M8 exit gate" build-job step on all three OS legs. All in-process, GPU-free.

#include "m8_exit_test.h"

#include "context/common/subprocess.h"
#include "context/common/verify_signature.h"
#include "context/editor/build/adapter.h"
#include "context/editor/build/build_errors.h"
#include "context/editor/build/build_orchestrator.h"
#include "context/editor/build/signing.h"
#include "context/editor/build/toolchain_manifest.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/pack/pack_reader.h"
#include "context/editor/serializer/canonical.h"

#include "context/runtime/session/session.h"
#include "context/runtime/session/state_hash.h"

#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace build = context::editor::build;
namespace compose = context::editor::compose;
namespace pack = context::editor::pack;
namespace serializer = context::editor::serializer;
namespace session = context::runtime::session;
namespace common = context::common;
namespace sp = context::common::subprocess;

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

const char* kProject = R"({
  "$schema": "ctx:scene", "version": 1,
  "entities": [
    {"id": "aaaa0000aaaa0001", "name": "Camera", "components": {"camera": {"fov": 1.0}}},
    {"id": "aaaa0000aaaa0002", "name": "Player", "components": {"transform": {}}}
  ]})";

[[nodiscard]] std::string read_file_text(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.good())
        return {};
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

int main()
{
    MapResolver resolver;
    resolver.add("scenes/main.scene.json", kProject);
    const auto linux_request = [&resolver]() {
        build::BuildRequest req;
        req.target = build::kTargetLinux;
        req.flavor = build::kFlavorServer;
        req.resolver = &resolver;
        req.root_scene_path = "scenes/main.scene.json";
        req.toolchain = build::toolchain_manifest();
        req.engine_version = 9;
        return req;
    };

    // === Seam 1 — per-agent build orchestration (a05): deterministic + fail-closed ====================
    {
        const build::BuildResult a = build::run_build(linux_request());
        const build::BuildResult b = build::run_build(linux_request());
        CHECK(a.ok && b.ok);
        CHECK(a.pack_bytes == b.pack_bytes);              // byte-identical re-build (R-FILE-010)
        CHECK(a.summary.generation == b.summary.generation);

        // Fails CLOSED with a build.* code (never throws) on a malformed request.
        build::BuildRequest bad = linux_request();
        bad.target = "playstation"; // no toolchain manifest triple
        const build::BuildResult r = build::run_build(bad);
        CHECK(!r.ok);
        CHECK(r.error_code == build::kBuildToolchainFetchFailedCode);
    }

    // === Seam 2 — the full v1 export adapter set + honest stub (a06/a10/a11/a13) =======================
    {
        for (const char* t : {build::kTargetLinux, build::kTargetWindows, build::kTargetMacos})
            for (const char* f : {build::kFlavorDesktop, build::kFlavorServer})
                CHECK(build::plan_adapter(t, f, "game.pack").supported);
        CHECK(build::plan_adapter(build::kTargetWeb, build::kFlavorDesktop, "game.pack").supported);
        // Honest stub (R-BUILD-007): an unknown target, and web in the (nonsensical) server flavor.
        CHECK(!build::plan_adapter("playstation", build::kFlavorDesktop, "game.pack").supported);
        CHECK(!build::plan_adapter(build::kTargetWeb, build::kFlavorServer, "game.pack").supported);
    }

    // === Seam 3 — desktop code-signing MUST split, never-silent (a10 + a13) ============================
    {
        for (const char* target : {build::kTargetWindows, build::kTargetMacos})
        {
            const build::SigningPlan plan = build::plan_signing(target);
            CHECK(plan.required);
            build::SigningInputs inputs;
            inputs.requested = true;
            inputs.artifact_signed = false; // per-PR path: no secrets → unsigned, but NEVER silent
            const build::SigningReport rep = build::evaluate_signing(plan, inputs);
            CHECK(rep.state == build::kSigningStateUnsigned);
            CHECK(rep.code == build::kBuildArtifactUnsignedCode);
            CHECK(!rep.warning.empty());
        }
        CHECK(build::plan_signing(build::kTargetWindows).method == build::kSigningMethodAuthenticode);
        CHECK(build::plan_signing(build::kTargetMacos).method == build::kSigningMethodDeveloperId);
        CHECK(build::plan_signing(build::kTargetMacos).notarization_required);
        CHECK(!build::plan_signing(build::kTargetLinux).required);
        CHECK(!build::plan_signing(build::kTargetWeb).required);
    }

    // === Seam 4 — verify-before-use fails CLOSED under the a08 pinned trust root (R-SEC-009 / L-58) ====
    {
        // The pure exit taxonomy never maps a non-zero verifier result to Ok, on either host shell.
        CHECK(common::classify_verify_exit_code(0, sp::Shell::Posix) == common::VerifyStatus::Ok);
        CHECK(common::classify_verify_exit_code(255, sp::Shell::Posix) != common::VerifyStatus::Ok);
        CHECK(common::classify_verify_exit_code(-1, sp::Shell::Cmd) != common::VerifyStatus::Ok);
        // The pinned production root pins ONE Ed25519 publisher under the release identity + namespace.
        const std::string root = read_file_text(CONTEXT_TRUST_ROOT);
        CHECK(!root.empty());
        CHECK(contains(root, std::string(common::kReleaseSignerIdentity)));
        CHECK(contains(root, std::string(common::kArtifactNamespace)));
        CHECK(contains(root, "ssh-ed25519") && !contains(root, "ssh-rsa"));
    }

    // === Seam 5 — pack format v1 frozen (R-ASSET-005): the orchestrator's pack reads back valid ========
    {
        const build::BuildResult r = build::run_build(linux_request());
        CHECK(r.ok && !r.pack_bytes.empty());
        const pack::ParsedPack parsed = pack::read_pack(r.pack_bytes);
        CHECK(parsed.ok);                    // every chunk's content hash verified
        CHECK(parsed.engine_version == 9);
        CHECK(r.summary.unit_count >= 1);    // GUID-addressed content units
    }

    // === Seam 6 — the R-BUILD-009 headless-smoke inputs are complete (runnable-artifact layout) ========
    {
        const build::AdapterPlan plan =
            build::plan_adapter(build::kTargetLinux, build::kFlavorServer, "game.pack");
        CHECK(plan.supported);
        bool has_runtime = false, has_pack = false, has_launcher = false, has_manifest = false;
        for (const build::ArtifactEntry& e : plan.layout)
        {
            has_runtime = has_runtime || e.role == build::kRoleRuntime;
            has_pack = has_pack || e.role == build::kRolePack;
            has_launcher = has_launcher || e.role == build::kRoleLauncher;
            has_manifest = has_manifest || e.role == build::kRoleManifest;
        }
        CHECK(has_runtime && has_pack && has_launcher && has_manifest);
        CHECK(!build::render_launcher(plan).empty()); // the boot launcher renders
    }

    // === Seam 7 — wedge determinism (L-54): the shipped Session is bit-reproducible ====================
    {
        // The packed-vs-shipped state-hash gate + the L-48/R-NET-001 replication gate are the m8-exit-4a /
        // m8-exit-4b sibling aliases (they launch the shipped binary / load the pack); here the seam is
        // the underlying LAW — the SAME (seed, scenario, ticks) yields the SAME hierarchical state hash.
        constexpr std::uint64_t kSeed = 2718281;
        constexpr std::uint64_t kTicks = 24;
        session::SessionConfig cfg;
        cfg.seed = kSeed;
        cfg.scenario = "demo";
        session::Session a(cfg);
        session::Session b(cfg);
        const std::uint64_t ha = a.step(kTicks).state_hash.root;
        const std::uint64_t hb = b.step(kTicks).state_hash.root;
        CHECK(ha == hb);
        CHECK(ha != 0);
    }

    // === Seam 8 — OPS1 HONESTY (R-BUILD-006 / R-QA-012): build-time budget is advisory-until-provisioned =
    {
        // The a12 build-time budget number is advisory until the perf-isolated runner class is
        // provisioned (ops1, an open owner hardware gate). The M8 exit RECORDS that honestly: the
        // committed budget declares itself advisory + names its unprovisioned runner — it does NOT assert
        // a blocking build-time number the runner cannot yet enforce.
        const std::string budget = read_file_text(CONTEXT_BUILD_TIME_BUDGET);
        CHECK(!budget.empty());
        CHECK(contains(budget, "\"advisory_until_provisioned\": true"));
        CHECK(contains(budget, "perf-linux-bare-metal"));
        CHECK(contains(budget, "R-BUILD-006"));
    }

    return report("m8-exit-5-seam-checklist",
                  "all M8 build-pipeline seams held (orchestration, adapters, signing, verify-before-use, "
                  "pack v1, smoke inputs, wedge determinism, OPS1-honest advisory budget)");
}
