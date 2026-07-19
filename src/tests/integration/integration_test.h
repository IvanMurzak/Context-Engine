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

// --- sanitizer-aware timeout scaling -------------------------------------------------------------
// Instrumented builds boot the REAL `context daemon` child far slower: on the `sanitize`
// (ASan+UBSan) leg under concurrent runner load the m1-exit-3 kill-9 recovery test's daemon boot
// has overshot the plain 15s discovery-hint wait ~2/3 of the time — a FLAKE, not a defect (see
// docs/sanitizer-v8-false-positives.md § Related). Under a sanitizer, widen the cross-process
// boot wait by kSanitizerTimeoutScale so the boot race has headroom; the plain dev/CI legs are
// unchanged (scale 1). The `sanitize` preset compiles ASan+UBSan together and the `tsan` preset
// compiles TSan, so detecting ASan OR TSan covers both CI sanitizer legs (GCC exposes the
// __SANITIZE_* macros; Clang signals via __has_feature).
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define CONTEXT_ITEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define CONTEXT_ITEST_SANITIZED 1
#endif
#endif

#if defined(CONTEXT_ITEST_SANITIZED)
inline constexpr int kSanitizerTimeoutScale = 4;
#else
inline constexpr int kSanitizerTimeoutScale = 1;
#endif

// Scale a base wall-clock wait (ms) for the active sanitizer; identity on a non-instrumented
// build. Never shrinks a timeout (kSanitizerTimeoutScale >= 1, asserted below).
inline constexpr int scaled_timeout_ms(int base_ms)
{
    return base_ms * kSanitizerTimeoutScale;
}
static_assert(kSanitizerTimeoutScale >= 1, "sanitizer timeout scale must never shrink a timeout");

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

// Read the daemon's D20 per-instance attach token from the discovery hint. Best-effort: an empty
// string when the file is absent/torn/tokenless — the caller then attaches without one, which since
// M9 e02 the daemon refuses (`attach.denied`), so a missing token surfaces as a refusal rather than
// as silently-unauthenticated access. Called only AFTER instance_endpoint() has already waited out
// the boot race, so it needs no retry loop of its own.
inline std::string instance_token(const fs::path& project)
{
    const std::string text = read_file(project / ".editor" / "instance.json");
    if (text.empty())
        return std::string();
    try
    {
        const Json doc = Json::parse(text);
        if (doc.contains("token") && doc.at("token").is_string())
            return doc.at("token").as_string();
    }
    catch (const std::exception&)
    {
        // torn read — treat as tokenless
    }
    return std::string();
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
        // D20 (enforcement DEFAULT ON since M9 e02): remember the daemon's per-instance attach
        // token so attach() can present it. This harness deliberately keeps its own RAW wire (it
        // must send malformed/edge-case handshakes the context_client SDK will not emit — a wrong
        // protocolMajor, an over-broad scope — to assert the daemon's refusals), so it carries the
        // token itself rather than riding the SDK.
        token_ = instance_token(project);
        client_.emplace(*endpoint);
        if (!client_->connect(timeout_ms))
        {
            error_ = "connect failed: " + client_->error();
            return false;
        }
        return true;
    }

    // The token this connection discovered (empty when the daemon published none). A test that wants
    // to prove the D20 refusal path attaches with attach_with_token("") or a wrong value.
    [[nodiscard]] const std::string& token() const noexcept { return token_; }

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
        return attach_with_token(protocol_major, caps, scope_spec, token_);
    }

    // attach(), with an EXPLICIT token — the seam for asserting the D20 refusal path (pass "" for a
    // tokenless attach, or a wrong value for an impostor).
    std::optional<Json> attach_with_token(std::uint64_t protocol_major,
                                          const std::vector<std::string>& caps,
                                          const std::string& scope_spec, const std::string& token)
    {
        Json params = Json::object();
        params.set("protocolMajor", Json(protocol_major));
        Json c = Json::array();
        for (const std::string& cap : caps)
            c.push_back(Json(cap));
        params.set("capabilities", std::move(c));
        if (!scope_spec.empty())
            params.set("scope", Json(scope_spec));
        if (!token.empty())
            params.set("token", Json(token));
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
    std::string token_;
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
