// Path normalization + project-root jail implementation.

#include "context/editor/filesync/path_jail.h"

#include <cstddef>
#include <vector>

namespace context::editor::filesync
{
namespace
{

// Detect (and strip) a leading root anchor: a POSIX "/" or a Windows drive "X:". Returns the anchor
// text (empty for a relative path) and advances `start` past it.
[[nodiscard]] std::string take_anchor(const std::string& unified, std::size_t& start)
{
    if (!unified.empty() && unified[0] == '/')
    {
        start = 1;
        return "/";
    }
    if (unified.size() >= 2 && unified[1] == ':')
    {
        const char drive = unified[0];
        const bool is_letter = (drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z');
        if (is_letter)
        {
            // Absolute drive path "X:/..." — skip the optional separator after the colon.
            start = (unified.size() >= 3 && unified[2] == '/') ? 3 : 2;
            return std::string{drive} + ":/";
        }
    }
    start = 0;
    return "";
}

} // namespace

std::string normalize_path(std::string_view path)
{
    std::string unified{path};
    for (char& ch : unified)
    {
        if (ch == '\\')
            ch = '/';
    }

    std::size_t start = 0;
    const std::string anchor = take_anchor(unified, start);
    const bool absolute = !anchor.empty();

    // Split on '/', folding '.', empty, and '..' segments as we go.
    std::vector<std::string> segments;
    std::string segment;
    for (std::size_t j = start; j <= unified.size(); ++j)
    {
        if (j == unified.size() || unified[j] == '/')
        {
            if (segment == "." || segment.empty())
            {
                // skip
            }
            else if (segment == "..")
            {
                if (!segments.empty() && segments.back() != "..")
                    segments.pop_back();
                else if (!absolute)
                    segments.push_back("..");
                // absolute + already at root: '..' is a no-op (cannot escape the anchor)
            }
            else
            {
                segments.push_back(segment);
            }
            segment.clear();
        }
        else
        {
            segment.push_back(unified[j]);
        }
    }

    std::string result = anchor;
    for (std::size_t k = 0; k < segments.size(); ++k)
    {
        if (k != 0)
            result.push_back('/');
        result += segments[k];
    }
    return result;
}

bool is_inside_jail(std::string_view root, std::string_view path)
{
    const std::string root_norm = normalize_path(root);
    const std::string path_norm = normalize_path(path);

    if (path_norm == root_norm)
        return true;
    if (root_norm.empty())
    {
        // Root normalizes to empty (root was "" or "." — the jail is the current directory). A path is
        // inside iff it is a relative path that does not ascend past the root. An ANCHORED path (a POSIX
        // "/..." or a Windows "X:/...", detected the same way take_anchor() does) can never sit under a
        // relative root, so it escapes; a ".."-leading path escapes too.
        if (path_norm.empty())
            return false;
        const bool anchored =
            path_norm[0] == '/' || (path_norm.size() >= 2 && path_norm[1] == ':');
        if (anchored)
            return false;
        return path_norm.rfind("..", 0) != 0;
    }

    // Beneath the root iff it starts with "<root>/".
    const std::string prefix = root_norm + "/";
    return path_norm.rfind(prefix, 0) == 0;
}

} // namespace context::editor::filesync
