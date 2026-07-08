// TS-resolved stack traces (R-OBS-005). STL-only, no V8 — a LOCAL gate. See stack_trace.h.

#include "context/runtime/ts/stack_trace.h"

#include <cstddef>
#include <optional>

namespace context::runtime::ts
{
namespace
{

// Parse a trailing ":<line>:<column>" off `loc`, returning the file part and the two 1-based
// numbers. Returns false when `loc` does not end in the ":<int>:<int>" shape (e.g. a native frame
// "[native code]"). Windows-safe: a drive-letter "C:\...:12:34" keeps the last two ':'-groups as
// line/column and everything before as the file.
bool splitLocation(std::string_view loc, std::string& file, std::uint32_t& line,
                   std::uint32_t& column)
{
    // Find the last two ':' separators.
    const std::size_t lastColon = loc.rfind(':');
    if (lastColon == std::string_view::npos || lastColon == 0)
    {
        return false;
    }
    const std::size_t prevColon = loc.rfind(':', lastColon - 1);
    if (prevColon == std::string_view::npos)
    {
        return false;
    }

    const std::string_view fileView = loc.substr(0, prevColon);
    const std::string_view lineView = loc.substr(prevColon + 1, lastColon - prevColon - 1);
    const std::string_view colView = loc.substr(lastColon + 1);
    if (fileView.empty() || lineView.empty() || colView.empty())
    {
        return false;
    }

    const auto toUint = [](std::string_view sv, std::uint32_t& out) {
        std::uint64_t v = 0;
        for (const char c : sv)
        {
            if (c < '0' || c > '9')
            {
                return false;
            }
            v = v * 10 + static_cast<std::uint64_t>(c - '0');
            if (v > 0xFFFFFFFFULL)
            {
                return false;
            }
        }
        out = static_cast<std::uint32_t>(v);
        return true;
    };

    std::uint32_t l = 0;
    std::uint32_t c = 0;
    if (!toUint(lineView, l) || !toUint(colView, c))
    {
        return false;
    }
    file.assign(fileView);
    line = l;
    column = c;
    return true;
}

// Trim ASCII whitespace from both ends.
std::string_view trim(std::string_view s)
{
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
    {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
    {
        --e;
    }
    return s.substr(b, e - b);
}

// Parse one "at ..." frame body (the text after "at "). Fills `frame`; returns false when the body
// is not a recognisable "<fn> (<loc>)" or bare "<loc>" frame.
bool parseFrameBody(std::string_view body, StackFrame& frame)
{
    body = trim(body);
    if (body.empty())
    {
        return false;
    }

    // "<function> (<location>)" — the location is the parenthesised tail. Split at the FIRST '(' that
    // opens that tail (the boundary after the function label), NOT the last: a file path may itself
    // contain '(' (e.g. a Windows folder "me (dev)"), and rfind would truncate the file to the
    // innermost parenthesis.
    if (body.back() == ')')
    {
        const std::size_t open = body.find('(');
        if (open == std::string_view::npos)
        {
            return false;
        }
        const std::string_view fn = trim(body.substr(0, open));
        const std::string_view loc = body.substr(open + 1, body.size() - open - 2);
        std::string file;
        std::uint32_t line = 0;
        std::uint32_t column = 0;
        if (!splitLocation(loc, file, line, column))
        {
            return false;
        }
        frame.function.assign(fn);
        frame.file = std::move(file);
        frame.line = line;
        frame.column = column;
        frame.resolved = false;
        return true;
    }

    // Bare "<location>" (no function label).
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    if (!splitLocation(body, file, line, column))
    {
        return false;
    }
    frame.function.clear();
    frame.file = std::move(file);
    frame.line = line;
    frame.column = column;
    frame.resolved = false;
    return true;
}

} // namespace

std::vector<StackFrame> parse_v8_stack(std::string_view stack)
{
    std::vector<StackFrame> frames;
    std::size_t pos = 0;
    while (pos <= stack.size())
    {
        std::size_t nl = stack.find('\n', pos);
        const std::string_view rawLine =
            stack.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        const std::string_view lineText = trim(rawLine);

        // A frame line starts with "at " (after trimming the indent). Everything else — the header
        // line, "async" wrappers we do not recognise — is skipped.
        if (lineText.size() > 3 && lineText.substr(0, 3) == "at ")
        {
            StackFrame frame;
            if (parseFrameBody(lineText.substr(3), frame))
            {
                frames.push_back(std::move(frame));
            }
        }

        if (nl == std::string_view::npos)
        {
            break;
        }
        pos = nl + 1;
    }
    return frames;
}

void remap_stack(std::vector<StackFrame>& frames, const SourceMap& map)
{
    for (StackFrame& frame : frames)
    {
        if (frame.line == 0 || frame.column == 0)
        {
            continue; // 1-based positions; 0 cannot be a real frame position
        }
        const std::optional<OriginalPosition> orig =
            map.resolve(frame.line - 1, frame.column - 1);
        if (!orig.has_value())
        {
            continue;
        }
        frame.file = orig->source;
        frame.line = orig->line + 1;
        frame.column = orig->column + 1;
        if (frame.function.empty() && !orig->name.empty())
        {
            frame.function = orig->name;
        }
        frame.resolved = true;
    }
}

std::string render_stack(const std::vector<StackFrame>& frames, std::string_view message)
{
    std::string out;
    if (!message.empty())
    {
        out.append(message);
        out.push_back('\n');
    }
    for (const StackFrame& frame : frames)
    {
        out.append("    at ");
        if (!frame.function.empty())
        {
            out.append(frame.function);
            out.append(" (");
            out.append(frame.file);
            out.push_back(':');
            out.append(std::to_string(frame.line));
            out.push_back(':');
            out.append(std::to_string(frame.column));
            out.push_back(')');
        }
        else
        {
            out.append(frame.file);
            out.push_back(':');
            out.append(std::to_string(frame.line));
            out.push_back(':');
            out.append(std::to_string(frame.column));
        }
        out.push_back('\n');
    }
    // Trim the trailing newline so the trace has no dangling blank line.
    if (!out.empty() && out.back() == '\n')
    {
        out.pop_back();
    }
    return out;
}

std::string resolve_ts_stack(std::string_view v8Stack, const SourceMap& map,
                             std::string_view message)
{
    std::vector<StackFrame> frames = parse_v8_stack(v8Stack);
    remap_stack(frames, map);
    return render_stack(frames, message);
}

} // namespace context::runtime::ts
