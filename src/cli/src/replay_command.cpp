// `context replay <artifact>` implementation (see replay_command.h).

#include "context/cli/replay_command.h"

#include "context/runtime/session/replay.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
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

std::string to_hex(std::uint64_t v)
{
    char buf[19];
    std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(v));
    return std::string(buf);
}

std::string join(const std::vector<std::string>& items)
{
    std::string out;
    for (const std::string& s : items)
    {
        if (!out.empty())
            out += ", ";
        out += s;
    }
    return out;
}
} // namespace

Envelope run_replay(const std::map<std::string, std::string>& bound,
                    const std::map<std::string, std::string>& flags)
{
    auto it = bound.find("artifact");
    if (it == bound.end())
        return Envelope::failure("usage.missing_argument", "replay requires an artifact path");

    const std::optional<std::string> bytes = read_file(it->second);
    if (!bytes.has_value())
        return Envelope::failure("file.not_found", "no replay artifact at '" + it->second + "'");

    const session::ReplayParseResult parsed = session::replay_parse(*bytes);
    if (!parsed.ok)
        return Envelope::failure(parsed.error_code, parsed.message);

    const std::string project =
        flags.count("project") ? flags.at("project") : std::string(".");
    const session::ReplayResult result = session::run_replay(parsed.artifact, project);

    // Content drift is reported BEFORE any divergence claim (manifest-first).
    if (!result.manifest_verified)
        return Envelope::failure("replay.manifest_drift",
                                 "project inputs drifted from the artifact manifest: " +
                                     join(result.drifted_paths));

    // A deterministic divergence localizes the first divergent tick (non-zero exit — a usable gate).
    if (result.first_divergence_tick >= 0)
        return Envelope::failure(
            "replay.divergence",
            "deterministic replay diverged at tick " +
                std::to_string(result.first_divergence_tick) + " (ran " +
                std::to_string(result.sim_tick) + " ticks)");

    Json data = Json::object();
    data.set("ok", Json(result.ok));
    data.set("manifestVerified", Json(result.manifest_verified));
    data.set("bestEffort", Json(result.best_effort));
    data.set("simTick", Json(static_cast<std::uint64_t>(result.sim_tick)));
    data.set("finalRoot", Json(to_hex(result.final_root)));
    data.set("seed", Json(std::to_string(parsed.artifact.seed)));
    data.set("deterministic", Json(parsed.artifact.deterministic));
    return Envelope::success(std::move(data));
}

} // namespace context::cli
