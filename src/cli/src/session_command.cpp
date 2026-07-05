// `context session <verb>` implementation (see session_command.h).

#include "context/cli/session_command.h"

#include "context/editor/serializer/canonical.h"
#include "context/runtime/session/replay.h"
#include "context/runtime/session/session.h"
#include "context/runtime/session/session_state.h"
#include "context/runtime/session/state_hash.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace context::cli
{

using editor::contract::Envelope;
using editor::contract::Json;
namespace session = context::runtime::session;

namespace
{
std::optional<std::string> read_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Durable write: stage into a sibling temp file, then rename it over `path` (R-FILE-004 semantics) so
// a concurrent/crashing reader sees either the whole OLD or whole NEW file, never a torn write. This
// hand-rolls the stage-then-rename (rather than filesync::atomic_write) because the CLI writes
// user-supplied paths that are not rooted in any FileStore jail.
bool write_file(const std::string& path, const std::string& content)
{
    namespace fs = std::filesystem;
    const fs::path target(path);
    const fs::path tmp(path + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out.good())
            return false;
    } // flush + close before the rename
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec)
    {
        fs::remove(tmp, ec); // best-effort: don't leave staging residue on a failed rename
        return false;
    }
    return true;
}

// A 64-bit value as a 0x-prefixed 16-digit hex string — the envelope hash representation (the
// R-CLI-008 JSON DOM stores numbers as doubles, which cannot hold a full 64-bit hash).
std::string to_hex(std::uint64_t v)
{
    char buf[19];
    std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(v));
    return std::string(buf);
}

