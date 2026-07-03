// Platform seam implementation: minimal concrete Clock/FileSystem impls (R-HEAD-001, R-QA-010).

#include "context/kernel/platform.h"

#include <chrono>
#include <string>

namespace context::kernel
{

std::uint64_t SteadyClock::now_nanos() const
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

bool MemoryFileSystem::exists(std::string_view path) const
{
    return files_.find(std::string{path}) != files_.end();
}

std::optional<std::string> MemoryFileSystem::read(std::string_view path) const
{
    auto it = files_.find(std::string{path});
    if (it == files_.end())
        return std::nullopt;
    return it->second;
}

bool MemoryFileSystem::write(std::string_view path, std::string_view data)
{
    files_[std::string{path}] = std::string{data};
    return true;
}

bool MemoryFileSystem::remove(std::string_view path)
{
    return files_.erase(std::string{path}) != 0;
}

} // namespace context::kernel
