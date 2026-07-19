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

std::optional<std::string> discover_endpoint(const fs::path& project, int timeout_ms)
{
    if (std::optional<InstanceInfo> info = discover_instance(project, timeout_ms))
        return info->endpoint;
    return std::nullopt;
}

} // namespace context::editor::client
