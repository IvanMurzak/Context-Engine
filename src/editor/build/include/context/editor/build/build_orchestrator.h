// Build orchestration core (R-BUILD-002 / R-BUILD-007, task a05): the pure, deterministic spine of the
// per-agent build pipeline. `run_build` drives the fixed phase sequence
//   verify → toolchain → aot → transcode → pack → link → adapter
// over IN-MEMORY inputs (no filesystem, no clock, no environment probes), so the same request always
// yields the same result — the build is a cache-keyable pure function (R-FILE-010), and the CLI wrapper
// (src/cli/build_command.cpp) owns the disk IO (reading the project, writing the pack).
//
// Agent-pool honesty (R-BUILD-007): `run_build` builds exactly THIS agent's requested target — one
// target, one pack. Orchestrating a fleet of targets across agents is caller configuration (a loop over
// requests), not a hidden capability in here.
//
// Each phase fails closed with its own build.* code (build_errors.h). The real byte-level derive →
// transcode → pack chain is wired (compose::flatten → partition_content_units → import::transcode_variant
// → pack::write_pack); the native AOT compile, the final machine-code link, and the platform adapter are
// honestly STUBBED until their tasks land (a06 adapters), reported as such in the summary — never a
// silent fake. The stubs still fail-closed with build.aot_failed / build.link_failed when their declared
// inputs are malformed, so every code is reachable from a real request (the R-QA-011 corpus).

#pragma once

#include "context/editor/build/toolchain_manifest.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/import/importer.h"
#include "context/editor/pack/pack_format.h"

#include <cstdint>
#include <string>
#include <vector>

namespace context::editor::build
{

// One authored-TypeScript entrypoint the build must AOT-compile for the target (R-LANG-*). v1
// validates the declaration + stakes out the AOT phase; the TS→native compile lands with a06+.
struct BuildScript
{
    std::string name;       // the logical script id (for diagnostics)
    std::string entrypoint; // the authored .ts entrypoint, project-relative
};

// One binary sidecar to transcode for the target (task a03): the pre-imported derived artifact plus the
// sidecar identity the pack addresses it by. The CLI/importer produces `artifact`; the orchestrator
// transcodes it to the request's target platform and packs the variant under that platform's column.
struct BuildArtifact
{
    std::string relpath;              // owner-relative sidecar path (the readable hint the pack stores)
    std::uint64_t raw_hash = 0;       // the sidecar's declared raw-byte hash — its load-by-GUID key
    std::string common_bytes;         // the common (untranscoded) sidecar payload — the a01 fallback blob
    import::DerivedArtifact artifact; // the imported artifact to transcode for the target platform
};

// The build request — everything the pure orchestrator needs, supplied by the caller (the CLI reads it
// off disk; a test constructs it in memory).
struct BuildRequest
{
    std::string target;                 // the build target id: windows | linux | macos | web
    const compose::SceneResolver* resolver = nullptr; // resolves the project's scene files (for flatten)
    std::string root_scene_path;        // the project's root scene (from the project manifest)
    std::vector<ToolchainEntry> toolchain; // the per-target toolchain manifest (R-PKG-002); usually the
                                           // embedded toolchain_manifest() — injectable for the corpus
    std::vector<BuildScript> scripts;   // authored TS scripts to AOT (may be empty — a data-only game)
    std::vector<BuildArtifact> artifacts; // binary sidecars to transcode (may be empty)
    std::vector<std::string> referenced_packages;  // packages this build references (the link set)
    std::vector<std::string> registrable_packages; // packages with a registrable Module (link inputs)
    std::uint64_t engine_version = pack::kDefaultEngineVersion; // R-FILE-010 cache-key input
    compose::ComposeLimits limits{};    // flatten limits (defaults = the shipped profile)
};

// The artifact summary a successful build reports (the CLI folds it into the R-CLI-008 envelope data).
struct BuildSummary
{
    std::string target;
    std::uint64_t engine_version = 0;
    std::uint64_t generation = 0;   // a deterministic content identity of the derived world this pack
                                    // was built from (FNV-1a-64 over the composed entity identities) —
                                    // the same project always yields the same generation (reproducible)
    std::uint64_t pack_hash = 0;    // FNV-1a-64 of the produced pack bytes (the artifact fingerprint)
    std::size_t pack_size = 0;      // the pack byte length
    std::size_t unit_count = 0;     // content units in the pack
    std::size_t chunk_count = 0;    // pack directory entries (units + sidecars)
    std::size_t sidecar_count = 0;  // transcoded sidecar variants packed
    std::size_t entity_count = 0;   // composed entities across all units
    std::vector<std::string> registered_packages; // the R-KERNEL-003 reg-TU package set (LTO/DCE footprint)
    std::string registration_tu;    // the generated registration TU source (per-build, cache-exempt — a12)
    bool adapter_stub = true;       // the platform adapter is a stub until a06 (reported, never faked)
};

// The outcome. ok ⇒ (pack_bytes, summary) are the build product; !ok ⇒ (error_code, error_message,
// error_pointer) name the fail-closed refusal (error_code is a build.* code from build_errors.h, or
// internal.error for a defensive invariant break).
struct BuildResult
{
    bool ok = false;
    std::string pack_bytes;
    BuildSummary summary;
    std::string error_code;
    std::string error_message;
    std::string error_pointer; // the offending file / package / target (surfaced as the envelope pointer)
};

// Drive one per-agent build to completion. Pure + deterministic + total (never throws).
[[nodiscard]] BuildResult run_build(const BuildRequest& request);

} // namespace context::editor::build
