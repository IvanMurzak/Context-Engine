// Persisted reconcile index implementation.

#include "context/editor/filesync/reconcile_index.h"

#include "context/editor/filesync/atomic_io.h"
#include "context/editor/filesync/file_store.h"

#include <sstream>

namespace context::editor::filesync
{

std::optional<IndexEntry> ReconcileIndex::get(std::string_view path) const
{
    auto it = entries_.find(std::string{path});
    if (it == entries_.end())
        return std::nullopt;
    return it->second;
}

void ReconcileIndex::put(std::string_view path, IndexEntry entry)
{
    entries_[std::string{path}] = entry;
}

void ReconcileIndex::erase(std::string_view path)
{
    entries_.erase(std::string{path});
}

std::string ReconcileIndex::serialize() const
{
    std::ostringstream out;
    for (const auto& [path, entry] : entries_)
        out << entry.size << ' ' << entry.mtime_nanos << ' ' << entry.content_hash << ' ' << path
            << '\n';
    return out.str();
}

ReconcileIndex ReconcileIndex::deserialize(std::string_view text)
{
    ReconcileIndex index;
    std::istringstream in{std::string{text}};
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        const std::size_t p1 = line.find(' ');
        const std::size_t p2 = (p1 == std::string::npos) ? p1 : line.find(' ', p1 + 1);
        const std::size_t p3 = (p2 == std::string::npos) ? p2 : line.find(' ', p2 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos)
            continue; // malformed line — a rebuildable index tolerates it by dropping the row.

        IndexEntry entry;
        try
        {
            entry.size = std::stoull(line.substr(0, p1));
            entry.mtime_nanos = std::stoull(line.substr(p1 + 1, p2 - p1 - 1));
            entry.content_hash = std::stoull(line.substr(p2 + 1, p3 - p2 - 1));
        }
        catch (const std::exception&)
        {
            continue;
        }
        index.entries_[line.substr(p3 + 1)] = entry;
    }
    return index;
}

bool ReconcileIndex::save(FileStore& fs, std::string_view path) const
{
    return atomic_write(fs, path, serialize(), "index");
}

ReconcileIndex ReconcileIndex::load(FileStore& fs, std::string_view path)
{
    const std::optional<std::string> text = fs.read(path);
    if (!text)
        return ReconcileIndex{};
    return deserialize(*text);
}

} // namespace context::editor::filesync
