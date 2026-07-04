// R-CLI-017 transport-portable large-result handle: the contract-side opaque resource-URI value type.
//
// An oversized RPC response does not travel inline: the daemon spools the payload and returns a
// SMALL `largeResult` envelope carrying this handle, which the client fetches over the SAME channel
// via `resource.read { handle, range }` (CLI alias: `context fetch`). The URI is OPAQUE to clients —
// only the daemon that minted it resolves it. The FORMAT and the verb are contract from day one (the
// wire shape cannot be retrofitted); v1 implements same-filesystem fetch only (the URI resolves
// against the local daemon — bridge/resource_store.h), and the sibling `localPath` field on the
// JSON shape is the sanctioned same-FS optimization: never the sole mechanism, so a remote or
// cross-container client still retrieves the result portably once the remote door (R-BRIDGE-007)
// lands. Handle (one oversized payload) and cursor (a paged sequence, R-CLI-012/R-BRIDGE-008)
// reconcile as ONE large-result contract.

#pragma once

#include "context/editor/contract/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace context::editor::contract
{

// The URI scheme every handle uses: "context-res://v0/<instanceId>/<payloadId>?bytes=<n>".
inline constexpr std::string_view kResourceUriScheme = "context-res";

struct ResourceHandle
{
    std::string instance_id;      // the daemon incarnation that minted the handle (epoch identity:
                                  // a restart invalidates every previous handle by construction)
    std::uint64_t payload_id = 0; // per-instance monotonic payload number
    std::uint64_t size_bytes = 0; // total payload size in bytes

    // Serialize as the opaque resource URI. Deterministic; parse() round-trips it exactly.
    [[nodiscard]] std::string to_uri() const;

    // Parse an opaque resource URI. nullopt on anything malformed (wrong scheme/version, an
    // instance id outside [A-Za-z0-9._-], a non-numeric payload id / byte count) — a client-supplied
    // string is untrusted input, so parsing is strict rather than forgiving.
    [[nodiscard]] static std::optional<ResourceHandle> parse(std::string_view uri);

    // The JSON shape a `largeResult` envelope carries:
    //   { "handle": "<uri>", "sizeBytes": N, "localPath": "<abs path>"? }
    // `local_path_hint` (optional) is the same-filesystem fast-path hint — the REAL on-disk spool
    // location, valid only while the minting daemon is alive and only for a same-FS client.
    [[nodiscard]] Json to_json(const std::string& local_path_hint = std::string()) const;
};

// The describe-advertised shape of the whole large-result mechanism (R-CLI-013): URI scheme, the
// fetch verb/method/CLI naming, the chunk encoding, and the field shapes of the handle object and
// the resource.read result. Registry::describe() embeds this as contract.largeResult.
[[nodiscard]] Json large_result_descriptor();

} // namespace context::editor::contract
