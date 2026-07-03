// In-memory fault-injectable FileStore implementation.

#include "context/editor/filesync/file_store.h"

#include "context/editor/filesync/path_jail.h"

#include <algorithm>

namespace context::editor::filesync
{

bool MemoryFileStore::exists(std::string_view path) const
{
    return files_.find(normalize_path(path)) != files_.end();
}

std::optional<std::string> MemoryFileStore::read(std::string_view path) const
{
    auto it = files_.find(normalize_path(path));
    if (it == files_.end())
        return std::nullopt;
    return it->second.data;
}

std::optional<FileStat> MemoryFileStore::stat(std::string_view path) const
{
    auto it = files_.find(normalize_path(path));
    if (it == files_.end())
        return std::nullopt;
    return FileStat{static_cast<std::uint64_t>(it->second.data.size()), it->second.mtime};
}

std::vector<std::string> MemoryFileStore::list(std::string_view dir) const
{
    const std::string root = normalize_path(dir);
    const std::string prefix = root + "/";
    std::vector<std::string> out;
    for (const auto& [path, node] : files_)
    {
        if (path == root || path.rfind(prefix, 0) == 0)
            out.push_back(path);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool MemoryFileStore::write(std::string_view path, std::string_view data)
{
    const std::string key = normalize_path(path);
    if (crash_write_path_ && *crash_write_path_ == key)
    {
        crash_write_path_.reset();
        throw SimulatedCrash("write " + key);
    }
    Node& node = files_[key];
    node.data.assign(data);
    node.mtime = next_mtime_++;
    return true;
}

bool MemoryFileStore::rename(std::string_view from, std::string_view to)
{
    const std::string dst = normalize_path(to);
    if (crash_rename_dest_ && *crash_rename_dest_ == dst)
    {
        crash_rename_dest_.reset();
        throw SimulatedCrash("rename to " + dst);
    }
    const std::string src = normalize_path(from);
    auto it = files_.find(src);
    if (it == files_.end())
        return false;
    Node moved = it->second;
    moved.mtime = next_mtime_++;
    files_.erase(it);
    files_[dst] = std::move(moved);
    return true;
}

bool MemoryFileStore::remove(std::string_view path)
{
    return files_.erase(normalize_path(path)) != 0;
}

void MemoryFileStore::crash_on_rename_to(std::string path)
{
    crash_rename_dest_ = normalize_path(path);
}

void MemoryFileStore::crash_on_write(std::string path)
{
    crash_write_path_ = normalize_path(path);
}

void MemoryFileStore::set_mtime(std::string_view path, std::uint64_t mtime_nanos)
{
    auto it = files_.find(normalize_path(path));
    if (it != files_.end())
        it->second.mtime = mtime_nanos;
}

} // namespace context::editor::filesync
