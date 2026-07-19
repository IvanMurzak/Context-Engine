// `context attach` implementation (see attach_command.h). The WIRE — endpoint discovery, the attach
// handshake (token + scopes + protocol negotiate), and the JSON-RPC calls — is context_client
// (src/editor/client/), the one client-side implementation shared with `context fetch`, the editor
// shell, and every out-of-tree consumer. This file keeps only what is CLI-specific: argv parsing and
// the R-CLI-008 envelope it prints.

#include "context/cli/attach_command.h"

#include "context/cli/args.h"
#include "context/editor/client/client.h"
#include "context/editor/contract/json.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace context::cli
{

namespace fs = std::filesystem;
using editor::client::AttachOptions;
using editor::client::Client;
using editor::contract::Envelope;
using editor::contract::Json;

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
    const bool do_reconcile = has_flag(args, "reconcile");

    // --- discover + connect + attach --------------------------------------------------------------
    // Request write (for `edit`) + session (for the optional `shutdown`); the daemon's launch-time
    // operator ceiling clamps this to least privilege (R-SEC-007). connect_to_project() retains the
    // D20 attach token from `.editor/instance.json` — required since e02 flipped enforcement ON —
    // and attach() presents it.
    AttachOptions attach_options;
    attach_options.scope = "write,session";
    std::string err;
    const std::unique_ptr<Client> client = Client::connect_to_project(project, 3000, err);
    if (!client)
        return finish(Envelope::failure("internal.error", err));

    if (!client->attach(attach_options, err))
        // Report the daemon's OWN refusal code (Client::failure_code): since e02 enforces the token,
        // a refusal here is usually `attach.denied` — a PERMISSION exit class, not the protocol
        // mismatch a single hardcoded code would claim.
        return finish(
            Envelope::failure(client->failure_code("handshake.incompatible_protocol"), err));

    // --- optional: fold on-disk truth into the derived world over the wire (R-FILE-002) ----------
    // `--reconcile` drives the daemon's `reconcile` verb before the edit/query pair, so a project
    // whose authored files landed out-of-band (e.g. a generated benchmark corpus — the R-FILE-011
    // N-daemons scenario) is ingested through the REAL crawl + derivation path over the wire.
    bool reconcile_ok = true;
    Json reconcile_data;
    if (do_reconcile)
    {
        const std::optional<Json> rec_res = client->call("reconcile", Json::object(), err);
        if (!rec_res.has_value())
            return finish(Envelope::failure("internal.error", err));
        reconcile_ok = rec_res->contains("ok") && rec_res->at("ok").as_bool();
        if (rec_res->contains("data"))
            reconcile_data = rec_res->at("data");
    }

    // --- edit a file over the wire (file-rewriter) ----------------------------------------------
    Json edit_params = Json::object();
    edit_params.set("path", Json(set_path));
    edit_params.set("content", Json(set_content));
    const std::optional<Json> edit_res = client->call("edit", std::move(edit_params), err);
    if (!edit_res.has_value())
        return finish(Envelope::failure("internal.error", err));
    const bool edit_ok = edit_res->contains("ok") && edit_res->at("ok").as_bool();
    const Json& edit_data = edit_res->at("data");
    const bool reflected = edit_data.contains("reflected") && edit_data.at("reflected").as_bool();

    // --- query the derived world over the wire (read-your-writes) --------------------------------
    Json query_params = Json::object();
    query_params.set("path", Json(set_path));
    const std::optional<Json> query_res = client->call("query", std::move(query_params), err);
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
    std::string shutdown_error;
    if (do_shutdown)
    {
        const std::optional<Json> stop_res = client->call("shutdown", Json::object(), err);
        shutdown_ack = stop_res.has_value();
        // A bare `false` says nothing about WHY the daemon did not ack (refused? wire dropped before
        // the reply?), which is exactly the diagnosis someone needs when this fails. Carry the reason.
        if (!shutdown_ack)
            shutdown_error = err;
    }

    // --- summarize ------------------------------------------------------------------------------
    Json data = Json::object();
    data.set("endpoint", Json(client->instance().endpoint));
    data.set("attached", Json(true));
    data.set("edit", edit_data);
    data.set("query", query_data);
    data.set("hashesMatch", Json(hashes_match));
    if (do_reconcile)
        data.set("reconcile", reconcile_data);
    if (do_shutdown)
    {
        data.set("shutdownAck", Json(shutdown_ack));
        if (!shutdown_error.empty())
            data.set("shutdownError", Json(shutdown_error));
    }

    const bool all_ok = edit_ok && reflected && query_ok && present && hashes_match && reconcile_ok;
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
