// `context determinism diff <left> <right>` implementation (see determinism_command.h).

#include "context/cli/determinism_command.h"

#include "context/runtime/session/replay.h"
#include "context/runtime/session/triage.h"

#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>

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

// Read + parse one side; on failure returns an error envelope in `err` (labeled with `side`).
std::optional<session::ReplayArtifact> load_side(const std::string& side, const std::string& path,
                                                 Envelope& err)
{
    const std::optional<std::string> bytes = read_file(path);
    if (!bytes.has_value())
    {
        err = Envelope::failure("file.not_found",
                                "no " + side + " replay artifact at '" + path + "'");
        return std::nullopt;
    }
    const session::ReplayParseResult parsed = session::replay_parse(*bytes);
    if (!parsed.ok)
    {
        err = Envelope::failure(parsed.error_code, side + ": " + parsed.message);
        return std::nullopt;
    }
    return parsed.artifact;
}
} // namespace

Envelope run_determinism_diff(const std::map<std::string, std::string>& bound,
                              const std::map<std::string, std::string>& /*flags*/)
{
    const auto left_it = bound.find("left");
    const auto right_it = bound.find("right");
    if (left_it == bound.end() || right_it == bound.end())
        return Envelope::failure("usage.missing_argument",
                                 "determinism diff requires two artifact paths (left, right)");

    Envelope err;
    const std::optional<session::ReplayArtifact> left = load_side("left", left_it->second, err);
    if (!left.has_value())
        return err;
    const std::optional<session::ReplayArtifact> right = load_side("right", right_it->second, err);
    if (!right.has_value())
        return err;

    const session::DivergenceReport report = session::triage_divergence(*left, *right);

    Json data = Json::object();
    data.set("diverged", Json(report.diverged));
    data.set("reproduced", Json(report.reproduced));
    data.set("seedMatch", Json(report.seed_match));
    data.set("scenarioMatch", Json(report.scenario_match));
    data.set("inputMatch", Json(report.input_match));
    data.set("leftTicks", Json(static_cast<std::uint64_t>(report.left_ticks)));
    data.set("rightTicks", Json(static_cast<std::uint64_t>(report.right_ticks)));

    if (report.diverged)
    {
        data.set("tick", Json(report.tick));
        if (!report.system.empty())
            data.set("system", Json(report.system));
        if (report.field.found)
        {
            Json entity = Json::object();
            entity.set("index", Json(static_cast<std::uint64_t>(report.field.entity_index)));
            entity.set("generation",
                       Json(static_cast<std::uint64_t>(report.field.entity_generation)));
            data.set("entity", std::move(entity));
            data.set("archetype", Json(report.field.archetype));
            data.set("component", Json(report.field.component));
            data.set("field", Json(report.field.field));
            data.set("componentField", Json(report.field.component + "." + report.field.field));
            data.set("structural", Json(report.field.structural));
            if (!report.field.structural)
            {
                data.set("leftValue", Json(report.field.left_value));
                data.set("rightValue", Json(report.field.right_value));
            }
        }
    }
    return Envelope::success(std::move(data));
}

} // namespace context::cli
