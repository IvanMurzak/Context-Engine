// Expected-writes table — self-echo suppression for daemon-initiated writes (R-FILE-002).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace context::editor::filesync
{

// The daemon registers (path, resulting content-hash) for each of its OWN writes so the watcher
// self-echo-suppresses them instead of re-processing them as external edits (which would make the
// daemon chase its own tail). Entries expire on a short TTL / one debounce cycle, so a self-echo
// suppression can NEVER mask a genuine LATER external edit of the same path (owner-emphasized
// invariant): once the entry expires or is consumed, the next observation of that path reconciles
// normally.
class ExpectedWrites
{
public:
    explicit ExpectedWrites(std::uint64_t ttl_nanos) noexcept : ttl_nanos_(ttl_nanos) {}

    void register_write(std::string_view path, std::uint64_t content_hash, std::uint64_t now_nanos);

    // True iff a still-live entry matches (path, content_hash): this observed change is our own echo.
    // A match CONSUMES the entry (one echo suppresses one re-hash), so a coincidental later external
    // write producing the same bytes is not perpetually suppressed. Expired entries never match.
    [[nodiscard]] bool is_self_echo(std::string_view path, std::uint64_t content_hash,
                                    std::uint64_t now_nanos);

    // Drop every entry whose TTL has elapsed at `now_nanos`.
    void expire(std::uint64_t now_nanos);

    [[nodiscard]] std::size_t size() const noexcept { return table_.size(); }

private:
    struct Entry
    {
        std::uint64_t content_hash = 0;
        std::uint64_t expiry_nanos = 0;
    };

    std::unordered_map<std::string, Entry> table_;
    std::uint64_t ttl_nanos_;
};

} // namespace context::editor::filesync
