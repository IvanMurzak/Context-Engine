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

// Parse a canonical RFC 6901 array-index token to a size_t. Returns false for a non-index token
// (empty, non-digit, or a leading zero other than "0" itself) OR one long enough to overflow — the
// length cap keeps a pathological digit-run away from std::stoull, which would otherwise throw an
// uncaught std::out_of_range. 10 digits addresses billions of elements, past any in-memory document,
// so a longer token is simply a non-resolving index (mirrors compose::parse_array_index).
inline bool parse_array_index(const std::string& token, std::size_t& out)
{
    if (token.empty() || token.size() > 10 ||
        token.find_first_not_of("0123456789") != std::string::npos ||
        (token.size() > 1 && token[0] == '0'))
        return false;
    out = static_cast<std::size_t>(std::stoull(token));
    return true;
}

} // namespace context::editor::merge::detail
