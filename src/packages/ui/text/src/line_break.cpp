// UAX #14 line breaking + greedy word wrap (see line_break.h). libunibreak supplies per-position break
// classes; wrap() shapes candidate lines with measure() and greedily fills to a width. libunibreak lives
// ONLY here so the public header stays dependency-free.

#include "context/packages/ui/text/line_break.h"

extern "C"
{
#include <linebreak.h>
}

#include <string>
#include <utility>

namespace context::packages::ui::text
{
namespace
{

// libunibreak break classes at each byte: MUSTBREAK=0, ALLOWBREAK=1, NOBREAK=2, INSIDEACHAR=3. brks[i]
// classifies the opportunity AFTER the character whose last byte is i.
std::string classify(std::string_view utf8)
{
    std::string brks(utf8.size(), static_cast<char>(LINEBREAK_NOBREAK));
    if (!utf8.empty())
        set_linebreaks_utf8(reinterpret_cast<const utf8_t*>(utf8.data()), utf8.size(), "en",
                            brks.data());
    return brks;
}

// Drop trailing ASCII whitespace from [begin,end): a soft break keeps its space on the previous line, but
// that space "hangs" past the margin — it does not count toward the line's content width, and rendering
// it is invisible. So both the fit decision AND the emitted line use the trimmed extent.
std::size_t trim_end(std::string_view utf8, std::size_t begin, std::size_t end)
{
    while (end > begin)
    {
        const char c = utf8[end - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            --end;
        else
            break;
    }
    return end;
}

// The total advance width of a shaped substring [begin,end) of `utf8`, trailing whitespace hung (trimmed).
float shaped_width(const FontFace& font, std::string_view utf8, std::size_t begin, std::size_t end,
                   float pixel_size)
{
    end = trim_end(utf8, begin, end);
    const std::vector<ShapedRun> runs = measure(font, utf8.substr(begin, end - begin), pixel_size);
    float w = 0.0f;
    for (const ShapedRun& r : runs)
        w += r.width;
    return w;
}

TextLine make_line(const FontFace& font, std::string_view utf8, std::size_t begin, std::size_t end,
                   float pixel_size)
{
    end = trim_end(utf8, begin, end);
    TextLine line;
    line.runs = measure(font, utf8.substr(begin, end - begin), pixel_size);
    for (const ShapedRun& r : line.runs)
        line.width += r.width;
    return line;
}

} // namespace

std::vector<std::size_t> break_opportunities(std::string_view utf8)
{
    const std::string brks = classify(utf8);
    std::vector<std::size_t> out;
    // A break AFTER byte i is a start opportunity at i+1; skip the final position (end of string).
    for (std::size_t i = 0; i + 1 < utf8.size(); ++i)
    {
        const char c = brks[i];
        if (c == static_cast<char>(LINEBREAK_MUSTBREAK) || c == static_cast<char>(LINEBREAK_ALLOWBREAK))
            out.push_back(i + 1);
    }
    return out;
}

std::vector<TextLine> wrap(const FontFace& font, std::string_view utf8, float pixel_size, float max_width)
{
    std::vector<TextLine> lines;
    if (utf8.empty())
    {
        lines.push_back(make_line(font, utf8, 0, 0, pixel_size));
        return lines;
    }
    if (max_width <= 0.0f)
    {
        lines.push_back(make_line(font, utf8, 0, utf8.size(), pixel_size));
        return lines;
    }

    const std::string brks = classify(utf8);

    // Candidate break points = end offsets of "words" (the byte AFTER an ALLOW/MUST opportunity), plus a
    // final forced point at size. Each carries whether the opportunity is MANDATORY.
    std::vector<std::pair<std::size_t, bool>> points; // (end offset, mandatory)
    for (std::size_t i = 0; i + 1 < utf8.size(); ++i)
    {
        const char c = brks[i];
        if (c == static_cast<char>(LINEBREAK_MUSTBREAK))
            points.emplace_back(i + 1, true);
        else if (c == static_cast<char>(LINEBREAK_ALLOWBREAK))
            points.emplace_back(i + 1, false);
    }
    points.emplace_back(utf8.size(), true); // end of text always terminates the last line

    std::size_t line_start = 0;
    std::size_t cur_end = 0; // exclusive end of the words accumulated on the current line
    for (const auto& [we, mandatory] : points)
    {
        if (we <= line_start)
            continue;
        const bool line_empty = (cur_end == line_start);
        const float w = shaped_width(font, utf8, line_start, we, pixel_size);
        if (line_empty || w <= max_width)
        {
            cur_end = we; // fits (or the line must take at least this first word even if it overflows)
        }
        else
        {
            // Adding this word overflows: flush the accumulated line, then start a new one at this word.
            lines.push_back(make_line(font, utf8, line_start, cur_end, pixel_size));
            line_start = cur_end;
            cur_end = we; // the new line takes this word (may itself overflow — no mid-word break)
        }
        if (mandatory && cur_end > line_start)
        {
            lines.push_back(make_line(font, utf8, line_start, cur_end, pixel_size));
            line_start = cur_end;
        }
    }
    if (cur_end > line_start)
        lines.push_back(make_line(font, utf8, line_start, cur_end, pixel_size));
    if (lines.empty())
        lines.push_back(make_line(font, utf8, 0, utf8.size(), pixel_size));
    return lines;
}

} // namespace context::packages::ui::text
