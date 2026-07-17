// Build orchestration core (see build_orchestrator.h).

#include "context/editor/build/build_orchestrator.h"

#include "context/editor/build/build_errors.h"
#include "context/editor/compose/content_unit.h"
#include "context/editor/import/platform_profile.h"
#include "context/editor/import/transcode.h"
#include "context/editor/pack/pack_format.h"
#include "context/editor/pack/pack_writer.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>

namespace context::editor::build
{

namespace compose = editor::compose;
namespace import = editor::import;
namespace pack = editor::pack;

namespace
{

// FNV-1a-64 — the same content-hash family the pack directory uses (pack_format.h), for the build's
// deterministic derived-world generation + pack fingerprint. Local + tiny so the module stays lean.
[[nodiscard]] std::uint64_t fnv1a64(std::string_view bytes) noexcept
{
    std::uint64_t h = 1469598103934665603ULL;
    for (const unsigned char c : bytes)
    {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

// Fold a 64-bit value into a running FNV-1a-64 state, little-endian byte order (stable across hosts).
[[nodiscard]] std::uint64_t fnv_fold_u64(std::uint64_t h, std::uint64_t value) noexcept
{
    for (int i = 0; i < 8; ++i)
    {
        h ^= static_cast<unsigned char>((value >> (i * 8)) & 0xFFU);
        h *= 1099511628211ULL;
    }
    return h;
}

// A deterministic content identity of the derived world this pack was built from: FNV-1a-64 folded over
// every composed entity's stable identity hash, in expansion order (deterministic). The same project
// always yields the same generation, so the build's reported generation is reproducible.
[[nodiscard]] std::uint64_t derived_generation(const compose::ComposedScene& scene) noexcept
{
    std::uint64_t h = 1469598103934665603ULL;
    for (const compose::ComposedEntity& e : scene.entities)
        h = fnv_fold_u64(h, e.identity_hash);
    return h;
}

[[nodiscard]] BuildResult fail(std::string_view code, std::string message, std::string pointer = {})
{
    BuildResult r;
    r.ok = false;
    r.error_code = std::string(code);
    r.error_message = std::move(message);
    r.error_pointer = std::move(pointer);
    return r;
}

} // namespace

BuildResult run_build(const BuildRequest& request)
{
    // --- Phase 1: verify the runnable template (build.template_unverified) ----------------------------
    // The project must derive to a non-empty composed world with no blocking composition diagnostic —
    // the pre-build "the project is a startable template" bar (R-QA-006 spirit). A missing/malformed/
    // empty root scene fails closed here rather than producing a hollow pack.
    if (request.resolver == nullptr || request.root_scene_path.empty())
        return fail(kBuildTemplateUnverifiedCode,
                    "no project scene to build (missing resolver or root scene path)",
                    request.root_scene_path);

    const compose::ComposedScene scene =
        compose::flatten(request.root_scene_path, *request.resolver, request.limits);
    if (!scene.ok)
        return fail(kBuildTemplateUnverifiedCode,
                    "the project scene failed composition (a blocking diagnostic); nothing was built",
                    request.root_scene_path);
    if (scene.entities.empty())
        return fail(kBuildTemplateUnverifiedCode,
                    "the project scene composed to zero entities; there is nothing to build",
                    request.root_scene_path);

    // --- Phase 2: resolve the per-target toolchain (R-PKG-002, build.toolchain_fetch_failed) ----------
    const ToolchainEntry* toolchain = resolve_toolchain(request.toolchain, request.target);
    if (toolchain == nullptr)
        return fail(kBuildToolchainFetchFailedCode,
                    "no toolchain manifest entry for target '" + request.target +
                        "'; its toolchain cannot be fetched (R-PKG-002 / L-42)",
                    request.target);

    // --- Phase 3: AOT the authored-script tier (build.aot_failed) -------------------------------------
    // v1 validates each declared TS entrypoint is well-formed (a non-empty `.ts` path); the actual
    // TS→native AOT compile lands with the platform adapters (a06+). A data-only game declares no
    // scripts and passes trivially.
    for (const BuildScript& script : request.scripts)
    {
        const std::string& ep = script.entrypoint;
        const bool is_ts = ep.size() >= 3 && ep.compare(ep.size() - 3, 3, ".ts") == 0;
        if (ep.empty() || !is_ts)
            return fail(kBuildAotFailedCode,
                        "authored script '" + script.name +
                            "' has no compilable TypeScript entrypoint; AOT cannot proceed",
                        ep.empty() ? script.name : ep);
    }

    // --- Phase 4: derive + transcode the per-platform variants (task a03, build.transcode_failed) -----
    const pack::PlatformVariant selector = pack::platform_variant_for(request.target);
    std::vector<pack::PackSidecar> sidecars;
    sidecars.reserve(request.artifacts.size());
    if (!request.artifacts.empty())
    {
        const import::PlatformProfile* profile = import::find_platform_profile(request.target);
        if (profile == nullptr)
            return fail(kBuildTranscodeFailedCode,
                        "target '" + request.target +
                            "' has no platform profile; its assets cannot be transcoded",
                        request.target);
        for (const BuildArtifact& art : request.artifacts)
        {
            const import::TranscodeResult tr = import::transcode_variant(art.artifact, *profile);
            if (!tr.ok)
                return fail(kBuildTranscodeFailedCode,
                            "transcoding sidecar '" + art.relpath + "' for target '" + request.target +
                                "' failed (" + tr.error + ")",
                            art.relpath);
            pack::PackSidecar sc;
            sc.relpath = art.relpath;
            sc.raw_hash = art.raw_hash;
            sc.bytes = art.common_bytes;
            sc.variants.push_back({selector, tr.variant.bytes});
            sidecars.push_back(std::move(sc));
        }
    }

    // --- Phase 5: pack the target artifact (the REAL deterministic pack — a01) -------------------------
    const compose::ContentUnitSet units = compose::partition_content_units(scene, *request.resolver);
    pack::PackWriteOptions opts;
    opts.engine_version = request.engine_version;
    opts.target_platform = selector;
    const pack::PackWriteResult packed = pack::write_pack(units, scene, sidecars, opts);
    if (!packed.ok)
        // A verified, composed scene serializes to canonical JSON by construction (R-FILE-001), so this
        // is a defensive internal invariant break, not a user-data fault.
        return fail("internal.error", "pack write failed unexpectedly (" + packed.error + ")",
                    request.root_scene_path);

    // --- Phase 6: final link — the R-KERNEL-003 generated-registration TU (build.link_failed) ---------
    // Generate the registration TU that names register_<pkg> for EXACTLY the referenced packages (the
    // LTO/DCE-friendly no-static-init mechanism). A referenced package with no registrable module is an
    // undefined register_<pkg> — precisely the link's undefined-symbol failure. The generated TU + its
    // package set are per-build + cache-exempt (they feed a12's budget lines).
    const std::set<std::string> registrable(request.registrable_packages.begin(),
                                             request.registrable_packages.end());
    std::vector<std::string> registered(request.referenced_packages.begin(),
                                        request.referenced_packages.end());
    std::sort(registered.begin(), registered.end());
    registered.erase(std::unique(registered.begin(), registered.end()), registered.end());
    for (const std::string& pkg : registered)
        if (registrable.find(pkg) == registrable.end())
            return fail(kBuildLinkFailedCode,
                        "the generated registration TU references register_" + pkg +
                            ", but no linked module defines it (R-KERNEL-003 undefined symbol)",
                        pkg);

    std::string reg_tu =
        "// GENERATED (a05 build link) — R-KERNEL-003 referenced-only registration.\n"
        "void register_referenced_packages(Kernel& kernel)\n{\n";
    for (const std::string& pkg : registered)
        reg_tu += "    register_" + pkg + "(kernel);\n";
    reg_tu += "    (void)kernel;\n}\n";

    // --- Phase 7: platform adapter (a06 Linux + a10 Windows + a11 web) + success summary --------------
    // The adapter plans the runnable artifact — the shipped RuntimeKernel binary (or the a11 web
    // wasm+js bundle) + this pack + a launcher + a manifest, in the R-BUILD-005 layout. plan_adapter is
    // pure + deterministic; the CLI (build_command.cpp) owns the on-disk artifact assembly + the
    // R-BUILD-009 smoke launch. A target with no real adapter yet (macos) plans supported=false — the
    // honest stub (R-BUILD-007), never a faked artifact.
    BuildResult result;
    result.ok = true;
    result.pack_bytes = packed.bytes;
    BuildSummary& s = result.summary;
    s.target = request.target;
    s.engine_version = request.engine_version;
    s.generation = derived_generation(scene);
    s.pack_hash = fnv1a64(packed.bytes);
    s.pack_size = packed.bytes.size();
    s.unit_count = units.units.size();
    s.sidecar_count = sidecars.size();
    s.chunk_count = units.units.size() + sidecars.size();
    s.entity_count = scene.entities.size();
    s.registered_packages = std::move(registered);
    s.registration_tu = std::move(reg_tu);
    s.adapter = plan_adapter(request.target, request.flavor, /*pack_name=*/"");
    return result;
}

} // namespace context::editor::build
