// Instance-document discovery implementation (see instance.h).

#include "context/editor/client/instance.h"

#include "context/editor/contract/json.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;

namespace context::editor::client
{

using contract::Json;

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

std::optional<InstanceInfo> parse_instance_document(const std::string& text)
{
    if (text.empty())
        return std::nullopt;
    Json doc;
    try
    {
        doc = Json::parse(text);
    }
    catch (const std::exception&)
    {
        // A torn read while the daemon writes the file — the caller retries.
        return std::nullopt;
    }
    if (!doc.is_object())
        return std::nullopt;
    if (!doc.contains("endpoint") || !doc.at("endpoint").is_string() ||
        doc.at("endpoint").as_string().empty())
        return std::nullopt;

    InstanceInfo info;
    info.endpoint = doc.at("endpoint").as_string();
    if (doc.contains("token") && doc.at("token").is_string())
        info.token = doc.at("token").as_string();
    if (doc.contains("protocolMajor") && doc.at("protocolMajor").is_number())
        info.protocol_major = static_cast<std::uint32_t>(doc.at("protocolMajor").as_int());
    if (doc.contains("pid") && doc.at("pid").is_number())
        info.pid = doc.at("pid").as_int();
    return info;
}

std::optional<InstanceInfo> discover_instance(const fs::path& project, int timeout_ms)
{
    const fs::path instance = project / ".editor" / "instance.json";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        std::error_code ec;
        if (fs::exists(instance, ec))
        {
            if (std::optional<InstanceInfo> info = parse_instance_document(read_file(instance)))
                return info;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

std::string format_daemon_ready_line(const InstanceInfo& info)
{
    Json doc = Json::object();
    doc.set("endpoint", Json(info.endpoint));
    doc.set("token", Json(info.token));
    doc.set("protocolMajor", Json(static_cast<std::uint64_t>(info.protocol_major)));
    doc.set("pid", Json(static_cast<std::int64_t>(info.pid)));
    return std::string(kDaemonReadyLinePrefix) + " " + doc.dump(0);
}

std::optional<InstanceInfo> parse_daemon_ready_line(std::string_view line)
{
    const std::string_view prefix = kDaemonReadyLinePrefix;
    // Trim leading whitespace so an indented log framing does not defeat the prefix match.
    std::size_t begin = 0;
    while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t'))
        ++begin;
    line.remove_prefix(begin);
    if (line.size() < prefix.size() || line.substr(0, prefix.size()) != prefix)
        return std::nullopt;
    std::string_view rest = line.substr(prefix.size());
    // A separator MUST follow the prefix, so a stray token like "context.daemon.readyX" is not matched.
    std::size_t sep = 0;
    while (sep < rest.size() && (rest[sep] == ' ' || rest[sep] == '\t'))
        ++sep;
    if (sep == 0)
        return std::nullopt;
    rest.remove_prefix(sep);
    // The payload has the same shape as instance.json — reuse its parser (it requires a non-empty
    // endpoint, exactly the "usable ready line" condition).
    return parse_instance_document(std::string(rest));
}

} // namespace context::editor::client
