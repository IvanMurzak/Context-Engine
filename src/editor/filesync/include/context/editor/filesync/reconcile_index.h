// The persisted reconcile index (gitignored `.editor/index`) — R-FILE-002.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace context::editor::filesync
{

class FileStore;

// One index row: the last-known size, mtime, and content-hash of a file.
struct IndexEntry
{
    std::uint64_t size = 0;
    std::uint64_t mtime_nanos = 0;
    std::uint64_t content_hash = 0;
};

// path -> (size, mtime, content-hash). Its ONLY purpose is to make warm attach across daemon restarts
// mtime/size-gated instead of a cold full re-hash (R-FILE-002); it is fully rebuildable from disk at
// any time. Backed by std::map so serialization is deterministic (sorted by path).
class ReconcileIndex
{
public:
    [[nodiscard]] std::optional<IndexEntry> get(std::string_view path) const;
    void put(std::string_view path, IndexEntry entry);
    void erase(std::string_view path);
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] const std::map<std::string, IndexEntry>& entries() const noexcept
    {
        return entries_;
    }

    // Deterministic text form: one "size mtime hash path" line per entry (path last so it may contain
    // spaces). Round-trips through deserialize().
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static ReconcileIndex deserialize(std::string_view text);

    // Persist atomically (temp+rename) to `path`; load it back (missing/corrupt file => empty index,
    // which is safe — the index is always rebuildable).
    bool save(FileStore& fs, std::string_view path) const;
    [[nodiscard]] static ReconcileIndex load(FileStore& fs, std::string_view path);

private:
    std::map<std::string, IndexEntry> entries_;
};

} // namespace context::editor::filesync
