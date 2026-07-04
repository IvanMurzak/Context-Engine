// Shared wire-client plumbing implementation (see wire_client.h). Moved verbatim from
// attach_command.cpp when `context fetch` (R-CLI-017) became the second operational wire client.

#include "context/cli/wire_client.h"

#include <chrono>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace context::cli
{

using editor::bridge::TransportClient;
using editor::contract::Json;

namespace
{
std::string read_file(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
} // namespace

std::optional<std::string> flag_value(const std::vector<std::string>& args, const std::string& name)
{
    const std::string prefix = "--" + name;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == prefix && i + 1 < args.size())
            return args[i + 1];
        const std::string eq = prefix + "=";
        if (args[i].rfind(eq, 0) == 0)
            return args[i].substr(eq.size());
    }
    return std::nullopt;
}

std::optional<std::uint64_t> parse_u64(std::string_view text)
{
    if (text.empty())
        return std::nullopt;
    std::uint64_t value = 0;
    for (const char ch : text)
    {
        if (ch < '0' || ch > '9')
            return std::nullopt;
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u)
            return std::nullopt; // would overflow
        value = value * 10u + digit;
    }
    return value;
}

bool has_flag(const std::vector<std::string>& args, const std::string& name)
{
    const std::string f = "--" + name;
    for (const std::string& a : args)
        if (a == f)
            return true;
    return false;
}

std::optional<std::string> discover_endpoint(const fs::path& project, int timeout_ms)
{
    const fs::path instance = project / ".editor" / "instance.json";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        std::error_code ec;
        if (fs::exists(instance, ec))
        {
            const std::string text = read_file(instance);
            if (!text.empty())
            {
                try
                {
                    const Json doc = Json::parse(text);
                    if (doc.contains("endpoint") && doc.at("endpoint").is_string() &&
                        !doc.at("endpoint").as_string().empty())
                        return doc.at("endpoint").as_string();
                }
                catch (const std::exception&)
                {
                    // A torn read while the daemon writes the file — retry until it is complete.
                }
            }
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

std::string build_request(std::int64_t id, const std::string& method, Json params)
{
    Json req = Json::object();
    req.set("jsonrpc", Json(std::string("2.0")));
    req.set("id", Json(id));
    req.set("method", Json(method));
    req.set("params", std::move(params));
    return req.dump(0);
}

std::optional<Json> call(TransportClient& client, std::int64_t id, const std::string& method,
                         Json params, std::string& error, bool* rejected_by_daemon)
{
    const std::optional<std::string> raw =
        client.request(build_request(id, method, std::move(params)));
    if (!raw.has_value())
    {
        error = "transport error on '" + method + "': " + client.error();
        return std::nullopt;
    }
    Json response;
    try
    {
        response = Json::parse(*raw);
    }
    catch (const std::exception& e)
    {
        error = "malformed response to '" + method + "': " + e.what();
        return std::nullopt;
    }
    if (response.contains("error"))
    {
        const Json& err = response.at("error");
        const std::string msg =
            err.contains("message") ? err.at("message").as_string() : std::string("(no message)");
        error = "daemon rejected '" + method + "': " + msg;
        if (rejected_by_daemon != nullptr)
            *rejected_by_daemon = true;
        return std::nullopt;
    }
    if (!response.contains("result"))
    {
        error = "response to '" + method + "' carried neither result nor error";
        return std::nullopt;
    }
    return response.at("result");
}

} // namespace context::cli
