// Replay artifact record / (de)serialize / manifest-verify / run (see replay.h).

#include "context/runtime/session/replay.h"

#include "context/editor/filesync/path_jail.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"
#include "context/runtime/session/json_build.h"
#include "context/runtime/session/state_hash.h"

#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <utility>

#ifndef CONTEXT_ENGINE_VERSION
#define CONTEXT_ENGINE_VERSION "0.0.0"
#endif

namespace context::runtime::session
{

namespace
{
// Read a file's bytes; sets `ok` false if it cannot be opened.
std::string read_file(const std::string& path, bool& ok)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        ok = false;
        return {};
    }
    ok = true;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Join a project root + a project-relative path with a single '/'.
std::string join_path(std::string_view root, std::string_view rel)
{
    std::string out(root);
    if (!out.empty() && out.back() != '/' && out.back() != '\\')
        out += '/';
    out += rel;
    return out;
}
} // namespace

ReplayArtifact record_replay(const SessionConfig& config, const InputStream& stream,
                             std::uint64_t ticks, std::vector<ContentManifestEntry> manifest,
                             bool deterministic)
{
    Session session(config);
    // Seed the input schedule from the stream, then step with tracing on so the expected per-tick
    // root-hash trace can be captured.
    for (const TickInputs& t : stream.ticks)
    {
        for (const InputEvent& e : t.events)
            session.inject_event_at(t.tick, e);
        for (const ActionActivation& a : t.actions)
            session.inject_action_at(t.tick, a);
    }
    session.set_trace(deterministic);
    session.step(ticks);

    ReplayArtifact artifact;
    artifact.seed = config.seed;
    artifact.tick_count = ticks;
    artifact.scenario = session.scenario();
    artifact.engine_version = CONTEXT_ENGINE_VERSION;
    // Literal, not `kProtocolMajor` (src/editor/contract/include/context/editor/contract/handshake.h):
    // context_session does not (and should not) link context_contract — runtime/session stays
    // decoupled from the editor/contract layer boundary. Mirror this value if kProtocolMajor ever
    // moves past 1.
    artifact.protocol_major = 1; // the FROZEN contract major (kProtocolMajor, M3 freeze, R-CLI-004)
    artifact.input_stream = session.input_log();
    artifact.content_manifest = std::move(manifest);
    artifact.deterministic = deterministic;
    if (deterministic)
        artifact.expected_hash_trace = trace_roots(session.trace());
    return artifact;
}

serializer::JsonValue replay_to_json(const ReplayArtifact& artifact)
{
    serializer::JsonValue doc = jb::object();
    jb::set(doc, "$schema", jb::str("ctx:replay"));
    jb::set(doc, "version", jb::integer(1));
    jb::set(doc, "deterministic", jb::boolean(artifact.deterministic));
    jb::set(doc, "engineVersion", jb::str(artifact.engine_version));
    jb::set(doc, "protocolMajor", jb::integer(artifact.protocol_major));
    jb::set(doc, "scenario", jb::str(artifact.scenario));
    jb::set(doc, "seed", jb::uinteger(artifact.seed));
    jb::set(doc, "tickCount", jb::uinteger(artifact.tick_count));

    serializer::JsonValue manifest = jb::array();
    for (const ContentManifestEntry& e : artifact.content_manifest)
    {
        serializer::JsonValue entry = jb::object();
        jb::set(entry, "hash", jb::uinteger(e.hash));
        jb::set(entry, "path", jb::str(e.path));
        jb::push(manifest, std::move(entry));
    }
    jb::set(doc, "contentManifest", std::move(manifest));

    jb::set(doc, "inputStream", input_stream_to_json(artifact.input_stream));

    serializer::JsonValue trace = jb::array();
    for (std::uint64_t root : artifact.expected_hash_trace)
        jb::push(trace, jb::uinteger(root));
    jb::set(doc, "expectedHashTrace", std::move(trace));
    return doc;
}

std::string replay_dump(const ReplayArtifact& artifact)
{
    std::string out;
    if (!serializer::serialize_canonical(replay_to_json(artifact), out))
        out.clear(); // unreachable: replay trees carry only integers/strings (no non-finite doubles)
    return out;
}

namespace
{
// Upper bound on a replay artifact's tickCount. A ctx:replay artifact is untrusted, shareable input;
// this turns a maliciously huge tickCount (e.g. a 20-byte file with tickCount 2^64-1) from an
// effectively-infinite step loop + unbounded trace growth into a clean replay.artifact_invalid.
// 10M ticks is ~46 hours of 60Hz sim — orders of magnitude above any real determinism replay.
constexpr std::uint64_t kMaxReplayTicks = 10'000'000ULL;

ReplayParseResult artifact_invalid(std::string message)
{
    ReplayParseResult r;
    r.ok = false;
    r.error_code = "replay.artifact_invalid";
    r.message = std::move(message);
    return r;
}
} // namespace

