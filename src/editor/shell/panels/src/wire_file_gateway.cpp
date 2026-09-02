// The wire file-write gateway (see wire_file_gateway.h for the D10 / lifetime / fail-closed
// rationale). Every member name below is asserted against a LIVE daemon by the e2 T2 drill, so a
// rename on either side reddens a ctest rather than silently refusing every file operation.

#include "context/editor/shell/panels/wire_file_gateway.h"

#include "context/editor/client/client.h" // the wire writes (complete type HERE only)
#include "wire_read.h"                    // read_string / read_bool / envelope_data

#include <cstddef>
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

// The wire round-trip EVERY file verb makes, in ONE place: the unbound-gateway refusal, the two
// counters that bracket a request, and the daemon's failure code passed through verbatim. Returns
// the reply on success; on a refusal it fills `refusal` and returns nullopt, so a caller that
// forgets to check gets no reply to read rather than a silently unrecorded write.
//
// WHY IT IS FACTORED: the counters are the whole point of this class — `refusals()` is what makes
// "the human was told" checkable rather than assumed, and `writes_issued()` is what a landed write
// is measured against. Each verb asserts its own totals, so three hand-maintained copies of the
// increments would let a fourth verb drop one with every existing test still green. It is a
// file-local free function rather than a private member deliberately: the header (like its sibling
// `wire_override_gateway.h`) keeps `contract::Json` OFF the panel-facing surface, and one shared
// tail is not worth widening that.
[[nodiscard]] std::optional<contract::Json> issue(client::Client* client, const char* method,
                                                 contract::Json params,
                                                 const char* no_daemon_message,
                                                 const std::string& subject,
                                                 std::size_t& writes_issued, std::size_t& refusals,
                                                 files::FileWriteResult& refusal)
{
    if (client == nullptr)
    {
        ++refusals;
        refusal = refused(WireFileWriteGateway::kNoDaemonCode, no_daemon_message, subject);
        return std::nullopt;
    }

    ++writes_issued;
    std::string error;
    std::optional<contract::Json> reply = client->call(method, std::move(params), error);
    if (!reply.has_value())
    {
        ++refusals;
        // The daemon's own code, VERBATIM (asset.move_destination_exists, path.jail_violation,
        // scope.denied, …) — each is a different fact with a different remedy, and the R-CLI-008
        // rule `Client::failure_code` states in one place is to pass it through, never to flatten it.
        refusal = refused(client->failure_code("internal.error"), error, subject);
        return std::nullopt;
    }
    return reply;
}

} // namespace

void WireFileWriteGateway::bind_client(client::Client* client) noexcept
{
    client_ = client;
}

files::FileWriteResult WireFileWriteGateway::move_file(const std::string& from,
                                                       const std::string& to)
{
    contract::Json params = contract::Json::object();
    params.set("from", contract::Json(from));
    params.set("to", contract::Json(to));

    files::FileWriteResult refusal;
    const std::optional<contract::Json> reply =
        issue(client_, kMoveMethod, std::move(params),
              "no daemon connection is bound; the move was not delivered and nothing was written",
              from, writes_issued_, refusals_, refusal);
    if (!reply.has_value())
        return refusal;

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
    contract::Json params = contract::Json::object();
    params.set("path", contract::Json(path));

    files::FileWriteResult refusal;
    const std::optional<contract::Json> reply =
        issue(client_, kDeleteMethod, std::move(params),
              "no daemon connection is bound; NOTHING was deleted", path, writes_issued_, refusals_,
              refusal);
    if (!reply.has_value())
        return refusal;

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
    contract::Json params = contract::Json::object();
    params.set("restoreToken", contract::Json(restore_token));

    files::FileWriteResult refusal;
    const std::optional<contract::Json> reply =
        issue(client_, kRestoreMethod, std::move(params),
              "no daemon connection is bound; nothing was restored", "", writes_issued_, refusals_,
              refusal);
    if (!reply.has_value())
        return refusal;

    const contract::Json& data = envelope_data(*reply);
    files::FileWriteResult out;
    out.status = files::FileWriteResult::Status::applied;
    out.path = read_string(data, "path");
    out.restore_token = restore_token;
    ++writes_applied_;
    return out;
}

} // namespace context::editor::shell::panels