std::optional<std::uint64_t> parse_u64(const std::string& s)
{
    // std::stoull mirrors strtoull, which accepts a leading '-' and returns the 2^64 wraparound
    // rather than failing — so "-1" would parse as 18446744073709551615. Reject a leading sign so an
    // unsigned flag (--seed / --ticks / --at) honors its documented "unsigned integer" contract.
    const std::size_t first = s.find_first_not_of(" \t");
    if (first != std::string::npos && s[first] == '-')
        return std::nullopt;
    try
    {
        std::size_t pos = 0;
        const unsigned long long v = std::stoull(s, &pos, 0);
        if (pos != s.size())
            return std::nullopt;
        return static_cast<std::uint64_t>(v);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::optional<std::int64_t> parse_i64(const std::string& s)
{
    try
    {
        std::size_t pos = 0;
        const long long v = std::stoll(s, &pos, 0);
        if (pos != s.size())
            return std::nullopt;
        return static_cast<std::int64_t>(v);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

const std::string* flag(const std::map<std::string, std::string>& flags, const char* name)
{
    auto it = flags.find(name);
    return it != flags.end() ? &it->second : nullptr;
}

// The hierarchical state hash as an envelope object (hashes as hex strings).
Json state_hash_json(const session::StateHash& hash)
{
    Json out = Json::object();
    out.set("root", Json(to_hex(hash.root)));
    Json archetypes = Json::array();
    for (const session::ArchetypeHash& a : hash.archetypes)
    {
        Json entry = Json::object();
        entry.set("signature", Json(a.signature));
        entry.set("hash", Json(to_hex(a.hash)));
        entry.set("entityCount", Json(static_cast<std::uint64_t>(a.entity_count)));
        archetypes.push_back(std::move(entry));
    }
    out.set("archetypes", std::move(archetypes));
    return out;
}

Json trace_json(const session::HashTrace& trace)
{
    Json out = Json::array();
    for (const session::HashTree& tree : trace)
    {
        Json entry = Json::object();
        entry.set("tick", Json(static_cast<std::uint64_t>(tree.tick)));
        entry.set("root", Json(to_hex(tree.root)));
        Json systems = Json::array();
        for (const session::SystemHash& s : tree.per_system)
        {
            Json sys = Json::object();
            sys.set("system", Json(s.system));
            sys.set("hash", Json(to_hex(s.hash)));
            systems.push_back(std::move(sys));
        }
        entry.set("perSystem", std::move(systems));
        out.push_back(std::move(entry));
    }
    return out;
}

// Load a session-state file into a Session, or a failure envelope describing why.
struct Loaded
{
    std::optional<session::Session> session;
    Envelope error;
};

Loaded load_session(const std::string& state_path)
{
    const std::optional<std::string> bytes = read_file(state_path);
    if (!bytes.has_value())
        return {std::nullopt,
                Envelope::failure("session.state_not_found",
                                  "no session-state file at '" + state_path + "'")};
    session::LoadResult loaded = session::session_state_parse(*bytes);
    if (!loaded.ok)
        return {std::nullopt, Envelope::failure(loaded.error_code, loaded.message)};
    return {std::move(loaded.session), Envelope::success()};
}

Envelope save_session(const std::string& state_path, const session::Session& s, Json data)
{
    if (!write_file(state_path, session::session_state_dump(s)))
        return Envelope::failure("internal.error",
                                 "could not write session-state file '" + state_path + "'");
    return Envelope::success(std::move(data));
}

// --- the individual verbs -------------------------------------------------------------------------

Envelope session_new(const std::string& state, const std::map<std::string, std::string>& flags)
{
    session::SessionConfig config;
    if (const std::string* seed = flag(flags, "seed"))
    {
        const std::optional<std::uint64_t> parsed = parse_u64(*seed);
        if (!parsed.has_value())
            return Envelope::failure("session.input_invalid", "--seed is not an unsigned integer");
        config.seed = *parsed;
    }
    if (const std::string* scenario = flag(flags, "scenario"))
    {
        // Only the built-in `demo` scenario exists today; reject an unknown name loudly rather than
        // silently constructing an empty world (setup_scenario populates entities for "demo" only).
        if (*scenario != "demo")
            return Envelope::failure(
                "session.input_invalid",
                "--scenario '" + *scenario + "' is not a known scenario (expected 'demo')");
        config.scenario = *scenario;
    }

    session::Session s(config);
    Json data = Json::object();
    data.set("stateFile", Json(state));
    data.set("seed", Json(std::to_string(s.seed())));
    data.set("scenario", Json(s.scenario()));
    data.set("simTick", Json(static_cast<std::uint64_t>(s.sim_tick())));
    return save_session(state, s, std::move(data));
}

Envelope session_step(const std::string& state, const std::map<std::string, std::string>& flags)
{
    Loaded loaded = load_session(state);
    if (!loaded.session.has_value())
        return loaded.error;
    session::Session& s = *loaded.session;

    std::uint64_t ticks = 1;
    if (const std::string* t = flag(flags, "ticks"))
    {
        const std::optional<std::uint64_t> parsed = parse_u64(*t);
        if (!parsed.has_value())
            return Envelope::failure("session.input_invalid", "--ticks is not an unsigned integer");
        ticks = *parsed;
    }
    if (flag(flags, "trace"))
        s.set_trace(true);

    const session::StepResult result = s.step(ticks);
    Json data = Json::object();
    data.set("simTick", Json(static_cast<std::uint64_t>(result.sim_tick)));
    data.set("stateHash", state_hash_json(result.state_hash));
    if (s.trace_enabled())
        data.set("trace", trace_json(s.trace()));
    return save_session(state, s, std::move(data));
}

Envelope session_seed(const std::string& state, const std::map<std::string, std::string>& flags)
{
    Loaded loaded = load_session(state);
    if (!loaded.session.has_value())
        return loaded.error;
    session::Session& s = *loaded.session;

    const bool set = flag(flags, "set") != nullptr;
    if (set)
    {
        const std::optional<std::uint64_t> parsed = parse_u64(*flag(flags, "set"));
        if (!parsed.has_value())
            return Envelope::failure("session.input_invalid", "--set is not an unsigned integer");
        s.set_seed(*parsed);
    }
    Json data = Json::object();
    data.set("seed", Json(std::to_string(s.seed())));
    data.set("simTick", Json(static_cast<std::uint64_t>(s.sim_tick())));
    if (set)
        return save_session(state, s, std::move(data));
    return Envelope::success(std::move(data));
}

Envelope session_inject(const std::string& state, const std::map<std::string, std::string>& flags)
{
    Loaded loaded = load_session(state);
    if (!loaded.session.has_value())
        return loaded.error;
    session::Session& s = *loaded.session;

    std::int64_t value = 1;
    if (const std::string* v = flag(flags, "value"))
    {
        const std::optional<std::int64_t> parsed = parse_i64(*v);
        if (!parsed.has_value())
            return Envelope::failure("session.input_invalid", "--value is not an integer");
        value = *parsed;
    }
    std::uint64_t at = s.sim_tick();
    if (const std::string* a = flag(flags, "at"))
    {
        const std::optional<std::uint64_t> parsed = parse_u64(*a);
        if (!parsed.has_value())
            return Envelope::failure("session.input_invalid", "--at is not an unsigned integer");
        at = *parsed;
    }

    std::string kind;
    if (const std::string* action = flag(flags, "action"))
    {
        const std::string phase = flag(flags, "phase") ? *flag(flags, "phase") : "performed";
        s.inject_action_at(at, session::ActionActivation{*action, phase, value});
        kind = "action";
    }
    else if (const std::string* device = flag(flags, "event"))
    {
        const std::string code = flag(flags, "code") ? *flag(flags, "code") : "";
        s.inject_event_at(at, session::InputEvent{*device, code, value});
        kind = "event";
    }
    else
    {
        return Envelope::failure("session.input_invalid",
                                 "inject requires --action <name> or --event <device>");
    }

    Json data = Json::object();
    data.set("injected", Json(kind));
    data.set("atTick", Json(at));
    data.set("simTick", Json(static_cast<std::uint64_t>(s.sim_tick())));
    return save_session(state, s, std::move(data));
}

Envelope session_hash(const std::string& state, const std::map<std::string, std::string>& /*flags*/)
{
    Loaded loaded = load_session(state);
    if (!loaded.session.has_value())
        return loaded.error;
    const session::Session& s = *loaded.session;

    Json data = Json::object();
    data.set("simTick", Json(static_cast<std::uint64_t>(s.sim_tick())));
    data.set("stateHash", state_hash_json(s.state_hash()));
    return Envelope::success(std::move(data));
}

Envelope session_record(const std::string& state, const std::map<std::string, std::string>& flags)
{
    Loaded loaded = load_session(state);
    if (!loaded.session.has_value())
        return loaded.error;
    const session::Session& s = *loaded.session;

    // Build the content manifest from --manifest paths, content-hashed under --project.
    const std::string project = flag(flags, "project") ? *flag(flags, "project") : ".";
    std::vector<session::ContentManifestEntry> manifest;
    if (const std::string* list = flag(flags, "manifest"))
    {
        std::stringstream ss(*list);
        std::string rel;
        while (std::getline(ss, rel, ','))
        {
            if (rel.empty())
                continue;
            const std::string full = project + "/" + rel;
            const std::optional<std::string> bytes = read_file(full);
            if (!bytes.has_value())
                return Envelope::failure("session.input_invalid",
                                         "manifest input not found: '" + full + "'");
            manifest.push_back(session::ContentManifestEntry{
                rel, editor::serializer::canonicalize(*bytes).canonical_hash});
        }
    }

    const bool deterministic = flag(flags, "non-deterministic") == nullptr;
    session::SessionConfig config;
    config.seed = s.seed();
    config.scenario = s.scenario();
    const session::ReplayArtifact artifact = session::record_replay(
        config, s.input_log(), s.sim_tick(), std::move(manifest), deterministic);

    Json data = Json::object();
    if (const std::string* out = flag(flags, "out"))
    {
        if (!write_file(*out, session::replay_dump(artifact)))
            return Envelope::failure("internal.error", "could not write artifact to '" + *out + "'");
        data.set("artifactFile", Json(*out));
    }
    data.set("seed", Json(std::to_string(artifact.seed)));
    data.set("tickCount", Json(static_cast<std::uint64_t>(artifact.tick_count)));
    data.set("deterministic", Json(artifact.deterministic));
    data.set("manifestEntries", Json(static_cast<std::uint64_t>(artifact.content_manifest.size())));
    Json trace = Json::array();
    for (std::uint64_t root : artifact.expected_hash_trace)
        trace.push_back(Json(to_hex(root)));
    data.set("expectedHashTrace", std::move(trace));
    return Envelope::success(std::move(data));
}
} // namespace

Envelope run_session(const std::string& verb, const std::map<std::string, std::string>& bound,
                     const std::map<std::string, std::string>& flags)
{
    auto it = bound.find("state");
    if (it == bound.end())
        return Envelope::failure("usage.missing_argument", "session requires a state-file path");
    const std::string& state = it->second;

    if (verb == "new")
        return session_new(state, flags);
    if (verb == "step")
        return session_step(state, flags);
    if (verb == "seed")
        return session_seed(state, flags);
    if (verb == "inject")
        return session_inject(state, flags);
    if (verb == "hash")
        return session_hash(state, flags);
    if (verb == "record")
        return session_record(state, flags);
    return Envelope::failure("usage.unknown_verb", "unknown session verb: '" + verb + "'");
}

} // namespace context::cli
