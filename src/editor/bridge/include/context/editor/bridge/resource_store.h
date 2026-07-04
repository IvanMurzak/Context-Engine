// R-CLI-017 large-result resource store: the daemon-side spool + same-filesystem resolver behind
// the transport frame cap (R-BRIDGE-007).
//
// An RPC response whose serialized size exceeds the spool threshold does not travel inline (the
// frame cap in transport.h bounds a single frame; a huge inline result would also make every client
// buffer it whole). Instead the composing layer (KernelServer::finalize_response) spools the
// oversized RESULT here and replaces it with a SMALL `largeResult` envelope carrying the
// contract-side opaque handle (contract/resource_handle.h). The client then fetches the payload
// over the SAME channel via `resource.read { handle, range }` in bounded chunks (CLI:
// `context fetch`).
//
// v1 scope (R-CLI-017): same-filesystem fetch only — handles resolve against THIS live daemon
// instance (the store's instance id is the daemon's incarnation id, so a restart invalidates every
// previous handle by construction). Spool files live under the REAL `.editor/resources/` control
// dir (outside the reconcile crawl root), which doubles as the sanctioned same-FS `localPath` fast
// path a co-located client MAY read directly; the wire fetch stays the portable mechanism. Chunks
// are HEX-encoded inside the JSON result: binary-clean, byte-range-exact (a raw slice of UTF-8 text
// could split a code point and break JSON encoding), and bounded by kResourceReadMaxChunkBytes so a
// chunk always fits the frame cap with room to spare.

#pragma once

#include "context/editor/contract/resource_handle.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace context::editor::bridge
{

// The default spool policy: a serialized RPC response larger than this returns a handle instead of
// inline data. Well under transport.h's kMaxFrameBytes (64 MiB) so the DEFAULT inline path never
// even approaches the frame cap. Operational policy, not wire shape (describe advertises shapes).
inline constexpr std::uint64_t kLargeResultThresholdBytes = 4u * 1024u * 1024u;

// The largest DECODED byte count one resource.read returns. Hex doubles it on the wire (16 MiB),
// still comfortably inside the 64 MiB frame cap.
inline constexpr std::uint64_t kResourceReadMaxChunkBytes = 8u * 1024u * 1024u;

// Lowercase hex codec for the chunk payloads (and their tests / client-side reassembly).
[[nodiscard]] std::string hex_encode(std::string_view bytes);
[[nodiscard]] std::optional<std::string> hex_decode(std::string_view hex);

class ResourceStore
{
public:
    // `dir` — the REAL on-disk spool directory (created on demand; any residue from a previous
    // incarnation is best-effort cleared, since its handles are invalid by construction).
    // `instance_id` — the minting daemon's incarnation id, embedded in every handle URI.
    ResourceStore(std::filesystem::path dir, std::string instance_id);

    // Spool one oversized payload; returns the transport-portable handle. nullopt on I/O failure
    // (the caller then falls back to the inline response).
    [[nodiscard]] std::optional<contract::ResourceHandle> put(std::string_view payload);

    struct ReadResult
    {
        std::uint64_t offset = 0; // where this chunk starts
        std::string bytes;        // the DECODED chunk (<= min(requested, kResourceReadMaxChunkBytes))
        std::uint64_t total = 0;  // total payload size
        bool eof = false;         // offset + bytes.size() == total
    };

    // Resolve a handle minted by THIS store and read [offset, offset+max_len). `max_len` == 0 means
    // "up to the chunk cap". nullopt when the handle is foreign (wrong instance), unknown, its
    // spool file is gone, or offset > total. offset == total yields an empty eof chunk.
    [[nodiscard]] std::optional<ReadResult> read(const contract::ResourceHandle& handle,
                                                 std::uint64_t offset, std::uint64_t max_len) const;

    // The same-filesystem fast-path hint for a handle minted by put(): the spool file's absolute
    // path (generic form). Empty when unknown.
    [[nodiscard]] std::string local_path_hint(const contract::ResourceHandle& handle) const;

    [[nodiscard]] const std::string& instance_id() const noexcept { return instance_id_; }
    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_; }

private:
    [[nodiscard]] std::filesystem::path spool_path(std::uint64_t payload_id) const;

    std::filesystem::path dir_;
    std::string instance_id_;
    std::uint64_t next_payload_ = 0;
    std::map<std::uint64_t, std::uint64_t> sizes_; // payload id -> total bytes (this incarnation)
};

} // namespace context::editor::bridge
