// Shared harness for the M1 exit-gate integration tests (ROADMAP §1 M1 Exit / issue #36): the tiny
// CHECK() runner (mirrors the per-component pattern) plus the cross-process helpers every criterion
// test needs — temp projects, raw on-disk edits, daemon discovery-hint waits, and a minimal JSON-RPC
// 2.0 client speaking to a REAL `context daemon` process over the REAL loopback IPC wire (#35).

#pragma once

#include "context/editor/bridge/transport.h"
#include "context/editor/contract/json.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace itest
{

inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

} // namespace itest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            itest::fail(__FILE__, __LINE__, #cond);                                                \
    } while (false)

#define ITEST_MAIN_END() return itest::g_failures == 0 ? 0 : 1

namespace itest
{

namespace fs = std::filesystem;
using context::editor::bridge::TransportClient;
using context::editor::contract::Json;

inline fs::path make_temp_project(const char* tag)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir =
        fs::temp_directory_path() / ("ctx-m1exit-" + std::string(tag) + "-" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

inline std::string read_file(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The "raw text edit" of L-19: a plain out-of-band write that bypasses every engine code path —
// exactly what an external editor / git checkout does to authored files.
inline bool write_file_raw(const fs::path& path, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f << content;
    return static_cast<bool>(f);
}

// Wait until the daemon publishes its R-ARCH-005 discovery hint (instance.json non-empty AND
// different from `previous` — pass "" for a first boot; pass the OLD file's content across a restart
// so a stale hint left behind by a killed daemon is not mistaken for the new incarnation's).
inline bool wait_for_instance(const fs::path& project, int timeout_ms,
                              const std::string& previous = std::string())
{
    const fs::path instance = project / ".editor" / "instance.json";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        std::error_code ec;
        if (fs::exists(instance, ec))
        {
            const std::string text = read_file(instance);
            if (!text.empty() && text != previous)
                return true;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

// Read the daemon's endpoint from the discovery hint (retrying through the boot race).
inline std::optional<std::string> instance_endpoint(const fs::path& project, int timeout_ms)
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
                    // torn read while the daemon writes the hint — retry
                }
            }
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

// A minimal JSON-RPC 2.0 client over the REAL IPC transport. call() returns the FULL parsed
// response object (so a test can inspect `result` OR `error.data.code`); nullopt only on a
// transport/parse failure (error() then says why).
class RpcClient
{
public:
    bool connect(const fs::path& project, int timeout_ms = 5000)
    {
        const std::optional<std::string> endpoint = instance_endpoint(project, timeout_ms);
        if (!endpoint.has_value())
        {
            error_ = "no discovery hint (instance.json) appeared";
            return false;
        }
        client_.emplace(*endpoint);
        if (!client_->connect(timeout_ms))
        {
            error_ = "connect failed: " + client_->error();
            return false;
        }
        return true;
    }

    std::optional<Json> call(const std::string& method, Json params)
    {
        if (!client_.has_value())
        {
            error_ = "not connected";
            return std::nullopt;
        }
        Json req = Json::object();
        req.set("jsonrpc", Json(std::string("2.0")));
        req.set("id", Json(++id_));
        req.set("method", Json(method));
        req.set("params", std::move(params));
        const std::optional<std::string> raw = client_->request(req.dump(0));
        if (!raw.has_value())
        {
            error_ = "transport error on '" + method + "': " + client_->error();
            return std::nullopt;
        }
        try
        {
            return Json::parse(*raw);
        }
        catch (const std::exception& e)
        {
            error_ = "malformed response to '" + method + "': " + e.what();
            return std::nullopt;
        }
    }

    // The attach handshake (R-CLI-010): carry {protocolMajor, capabilities[]} + the requested scope
    // spec. Returns the full response (success carries result.{protocolMajor, capabilities, scopes}).
    std::optional<Json> attach(std::uint64_t protocol_major, const std::vector<std::string>& caps,
                               const std::string& scope_spec)
    {
        Json params = Json::object();
        params.set("protocolMajor", Json(protocol_major));
        Json c = Json::array();
        for (const std::string& cap : caps)
            c.push_back(Json(cap));
        params.set("capabilities", std::move(c));
        if (!scope_spec.empty())
            params.set("scope", Json(scope_spec));
        return call("attach", std::move(params));
    }

    void close()
    {
        if (client_.has_value())
            client_->close();
    }

    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    std::optional<TransportClient> client_;
    std::string error_;
    std::int64_t id_ = 0;
};

// --- small response-shape helpers ---------------------------------------------------------------

inline bool is_ok(const std::optional<Json>& response)
{
    return response.has_value() && response->contains("result");
}

// The R-CLI-008 catalog code a JSON-RPC error response carries in error.data.code ("" when absent).
inline std::string error_code_of(const std::optional<Json>& response)
{
    if (!response.has_value() || !response->contains("error"))
        return std::string();
    const Json& err = response->at("error");
    if (!err.contains("data") || !err.at("data").contains("code"))
        return std::string();
    return err.at("data").at("code").as_string();
}

} // namespace itest
