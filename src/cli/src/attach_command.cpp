// `context attach` implementation (see attach_command.h).

#include "context/cli/attach_command.h"

#include "context/editor/bridge/transport.h"
#include "context/editor/contract/json.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace context::cli
{

namespace fs = std::filesystem;
using editor::contract::Envelope;
using editor::contract::Json;
using editor::bridge::TransportClient;

namespace
{
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

bool has_flag(const std::vector<std::string>& args, const std::string& name)
{
    const std::string f = "--" + name;
    for (const std::string& a : args)
        if (a == f)
            return true;
    return false;
}

std::string read_file(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Discover the daemon's endpoint from `<project>/.editor/instance.json`, retrying for the boot race
// (the daemon may still be publishing the file). nullopt if none appears within `timeout_ms`.
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

// Send one JSON-RPC request and return its `result` object. On a transport failure, a JSON-RPC error
// response, or a malformed reply, returns nullopt and sets `error`. When the failure is a daemon-side
// JSON-RPC error response (as opposed to a transport/parse failure) and `rejected_by_daemon` is
// non-null, sets `*rejected_by_daemon` — letting the handshake call site distinguish a genuine
// protocol rejection from a mere transport hiccup.
std::optional<Json> call(TransportClient& client, std::int64_t id, const std::string& method,
                         Json params, std::string& error, bool* rejected_by_daemon = nullptr)
{
    const std::optional<std::string> raw = client.request(build_request(id, method, std::move(params)));
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
} // namespace

Envelope run_attach(const std::vector<std::string>& args)
{
    // Resolve `--out` and its result-file sink FIRST so every failure path — including the earliest
    // --project validation below — honors --out when requested (parity with run_daemon()'s fail()).
    const std::optional<std::string> out = flag_value(args, "out");
    auto finish = [&out](Envelope env) -> Envelope
    {
        if (out.has_value())
        {
            std::ofstream f(*out, std::ios::binary | std::ios::trunc);
            if (f)
                f << env.dump(2) << '\n';
        }
        return env;
    };

    const std::optional<std::string> project_flag = flag_value(args, "project");
    if (!project_flag.has_value())
        return finish(
            Envelope::failure("usage.missing_argument", "context attach requires --project <dir>"));

    std::error_code ec;
    const fs::path project = fs::absolute(fs::path(*project_flag), ec);
    if (ec)
        return finish(Envelope::failure("internal.error",
                                        "could not resolve --project '" + *project_flag +
                                            "' to an absolute path: " + ec.message()));
    const std::string set_path = flag_value(args, "set-path").value_or("proj/a.scene");
    const std::string set_content = flag_value(args, "set-content").value_or("entity: 1");
    const bool do_shutdown = has_flag(args, "shutdown");

    // --- discover + connect ---------------------------------------------------------------------
    const std::optional<std::string> endpoint = discover_endpoint(project, 3000);
    if (!endpoint.has_value())
        return finish(Envelope::failure(
            "internal.error", "no running daemon found for project '" + project.string() +
                                  "' (no .editor/instance.json). Start one with `context daemon`."));

    TransportClient client(*endpoint);
    if (!client.connect(3000))
        return finish(Envelope::failure("internal.error",
                                        "could not connect to the daemon endpoint '" + *endpoint +
                                            "': " + client.error()));

    std::int64_t id = 0;
    std::string err;

    // --- attach: the capability handshake (requesting the file_write scope) ----------------------
    Json attach_params = Json::object();
    attach_params.set("protocolMajor", Json(static_cast<std::uint64_t>(0)));
    Json caps = Json::array();
    caps.push_back(Json(std::string("describe")));
    attach_params.set("capabilities", std::move(caps));
    // Request write (for `edit`) + session (for the optional `shutdown`); the daemon's launch-time
    // operator ceiling clamps this to least privilege (R-SEC-007).
    attach_params.set("scope", Json(std::string("write,session")));
    bool attach_rejected = false;
    const std::optional<Json> attach_res =
        call(client, ++id, "attach", std::move(attach_params), err, &attach_rejected);
    if (!attach_res.has_value())
        // Only a daemon-side rejection of the handshake is a genuine protocol/version mismatch; a
        // transport hiccup or malformed reply is internal (matching the edit/query call sites below).
        return finish(Envelope::failure(
            attach_rejected ? "handshake.incompatible_protocol" : "internal.error", err));

    // --- edit a file over the wire (file-rewriter) ----------------------------------------------
    Json edit_params = Json::object();
    edit_params.set("path", Json(set_path));
    edit_params.set("content", Json(set_content));
    const std::optional<Json> edit_res = call(client, ++id, "edit", std::move(edit_params), err);
    if (!edit_res.has_value())
        return finish(Envelope::failure("internal.error", err));
    const bool edit_ok = edit_res->contains("ok") && edit_res->at("ok").as_bool();
    const Json& edit_data = edit_res->at("data");
    const bool reflected = edit_data.contains("reflected") && edit_data.at("reflected").as_bool();

    // --- query the derived world over the wire (read-your-writes) --------------------------------
    Json query_params = Json::object();
    query_params.set("path", Json(set_path));
    const std::optional<Json> query_res = call(client, ++id, "query", std::move(query_params), err);
    if (!query_res.has_value())
        return finish(Envelope::failure("internal.error", err));
    const bool query_ok = query_res->contains("ok") && query_res->at("ok").as_bool();
    const Json& query_data = query_res->at("data");
    const bool present = query_data.contains("present") && query_data.at("present").as_bool();

    // Cross-process consistency: the edit's canonical hash must equal what the query reads back.
    const std::string edit_hash =
        edit_data.contains("canonicalHash") ? edit_data.at("canonicalHash").as_string() : "";
    const std::string query_hash =
        query_data.contains("canonicalHash") ? query_data.at("canonicalHash").as_string() : "";
    const bool hashes_match = !edit_hash.empty() && edit_hash == query_hash;

    // --- optional clean shutdown ----------------------------------------------------------------
    bool shutdown_ack = false;
    if (do_shutdown)
    {
        const std::optional<Json> stop_res = call(client, ++id, "shutdown", Json::object(), err);
        shutdown_ack = stop_res.has_value();
    }

    // --- summarize ------------------------------------------------------------------------------
    Json data = Json::object();
    data.set("endpoint", Json(*endpoint));
    data.set("attached", Json(true));
    data.set("edit", edit_data);
    data.set("query", query_data);
    data.set("hashesMatch", Json(hashes_match));
    if (do_shutdown)
        data.set("shutdownAck", Json(shutdown_ack));

    const bool all_ok = edit_ok && reflected && query_ok && present && hashes_match;
    Envelope env = Envelope::success(std::move(data));
    if (!all_ok)
    {
        Envelope fail = Envelope::failure(
            "internal.error", "cross-process attach drive did not fully reflect the edit "
                              "(edit_ok/reflected/query_ok/present/hashesMatch mismatch)");
        return finish(std::move(fail));
    }
    return finish(std::move(env));
}

} // namespace context::cli
