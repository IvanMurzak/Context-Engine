// `context fetch` implementation (see fetch_command.h).

#include "context/cli/fetch_command.h"

#include "context/cli/wire_client.h"
#include "context/editor/bridge/resource_store.h" // hex_decode (the chunk codec)
#include "context/editor/bridge/transport.h"
#include "context/editor/contract/resource_handle.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

namespace context::cli
{

namespace fs = std::filesystem;
using editor::bridge::TransportClient;
using editor::contract::Envelope;
using editor::contract::Json;
using editor::contract::ResourceHandle;

namespace
{
// Parse the optional range positional "<offset>:<length>" (both non-negative decimal). nullopt on
// malformed input.
std::optional<std::pair<std::uint64_t, std::uint64_t>> parse_range(const std::string& text)
{
    const std::size_t colon = text.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size())
        return std::nullopt;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    for (std::size_t i = 0; i < colon; ++i)
    {
        if (text[i] < '0' || text[i] > '9')
            return std::nullopt;
        offset = offset * 10u + static_cast<std::uint64_t>(text[i] - '0');
    }
    for (std::size_t i = colon + 1; i < text.size(); ++i)
    {
        if (text[i] < '0' || text[i] > '9')
            return std::nullopt;
        length = length * 10u + static_cast<std::uint64_t>(text[i] - '0');
    }
    return std::make_pair(offset, length);
}

Json range_params(const std::string& uri, std::uint64_t offset, std::uint64_t length)
{
    Json range = Json::object();
    range.set("offsetBytes", Json(offset));
    if (length > 0)
        range.set("lengthBytes", Json(length));
    Json params = Json::object();
    params.set("handle", Json(uri));
    params.set("range", std::move(range));
    return params;
}
} // namespace

Envelope run_fetch(const std::string& handle_uri, const std::map<std::string, std::string>& flags)
{
    // Honor --out on every path (parity with run_attach / run_daemon).
    const auto out_it = flags.find("out");
    auto finish = [&](Envelope env) -> Envelope
    {
        if (out_it != flags.end())
        {
            std::ofstream f(out_it->second, std::ios::binary | std::ios::trunc);
            if (f)
                f << env.dump(2) << '\n';
        }
        return env;
    };

    // The handle must at least parse as a resource URI before we go looking for a daemon.
    const std::optional<ResourceHandle> handle = ResourceHandle::parse(handle_uri);
    if (!handle.has_value())
        return finish(Envelope::failure("resource.unknown_handle",
                                        "not a resource URI (context-res://...): " + handle_uri));

    const auto project_it = flags.find("project");
    if (project_it == flags.end())
        return finish(Envelope::failure("usage.missing_argument",
                                        "context fetch requires --project <dir> (the project whose "
                                        "daemon minted the handle)"));
    std::error_code ec;
    const fs::path project = fs::absolute(fs::path(project_it->second), ec);
    if (ec)
        return finish(Envelope::failure("internal.error", "could not resolve --project '" +
                                                              project_it->second +
                                                              "' to an absolute path: " +
                                                              ec.message()));

    std::optional<std::pair<std::uint64_t, std::uint64_t>> range;
    if (const auto range_it = flags.find("range"); range_it != flags.end())
    {
        range = parse_range(range_it->second);
        if (!range.has_value())
            return finish(Envelope::failure(
                "usage.invalid", "range expects <offset>:<length>, got '" + range_it->second + "'"));
    }

    // --- discover + connect + attach (read/query baseline suffices for resource.read) ------------
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

    Json attach_params = Json::object();
    attach_params.set("protocolMajor", Json(static_cast<std::uint64_t>(0)));
    attach_params.set("scope", Json(std::string("read")));
    bool attach_rejected = false;
    if (!call(client, ++id, "attach", std::move(attach_params), err, &attach_rejected).has_value())
        return finish(Envelope::failure(
            attach_rejected ? "handshake.incompatible_protocol" : "internal.error", err));

    // --- single-range mode: one resource.read, the raw read result is the answer -----------------
    // Error classing (both modes, same as the attach call above): a DAEMON-side rejection of
    // resource.read is an unknown/expired handle (not-found exit class); a transport or parse
    // failure says nothing about the handle — that is internal.error (exit-table honesty).
    if (range.has_value())
    {
        bool rejected = false;
        const std::optional<Json> res =
            call(client, ++id, "resource.read", range_params(handle_uri, range->first, range->second),
                 err, &rejected);
        if (!res.has_value())
            return finish(Envelope::failure(
                rejected ? "resource.unknown_handle" : "internal.error", err));
        Json data = res->contains("data") ? res->at("data") : Json::object();
        return finish(Envelope::success(std::move(data)));
    }

    // --- full fetch: chunk loop, hex-decode, byte-exact reassembly -------------------------------
    std::string payload;
    // sizeBytes comes from the CLIENT-parsed URI (untrusted input): clamp the up-front reservation
    // to one chunk — growth past that is amortized — so a doctored ?bytes= cannot throw
    // length_error/bad_alloc out of the documented no-throw contract. The daemon-declared totals
    // then bound the loop below.
    payload.reserve(static_cast<std::size_t>(
        std::min(handle->size_bytes, editor::bridge::kResourceReadMaxChunkBytes)));
    std::uint64_t offset = 0;
    for (;;)
    {
        bool rejected = false; // fresh per call: call() sets it on rejection, never clears it
        const std::optional<Json> res =
            call(client, ++id, "resource.read", range_params(handle_uri, offset, 0), err, &rejected);
        if (!res.has_value())
            return finish(Envelope::failure(
                rejected ? "resource.unknown_handle" : "internal.error", err));
        const Json& data = res->at("data");
        const std::optional<std::string> bytes =
            editor::bridge::hex_decode(data.at("chunkHex").as_string());
        if (!bytes.has_value())
            return finish(Envelope::failure("internal.error",
                                            "daemon returned a malformed chunk encoding"));
        payload += *bytes;
        offset += bytes->size();
        if (data.at("eof").as_bool())
            break;
        if (bytes->empty())
            return finish(Envelope::failure("internal.error",
                                            "daemon returned an empty non-eof chunk (no progress)"));
        // Bound reassembly by the handle's declared size: a stream that exceeds it without eof can
        // only be a doctored handle or a misbehaving daemon — fail instead of growing unbounded
        // (the post-loop equality check could otherwise only fire after arbitrary allocation).
        if (payload.size() > handle->size_bytes)
            return finish(Envelope::failure(
                "internal.error", "daemon streamed past the handle's declared sizeBytes (" +
                                      std::to_string(handle->size_bytes) + ") without eof"));
    }

    if (payload.size() != handle->size_bytes)
        return finish(Envelope::failure(
            "internal.error", "reassembled size " + std::to_string(payload.size()) +
                                  " != handle sizeBytes " + std::to_string(handle->size_bytes)));

    // The spooled payload IS the original result envelope JSON — return it parsed, so `context
    // fetch --json` emits exactly what the oversized verb would have printed inline. Fall back to
    // a raw carrier only if the payload is (unexpectedly) not JSON.
    Json fetched;
    try
    {
        fetched = Json::parse(payload);
    }
    catch (const std::exception&)
    {
        Json data = Json::object();
        data.set("handle", Json(handle_uri));
        data.set("sizeBytes", Json(static_cast<std::uint64_t>(payload.size())));
        data.set("payloadHex", Json(editor::bridge::hex_encode(payload)));
        return finish(Envelope::success(std::move(data)));
    }
    return finish(Envelope::success(std::move(fetched)));
}

} // namespace context::cli
