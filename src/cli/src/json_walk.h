// Shared candidate-file walk for the JSON-tree CLI verbs (`context migrate`, `context validate`).
// Both verbs operate over "the target file, or every *.json beneath a target directory (skipping
// dot-dirs), sorted"; this is the ONE implementation of that rule so the two cannot drift (M2 polish
// audit, issue #70). Header-only + internal to the cli module (mirrors merge/src/pointer_format.h).

#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace context::cli
{

// Collect the candidate files under `target`: the target itself when it is a regular file, else every
// *.json under it (recursive), skipping dot-directories (.editor control state, .git, …). Sorted so
// the report (and any failure) is deterministic. Callers pre-check that `target` exists.
[[nodiscard]] inline std::vector<std::filesystem::path>
collect_json_candidates(const std::filesystem::path& target)
{
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    if (fs::is_regular_file(target))
    {
        files.push_back(target);
        return files;
    }
    std::error_code ec;
    fs::recursive_directory_iterator it(target, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end)
    {
        const fs::directory_entry& entry = *it;
        const std::string name = entry.path().filename().string();
        if (entry.is_directory(ec))
        {
            if (!name.empty() && name[0] == '.')
                it.disable_recursion_pending(); // never descend into dot-dirs (.editor, .git)
        }
        else if (entry.is_regular_file(ec) && name.size() > 5 &&
                 name.compare(name.size() - 5, 5, ".json") == 0)
        {
            files.push_back(entry.path());
        }
        it.increment(ec);
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace context::cli
