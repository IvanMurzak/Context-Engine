// Replay artifact: round-trip, manifest-drift detection, first-divergence, best-effort labeling
// (R-QA-013 — deliverable 4 of R-QA-005).

#include "context/editor/serializer/canonical.h"
#include "context/runtime/session/replay.h"
#include "session_test.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace context::runtime::session;

namespace
{
SessionConfig cfg(std::uint64_t seed)
{
    SessionConfig c;
    c.seed = seed;
    return c;
}

InputStream demo_stream()
{
    InputStream s;
    s.add_action(0, ActionActivation{"move_x", "performed", 2});
    s.add_action(4, ActionActivation{"move_y", "performed", 3});
    s.add_event(1, InputEvent{"key", "W", 1});
    return s;
}

void write_file(const std::filesystem::path& p, const std::string& content)
{
    std::ofstream out(p, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}
} // namespace

int main()
{
    // --- record -> canonical dump -> parse round-trips every field -----------------------------
    {
        const ReplayArtifact artifact = record_replay(cfg(101), demo_stream(), 10, {}, true);
        CHECK(artifact.seed == 101);
        CHECK(artifact.tick_count == 10);
        CHECK(artifact.deterministic);
        CHECK(artifact.expected_hash_trace.size() == 10); // one root per tick
        // Literal 1, not `kProtocolMajor`: mirrors replay.cpp's record_replay() — this test does not
        // link context_contract (runtime/session -> editor/contract stays decoupled); keep in sync
        // with record_replay() if kProtocolMajor ever moves past 1.
        CHECK(artifact.protocol_major == 1); // the FROZEN contract major (M3 freeze, R-CLI-004)
        CHECK(!artifact.engine_version.empty());

        const std::string dumped = replay_dump(artifact);
        const ReplayParseResult parsed = replay_parse(dumped);
        CHECK(parsed.ok);
        CHECK(parsed.artifact.seed == artifact.seed);
        CHECK(parsed.artifact.tick_count == artifact.tick_count);
        CHECK(parsed.artifact.expected_hash_trace == artifact.expected_hash_trace);
        // the input stream survives the round-trip.
        CHECK(parsed.artifact.input_stream.at_tick(0) != nullptr);
        CHECK(parsed.artifact.input_stream.at_tick(4) != nullptr);
    }

    // --- a clean replay (empty manifest) verifies + reproduces, no divergence ------------------
    {
        const ReplayArtifact artifact = record_replay(cfg(7), demo_stream(), 12, {}, true);
        const ReplayResult r = run_replay(artifact, ".");
        CHECK(r.manifest_verified);
        CHECK(r.ok);
        CHECK(r.first_divergence_tick == -1);
        CHECK(!r.best_effort);
        CHECK(r.sim_tick == 12);
    }

    // --- manifest drift is reported BEFORE running, never as a silent divergence ---------------
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "ctx-replay-test-manifest";
        fs::create_directories(dir);
        const std::string rel = "input.json";
        write_file(dir / rel, "{\"a\":1}\n");

        const std::uint64_t hash =
            context::editor::serializer::canonicalize("{\"a\":1}\n").canonical_hash;
        ReplayArtifact artifact = record_replay(cfg(3), demo_stream(), 6, {{rel, hash}}, true);

        // Unchanged content: the manifest verifies and the replay runs.
        const std::string root = dir.string();
        ReplayResult ok = run_replay(artifact, root);
        CHECK(ok.manifest_verified);
        CHECK(ok.drifted_paths.empty());
        CHECK(ok.ok);

        // Mutate the project input: drift is reported, the replay does NOT run + mislabel it.
        write_file(dir / rel, "{\"a\":2}\n");
        ReplayResult drift = run_replay(artifact, root);
        CHECK(!drift.manifest_verified);
        CHECK(drift.drifted_paths.size() == 1);
        CHECK(drift.drifted_paths[0] == rel);
        CHECK(!drift.ok);
        CHECK(drift.first_divergence_tick == -1); // drift, NOT a divergence

        // A missing input is also drift.
        fs::remove(dir / rel);
        ReplayResult missing = run_replay(artifact, root);
        CHECK(!missing.manifest_verified);
        CHECK(missing.drifted_paths.size() == 1);

        fs::remove_all(dir);
    }

    // --- deterministic divergence: a tampered expected trace localizes the FIRST divergent tick -
    {
        ReplayArtifact artifact = record_replay(cfg(55), demo_stream(), 8, {}, true);
        // Corrupt the expected hash at tick 3; replay recomputes the true trace and flags tick 3.
        CHECK(artifact.expected_hash_trace.size() == 8);
        artifact.expected_hash_trace[3] ^= 0x1ULL;
        const ReplayResult r = run_replay(artifact, ".");
        CHECK(r.manifest_verified);
        CHECK(!r.ok);
        CHECK(r.first_divergence_tick == 3);
    }

    // --- a non-deterministic artifact is replayed + explicitly labeled best-effort -------------
    {
        const ReplayArtifact artifact = record_replay(cfg(9), demo_stream(), 5, {}, false);
        CHECK(artifact.expected_hash_trace.empty()); // no expected trace recorded
        const ReplayResult r = run_replay(artifact, ".");
        CHECK(r.manifest_verified);
        CHECK(r.ok);
        CHECK(r.best_effort);
        CHECK(r.first_divergence_tick == -1);
    }

    // --- a malformed artifact is rejected with the catalog code --------------------------------
    {
        const ReplayParseResult bad = replay_parse("not json at all");
        CHECK(!bad.ok);
        CHECK(bad.error_code == "replay.artifact_invalid");
    }

    SESSION_TEST_MAIN_END();
}