ReplayParseResult replay_from_json(const serializer::JsonValue& doc)
{
    if (doc.type != serializer::JsonValue::Type::object)
        return artifact_invalid("replay artifact root is not an object");
    if (jb::as_int(jb::member(doc, "version")) != 1)
        return artifact_invalid("unsupported replay artifact version");

    ReplayArtifact artifact;
    artifact.seed = jb::as_uint(jb::member(doc, "seed"));
    artifact.tick_count = jb::as_uint(jb::member(doc, "tickCount"));
    if (artifact.tick_count > kMaxReplayTicks)
        return artifact_invalid("replay artifact tickCount exceeds the maximum supported (" +
                                std::to_string(kMaxReplayTicks) + ")");
    artifact.scenario = jb::as_str(jb::member(doc, "scenario"), "demo");
    artifact.engine_version = jb::as_str(jb::member(doc, "engineVersion"));
    artifact.protocol_major = jb::as_int(jb::member(doc, "protocolMajor"));
    artifact.deterministic = jb::as_bool(jb::member(doc, "deterministic"), true);

    if (const serializer::JsonValue* input = jb::member(doc, "inputStream"); input != nullptr)
        artifact.input_stream = input_stream_from_json(*input);

    if (const serializer::JsonValue* manifest = jb::member(doc, "contentManifest");
        manifest != nullptr && manifest->type == serializer::JsonValue::Type::array)
        for (const serializer::JsonValue& e : manifest->elements)
            artifact.content_manifest.push_back(
                ContentManifestEntry{jb::as_str(jb::member(e, "path")), jb::as_uint(jb::member(e, "hash"))});

    if (const serializer::JsonValue* trace = jb::member(doc, "expectedHashTrace");
        trace != nullptr && trace->type == serializer::JsonValue::Type::array)
        for (const serializer::JsonValue& root : trace->elements)
            artifact.expected_hash_trace.push_back(jb::as_uint(&root));

    ReplayParseResult result;
    result.ok = true;
    result.artifact = std::move(artifact);
    return result;
}

ReplayParseResult replay_parse(std::string_view text)
{
    serializer::ParseResult parsed = serializer::parse_json(text);
    if (!parsed.ok)
        return artifact_invalid("replay artifact is not well-formed JSON");
    return replay_from_json(parsed.root);
}

std::vector<std::string> verify_manifest(const ReplayArtifact& artifact,
                                         std::string_view project_root)
{
    std::vector<std::string> drifted;
    for (const ContentManifestEntry& entry : artifact.content_manifest)
    {
        // A ctx:replay artifact is untrusted, shareable input: a manifest entry that escapes the
        // project root (e.g. "../../etc/passwd") must never be opened as a content-hash oracle. Jail
        // the joined path with the same R-SEC-008 check the filesync write path uses, BEFORE read_file
        // hands it to the OS (which would resolve the ".." itself). An escaping entry is reported as
        // drift, exactly like a missing one.
        const std::string full = join_path(project_root, entry.path);
        if (!::context::editor::filesync::is_inside_jail(project_root, full))
        {
            drifted.push_back(entry.path);
            continue;
        }
        bool ok = false;
        const std::string bytes = read_file(full, ok);
        if (!ok)
        {
            drifted.push_back(entry.path); // missing input == drift
            continue;
        }
        // The engine's canonical-content hash: canonicalizes JSON, passes non-JSON through raw
        // (R-FILE-001) — the same content-identity notion the derivation cache keys on.
        const serializer::CanonicalizeResult canon = serializer::canonicalize(bytes);
        if (canon.canonical_hash != entry.hash)
            drifted.push_back(entry.path);
    }
    return drifted;
}

ReplayResult run_replay(const ReplayArtifact& artifact, std::string_view project_root)
{
    ReplayResult result;

    // Manifest-FIRST: a replay against drifted content is reported as drift, never run + mislabeled.
    result.drifted_paths = verify_manifest(artifact, project_root);
    if (!result.drifted_paths.empty())
    {
        result.manifest_verified = false;
        result.ok = false;
        return result;
    }
    result.manifest_verified = true;

    SessionConfig config;
    config.seed = artifact.seed;
    config.scenario = artifact.scenario;
    Session session(config);
    for (const TickInputs& t : artifact.input_stream.ticks)
    {
        for (const InputEvent& e : t.events)
            session.inject_event_at(t.tick, e);
        for (const ActionActivation& a : t.actions)
            session.inject_action_at(t.tick, a);
    }
    session.set_trace(artifact.deterministic);
    const StepResult stepped = session.step(artifact.tick_count);
    result.sim_tick = stepped.sim_tick;
    result.final_root = stepped.state_hash.root;

    if (!artifact.deterministic)
    {
        // Non-deterministic replay: nothing to verify against — explicitly BEST-EFFORT.
        result.best_effort = true;
        result.ok = true;
        return result;
    }

    // Deterministic: compare each tick's root hash to the expected trace; report the FIRST divergence.
    const std::vector<std::uint64_t> actual = trace_roots(session.trace());
    const std::size_t n = actual.size() < artifact.expected_hash_trace.size()
                              ? actual.size()
                              : artifact.expected_hash_trace.size();
    for (std::size_t i = 0; i < n; ++i)
        if (actual[i] != artifact.expected_hash_trace[i])
        {
            result.first_divergence_tick = static_cast<std::int64_t>(i);
            result.ok = false;
            return result;
        }
    // A length mismatch (short/long trace) is itself a divergence at the first missing tick.
    if (actual.size() != artifact.expected_hash_trace.size())
    {
        result.first_divergence_tick = static_cast<std::int64_t>(n);
        result.ok = false;
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace context::runtime::session
