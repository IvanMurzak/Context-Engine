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

    // `edit-batch` — the daemon-initiated MULTI-file write, serialized through the R-FILE-004
    // crash-recovery intent log (the dispatcher already enforced file_write via the scope table;
    // edit_files re-checks it defensively). The reply reports each file's canonical hash plus
    // whether the derived world reflects the whole batch after a settle.
    if (method == "edit-batch")
    {
        if (!params.contains("files") || !params.at("files").is_array() ||
            params.at("files").size() == 0)
            return Envelope::failure("usage.missing_argument",
                                     "edit-batch requires a non-empty 'files' array of "
                                     "{path, content} objects");

        std::vector<BatchEdit> edits;
        edits.reserve(params.at("files").size());
        for (std::size_t i = 0; i < params.at("files").size(); ++i)
        {
            const Json& f = params.at("files").at(i);
            const std::optional<std::string> path = string_param(f, "path");
            const std::optional<std::string> content = string_param(f, "content");
            if (!path.has_value() || !content.has_value())
                return Envelope::failure("usage.missing_argument",
                                         "edit-batch files[" + std::to_string(i) +
                                             "] needs string 'path' and 'content'");
            edits.push_back(BatchEdit{*path, *content});
        }

        EditBatchOutcome out = kernel_.edit_files(edits, session.scopes);
        if (!out.ok)
        {
            Envelope fail = Envelope::failure(out.error_code);
            if (!out.error_detail.empty())
                fail.add_warning(out.error_detail);
            return fail;
        }

        kernel_.settle(); // drain the batch into the derived world (R-BRIDGE-008 quiescence)

        Json files = Json::array();
        bool all_reflected = true;
        for (const derivation::WriteTicket& t : out.tickets)
        {
            const std::optional<derivation::DerivedSource> node = kernel_.query(t.path);
            const bool reflected = node.has_value() && node->canonical_hash == t.canonical_hash;
            all_reflected = all_reflected && reflected;
            Json entry = Json::object();
            entry.set("path", Json(t.path));
            entry.set("canonicalHash", hash_string(t.canonical_hash));
            entry.set("reflected", Json(reflected));
            files.push_back(std::move(entry));
        }

        Json data = Json::object();
        data.set("opId", Json(out.op_id));
        data.set("files", std::move(files));
        data.set("allReflected", Json(all_reflected));
        data.set("worldEntities", Json(static_cast<std::uint64_t>(kernel_.world().alive_count())));
        data.set("generation", Json(kernel_.generation()));
        return Envelope::success(std::move(data), kernel_.generation());
    }

    // `snapshot` — the R-BRIDGE-008 current-state snapshot a newly-attached or RECONNECTING client
    // reads before deltas: the incarnation epoch id (a restart forces a fresh snapshot rather than
    // trusting a stale "since seq N" cursor), the derived-world generation, the last seq — plus the
    // boot-time R-FILE-004 recovery diagnostics so a reconnecting client learns about any op the
    // previous incarnation left incomplete. Read-only (read/query baseline).
    if (method == "snapshot")
    {
        Json data = kernel_.events().snapshot();
        data.set("worldEntities", Json(static_cast<std::uint64_t>(kernel_.world().alive_count())));
        data.set("worldGeneration", Json(kernel_.generation()));
        Json recovery = Json::array();
        for (const auto& d : kernel_.recovery_diagnostics())
        {
            Json entry = Json::object();
            entry.set("code", Json(d.code));
            entry.set("opId", Json(d.op_id));
            entry.set("message", Json(d.message));
            Json remaining = Json::array();
            for (const std::string& p : d.remaining_writes)
                remaining.push_back(Json(p));
            entry.set("remainingWrites", std::move(remaining));
            recovery.push_back(std::move(entry));
        }
        data.set("recoveryDiagnostics", std::move(recovery));
        return Envelope::success(std::move(data), kernel_.generation());
    }

    // `reconcile` — fold EXTERNAL (out-of-band) edits into the derived world: drain watcher hints +
    // the full re-hash crawl (content hash authoritative over watchers, R-FILE-002), then settle.
    // Read-only w.r.t. authored state (it makes the daemon notice on-disk truth; it writes nothing),
    // so it sits on the read/query baseline.
    if (method == "reconcile")
    {
        const std::size_t changes = kernel_.ingest_external().size();
        const std::uint64_t generation = kernel_.settle();
        Json data = Json::object();
        data.set("changes", Json(static_cast<std::uint64_t>(changes)));
        data.set("generation", Json(generation));
        data.set("worldEntities", Json(static_cast<std::uint64_t>(kernel_.world().alive_count())));
        return Envelope::success(std::move(data), generation);
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
        {
            // A null accept() is EITHER a clean stop()/shutdown OR a genuine listener error — only the
            // latter sets server.error(). Surface an error as a non-zero exit instead of letting a
            // transient IPC failure masquerade as a graceful daemon shutdown.
            if (!server.error().empty())
                return 1;
            break; // clean stop / listener closed
        }

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
