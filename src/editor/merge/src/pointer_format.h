// Internal (non-public): RFC 6901 pointer FORMATTING, shared by the merge module's .cpp files.
// compose/json_pointer.h owns pointer PARSING + resolution; this is the write-side escaper the merge
// engine and the re-key/validate walkers use to build the pointers they emit.

#pragma once

#include <cstddef>
#include <string>

namespace context::editor::merge::detail
{

// Append one reference token, escaping '~' -> '~0' and '/' -> '~1' (RFC 6901).
inline std::string append_token(const std::string& pointer, const std::string& token)
{
    std::string out = pointer;
    out.push_back('/');
    for (char ch : token)
    {
        if (ch == '~')
            out += "~0";
        else if (ch == '/')
            out += "~1";
        else
            out.push_back(ch);
    }
    return out;
}

// Append an array index token.
inline std::string append_index(const std::string& pointer, std::size_t index)
{
    return pointer + "/" + std::to_string(index);
}

} // namespace context::editor::merge::detail
