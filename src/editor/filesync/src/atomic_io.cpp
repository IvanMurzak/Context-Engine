// Atomic write-temp-then-rename implementation.

#include "context/editor/filesync/atomic_io.h"

#include "context/editor/filesync/file_store.h"

namespace context::editor::filesync
{

std::string atomic_temp_path(std::string_view path, std::string_view unique)
{
    std::string temp{path};
    temp += ".tmp";
    if (!unique.empty())
    {
        temp.push_back('.');
        temp += unique;
    }
    return temp;
}

bool atomic_write(FileStore& fs, std::string_view path, std::string_view data,
                  std::string_view unique)
{
    const std::string temp = atomic_temp_path(path, unique);

    if (!fs.write(temp, data))
        return false;
    fs.fsync(temp); // the new bytes are durable in the temp file before it becomes visible.

    // The rename is the atomic commit point. It may throw SimulatedCrash (crash between temp-write and
    // rename); the caller then sees `path` still holding its previous content — never a torn write.
    if (!fs.rename(temp, path))
    {
        fs.remove(temp);
        return false;
    }
    fs.fsync(path);
    return true;
}

} // namespace context::editor::filesync
