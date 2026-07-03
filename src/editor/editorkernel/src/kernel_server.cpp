// KernelServer implementation (see kernel_server.h).

#include "context/editor/editorkernel/kernel_server.h"

#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"
#include "context/editor/derivation/derivation_graph.h"

#include <optional>
#include <string>

namespace context::editor::editorkernel
{

using contract::Envelope;
using contract::Json;

namespace
{
// A full-range 64-bit canonical hash exceeds 2^53, so it is carried as a DECIMAL STRING to round-trip
// losslessly over the wire (mirrors editor_driver.cpp — the R-CLI-006 replay key must not lose bits).
Json hash_string(std::uint64_t h)
{
    return Json(std::to_string(h));
}

std::optional<std::string> string_param(const Json& params, const std::string& key)
{
    if (params.contains(key) && params.at(key).is_string())
        return params.at(key).as_string();
    return std::nullopt;
}
} // namespace

KernelServer::KernelServer(EditorKernel& kernel) : kernel_(kernel)
{
    // Register as the dispatcher's method backend. MUST precede kernel.start() (the dispatcher is
    // constructed at boot and captures the backend then).
    kernel_.set_method_backend(this);
}

std::optional<Envelope> KernelServer::invoke(const std::string& method, const Json& params,
                                             const bridge::Session& session) const
{
    // `edit` — the daemon-initiated cross-process file write (the file_write scope was already
    // enforced by the dispatcher; edit_file re-checks it defensively). Writes through filesync
    // atomic-IO, then runs the read-your-writes barrier so the reply reports whether the derived
    // world already reflects the write (R-CLI-006).
    if (method == "edit")
    {
        const std::optional<std::string> path = string_param(params, "path");
        const std::optional<std::string> content = string_param(params, "content");
        if (!path.has_value() || !content.has_value())
            return Envelope::failure("usage.missing_argument",
                                     "edit requires string 'path' and 'content' params");

        EditOutcome out = kernel_.edit_file(*path, *content, session.scopes);
        if (!out.ok)
            return out.envelope();

        const std::optional<derivation::DerivedSource> observed =
            kernel_.query_after_hash(*path, out.ticket.canonical_hash);
        const bool reflected =
            observed.has_value() && observed->canonical_hash == out.ticket.canonical_hash;

        Json data = Json::object();
        data.set("path", Json(*path));
        data.set("canonicalHash", hash_string(out.ticket.canonical_hash));
        data.set("reflected", Json(reflected));
        data.set("worldEntities", Json(static_cast<std::uint64_t>(kernel_.world().alive_count())));
        data.set("generation", Json(kernel_.generation()));

        Envelope env = Envelope::success(std::move(data), kernel_.generation());
        if (!reflected)
            env.add_warning("the derived world did not reflect the edit within the read barrier bound");
        return env;
    }

    // `query` — read the derived node for a path + world stats. Read-only (read/query baseline).
    if (method == "query")
    {
        const std::optional<std::string> path = string_param(params, "path");
        if (!path.has_value())
            return Envelope::failure("usage.missing_argument", "query requires a string 'path' param");

        const std::optional<derivation::DerivedSource> node = kernel_.query(*path);
        Json data = Json::object();
        data.set("path", Json(*path));
        data.set("present", Json(node.has_value()));
        if (node.has_value())
        {
            data.set("canonicalHash", hash_string(node->canonical_hash));
            data.set("nodeGeneration", Json(node->generation));
        }
        data.set("worldEntities", Json(static_cast<std::uint64_t>(kernel_.world().alive_count())));
        data.set("generation", Json(kernel_.generation()));
        return Envelope::success(std::move(data), kernel_.generation());
    }

    // `shutdown` — ask the serve loop to stop after replying (session_control scope, enforced above).
    if (method == "shutdown")
    {
        stop_.store(true); // `stop_` is mutable; the serve loop breaks after this reply is written
        Json data = Json::object();
        data.set("stopping", Json(true));
        return Envelope::success(std::move(data));
    }

    return std::nullopt; // not an operational verb — let the dispatcher route it (describe / reserved)
}

int KernelServer::serve(bridge::TransportServer& server)
{
    while (!stop_.load())
    {
        std::optional<bridge::TransportConnection> conn = server.accept();
        if (!conn.has_value())
            break; // stopped or listener closed

        // One fresh session per connection: the dispatcher establishes it on the attach handshake and
        // reads its scopes on every subsequent method (R-SEC-007).
        bridge::Session session;
        while (true)
        {
            const std::optional<std::string> request = conn->read_frame();
            if (!request.has_value())
                break; // client disconnected (or a framing error)

            const std::string response = kernel_.daemon().dispatcher().handle(*request, session);
            if (!response.empty() && !conn->write_frame(response))
                break; // peer gone

            if (stop_.load())
                break; // a `shutdown` verb was served on this message; reply already sent
        }

        if (stop_.load())
            break;
    }
    return 0;
}

} // namespace context::editor::editorkernel
