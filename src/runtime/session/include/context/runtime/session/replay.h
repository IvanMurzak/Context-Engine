// The versioned, schema'd replay artifact (R-QA-005 / L-54; the ctx:replay kind).
//
// A replay artifact captures everything needed to reproduce a headless run: the seed, the tick
// count, the input stream, the engine + protocol versions it ran under, a CONTENT-HASH MANIFEST of
// the project inputs it ran against, and — in deterministic mode — the EXPECTED per-tick root-hash
// trace. It is a versioned engine file registered as the ctx:replay vocabulary kind (engine_schemas(),
// R-CLI-005), so `context describe` publishes its schema and it round-trips through the canonical
// serializer like the other M2 kinds.
//
// Replay is manifest-FIRST: run_replay verifies the content manifest against the project BEFORE
// stepping, so a replay against drifted content is reported as DRIFT — never silently run and
// mislabeled as a state divergence. In deterministic mode it compares each tick's root hash to the
// expected trace and reports the FIRST divergent tick; a non-deterministic artifact is replayed and
// explicitly labeled BEST-EFFORT (no expected trace to verify against).

#pragma once

#include "context/editor/serializer/json_tree.h"
#include "context/runtime/session/input.h"
#include "context/runtime/session/session.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace context::runtime::session
{

namespace serializer = ::context::editor::serializer;

// One project input the run depended on + its canonical content hash at record time.
struct ContentManifestEntry
{
    std::string path;      // project-root-relative
    std::uint64_t hash = 0; // serializer canonical-content hash of the file's bytes at record time
};

// The ctx:replay artifact.
struct ReplayArtifact
{
    std::uint64_t seed = 0;
    std::uint64_t tick_count = 0;
    std::string scenario = "demo";
    std::string engine_version;
    std::int64_t protocol_major = 0;
    InputStream input_stream;
    std::vector<ContentManifestEntry> content_manifest;
    bool deterministic = true;
    std::vector<std::uint64_t> expected_hash_trace; // per-tick root hashes (deterministic mode only)
};

// Record an artifact by running a fresh deterministic session (seed + scenario), feeding `stream`,
// and stepping `ticks`. In deterministic mode the expected per-tick root-hash trace is captured.
[[nodiscard]] ReplayArtifact record_replay(const SessionConfig& config, const InputStream& stream,
                                           std::uint64_t ticks,
                                           std::vector<ContentManifestEntry> manifest,
                                           bool deterministic = true);

// Canonical (de)serialization (conforms to the ctx:replay kind schema).
[[nodiscard]] serializer::JsonValue replay_to_json(const ReplayArtifact& artifact);
[[nodiscard]] std::string replay_dump(const ReplayArtifact& artifact);

struct ReplayParseResult
{
    bool ok = false;
    ReplayArtifact artifact;
    std::string error_code; // "replay.artifact_invalid" on failure
    std::string message;
};
[[nodiscard]] ReplayParseResult replay_from_json(const serializer::JsonValue& doc);
[[nodiscard]] ReplayParseResult replay_parse(std::string_view text);

// The manifest verification result: the project-relative paths whose current content hash no longer
// matches the artifact (drifted or missing). Empty == the project inputs are unchanged.
[[nodiscard]] std::vector<std::string> verify_manifest(const ReplayArtifact& artifact,
                                                       std::string_view project_root);

struct ReplayResult
{
    bool ok = false;             // true == manifest verified AND (deterministic ? no divergence)
    bool manifest_verified = false;
    std::vector<std::string> drifted_paths; // non-empty == content drift (never silent divergence)
    std::int64_t first_divergence_tick = -1; // -1 == no divergence (or best-effort)
    bool best_effort = false;    // true == non-deterministic artifact; not hash-verified
    std::uint64_t final_root = 0;
    std::uint64_t sim_tick = 0;
};

// Verify the manifest, then replay. On content drift, returns manifest_verified=false with
// drifted_paths set and does NOT run (the drift is reported, never mislabeled as divergence). On a
// verified manifest it replays; deterministic artifacts report the first divergent tick, a
// non-deterministic artifact is labeled best_effort.
[[nodiscard]] ReplayResult run_replay(const ReplayArtifact& artifact, std::string_view project_root);

} // namespace context::runtime::session
