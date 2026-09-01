// The wire file-write gateway (see wire_file_gateway.h for the D10 / lifetime / fail-closed
// rationale). Every member name below is asserted against a LIVE daemon by the e2 T2 drill, so a
// rename on either side reddens a ctest rather than silently refusing every file operation.

#include "context/editor/shell/panels/wire_file_gateway.h"

#include "context/editor/client/client.h" // the wire writes (complete type HERE only)
#include "wire_read.h"                    // read_string / read_bool / envelope_data

#include <optional>
#include <string>
#include <utility>

namespace context::editor::shell::panels
{

namespace
{

// The refusal shape, in ONE place. Two things it deliberately does NOT do: it never invents a
// message (the daemon's own diagnostic is what names the referring file, the occupied destination,
// the missing token — a generic "the operation failed" would throw away the only actionable half),
// and it never reports `applied`.
[[nodiscard]] files::FileWriteResult refused(std::string code, std::string message,
                                             std::string path)
{
    files::FileWriteResult out;
    out.status = files::FileWriteResult::Status::refused;
    out.code = std::move(code);
    out.message = std::move(message);
    out.path = std::move(path);
    return out;
}

} // namespace

void WireFileWriteGateway::bind_client(client::Client* client) noexcept
{
    client_ = client;
}

files::FileWriteResult WireFileWriteGateway::move_file(const std::string& from,
                                                       const std::string& to)
{
    if (client_ == nullptr)
    {
        ++refusals_;
        return refused(kNoDaemonCode,
                       "no daemon connection is bound; the move was not delivered and nothing was "
                       "written",
                       from);
    }

    contract::Json params = contract::Json::object();
    params.set("from", contract::Json(from));
    params.set("to", contract::Json(to));

    ++writes_issued_;
    std::string error;
    const std::optional<contract::Json> reply =
        client_->call(kMoveMethod, std::move(params), error);
    if (!reply.has_value())
    {
        ++refusals_;
        // The daemon's own code, VERBATIM (asset.move_destination_exists, path.jail_violation,
        // scope.denied, …) — each is a different fact with a different remedy, and the R-CLI-008
        // rule `Client::failure_code` states in one place is to pass it through, never to flatten it.
        return refused(client_->failure_code("internal.error"), error, from);
    }

    const contract::Json& data = envelope_data(*reply);
    files::FileWriteResult out;
    out.status = files::FileWriteResult::Status::applied;
    out.path = from;
    // Read the destination back off the REPLY rather than echoing the request: the daemon is the
    // authority on what it wrote, and an undo built from an echo would be an undo of what we asked
    // for rather than of what happened.
    out.other_path = read_string(data, "to");
    if (out.other_path.empty())
    {
        out.other_path = to;
    }
    ++writes_applied_;
    return out;
}

files::FileWriteResult WireFileWriteGateway::delete_file(const std::string& path)
{
    if (client_ == nullptr)
    {
        ++refusals_;
        return refused(kNoDaemonCode,
                       "no daemon connection is bound; NOTHING was deleted", path);
    }

    contract::Json params = contract::Json::object();
    params.set("path", contract::Json(path));

    ++writes_issued_;
    std::string error;
    const std::optional<contract::Json> reply =
        client_->call(kDeleteMethod, std::move(params), error);
    if (!reply.has_value())
    {
        ++refusals_;
        return refused(client_->failure_code("internal.error"), error, path);
    }

    const contract::Json& data = envelope_data(*reply);
    files::FileWriteResult out;
    out.status = files::FileWriteResult::Status::applied;
    out.path = read_string(data, "path");
    if (out.path.empty())
    {
        out.path = path;
    }
    out.restore_token = read_string(data, "restoreToken");
    ++writes_applied_;
    if (out.restore_token.empty())
    {
        // APPLIED but not reversible. Reported as applied because it IS — the file is gone, and
        // claiming otherwise would leave the panel rendering a row that no longer exists. Counted,
        // because "the human was promised an undo and did not get one" must never be invisible. The
        // converged-no-op delete (nothing was there) reaches here legitimately, which is why this is
        // a counter and not a refusal.
        ++unrecoverable_deletes_;
    }
    return out;
}

files::FileWriteResult WireFileWriteGateway::restore_file(const std::string& restore_token)
{
    if (client_ == nullptr)
    {
        ++refusals_;
        return refused(kNoDaemonCode, "no daemon connection is bound; nothing was restored", "");
    }

    contract::Json params = contract::Json::object();
    params.set("restoreToken", contract::Json(restore_token));

    ++writes_issued_;
    std::string error;
    const std::optional<contract::Json> reply =
        client_->call(kRestoreMethod, std::move(params), error);
    if (!reply.has_value())
    {
        ++refusals_;
        return refused(client_->failure_code("internal.error"), error, "");
    }

    const contract::Json& data = envelope_data(*reply);
    files::FileWriteResult out;
    out.status = files::FileWriteResult::Status::applied;
    out.path = read_string(data, "path");
    out.restore_token = restore_token;
    ++writes_applied_;
    return out;
}

} // namespace context::editor::shell::panels
